/* Steering wheels: any wheel Windows lists as a game controller (Moza, Logitech, Thrustmaster, Fanatec, ...) is read
 * through SDL's joystick API and reported to the game the way the console reports a PS4 steering wheel.
 * DriveClub opens a second pad of port type 2 next to the standard one (guest +0xe103d1); when
 * scePadGetControllerInformation gives that pad device class 6 (steering wheel) the game reads it instead of the
 * standard pad, takes the lock from scePadDeviceClassGetExtendedInformation (maxPhysicalWheelAngle) and the wheel
 * from scePadDeviceClassParseData: steering 0..65535 with 32768 the centre, accelerator and brake 0..65535 from
 * released to pressed (guest +0xe10ba0, +0xe10abf, +0xe10b22).
 * The wheel's values travel in the extension bytes of the pad data the game reads, as on the console.
 * Settings are the wheel_* lines of default.ini. Included by runtime_pad.c after runtime_input.inc. */

#define DEVICE_CLASS_STANDARD 0
#define DEVICE_CLASS_STEERING_WHEEL 6
enum { WAX_STEER, WAX_THROTTLE, WAX_BRAKE, WAX_CLUTCH, WAX_HANDBRAKE, WAX_COUNT };
enum { WINV_AUTO, WINV_NO, WINV_YES };
static const char *const wheel_axis_names[WAX_COUNT]={"steering","throttle","brake","clutch","handbrake"};
static struct { int axis, invert; } wheel_axis[WAX_COUNT]={{0,WINV_NO},{1,WINV_AUTO},{2,WINV_AUTO},{-1,WINV_AUTO},{-1,WINV_AUTO}};
static char wheel_device[64]="auto";       /* auto, off, or part of the wheel's name */
static int wheel_rotation=900;              /* degrees lock to lock */
static bool wheel_as_stick, wheel_hat_dpad=true;
static struct { int index; uint8_t out_kind; uint32_t button; int touch_side; } wheel_buttons[32];
static int wheel_button_count;
static bool ffb_enabled=true, ffb_invert;  /* force feedback (runtime_logiwheel.inc) */
static int ffb_gain=100, ffb_vibration=100;   /* percent; above 100 the forces clip at the wheel's maximum */

static SDL_Joystick *wheel;
static SDL_JoystickID wheel_id;
static bool wheel_inverted[WAX_COUNT];      /* decided when the wheel is opened (pedals at rest) */

/* H-pattern shifter: a controller of its own (a Logitech Driving Force Shifter on an adapter, ...) or buttons of the
 * wheel. Each wheel_gear line gives the button held in one gear. The game takes the gear the way a Logitech G29
 * gives its shifter's (guest +0xe132c0): gears 1..6 as the bits 1, 2, 4, 8, 16, 32, reverse 0x80, neutral 0. */
static char shifter_device[64]="auto";      /* auto (a controller named "shifter"), wheel, off, or part of a name */
static struct { int index; uint8_t gear_bits; } shifter_gears[8];
static int shifter_gear_count;
static SDL_Joystick *shifter;

typedef struct { uint16_t steer, throttle, brake, clutch, handbrake; uint8_t gear, magic; } WheelExtension;
#define WHEEL_MAGIC 0x57
_Static_assert(sizeof(WheelExtension)<=sizeof(((PadData *)0)->extension),"wheel data fits the pad extension");
typedef struct {
    int32_t device_class; uint8_t reserved[4];
    uint8_t capability, reserved1; uint16_t max_physical_wheel_angle; uint8_t reserved2[8];
} DeviceClassExtendedInfo;
typedef struct {
    int32_t device_class; uint8_t data_valid, pad[3];
    float steering_wheel_angle; uint16_t steering_wheel, accelerator_pedal, brake_pedal, clutch_pedal, hand_brake;
    uint8_t gear, reserved;
} DeviceClassData;
_Static_assert(sizeof(DeviceClassExtendedInfo)==20,"ScePadDeviceClassExtendedInformation layout");
_Static_assert(sizeof(DeviceClassData)==24,"ScePadDeviceClassData layout");
_Static_assert(__builtin_offsetof(DeviceClassExtendedInfo,max_physical_wheel_angle)==10,"maxPhysicalWheelAngle offset (guest +0xe1027d)");
_Static_assert(__builtin_offsetof(DeviceClassData,steering_wheel)==12,"steeringWheel offset (guest +0xe10bf4)");
_Static_assert(__builtin_offsetof(DeviceClassData,accelerator_pedal)==14,"acceleratorPedal offset (guest +0xe10ade)");

/* wheel_<setting> = <value>; lhs is lower case, rhs as written. */
static void wheel_parse(const char *lhs, char *rhs) {
    const char *key=lhs+6;
    if (!strcmp(key,"device")) { snprintf(wheel_device,sizeof wheel_device,"%s",rhs); return; }
    if (!strcmp(key,"shifter")) { snprintf(shifter_device,sizeof shifter_device,"%s",rhs); return; }
    if (!strcmp(key,"gear")) {
        /* <button number>, <1..6|r> */
        int index=-1; char g[8]="";
        if (sscanf(rhs," %d , %7s",&index,g)!=2 || index<0 || shifter_gear_count>=(int)(sizeof shifter_gears/sizeof *shifter_gears)) return;
        lower(g);
        const int n=atoi(g);
        const uint8_t bits=g[0]=='r' ? 0x80 : n>=1 && n<=6 ? (uint8_t)(1u<<(n-1)) : 0;
        if (!bits) return;
        shifter_gears[shifter_gear_count].index=index; shifter_gears[shifter_gear_count].gear_bits=bits;
        ++shifter_gear_count;
        return;
    }
    if (!strcmp(key,"rotation")) { int v=atoi(rhs); wheel_rotation=v<90 ? 90 : v>3600 ? 3600 : v; return; }
    if (!strcmp(key,"ffb_gain") || !strcmp(key,"ffb_vibration")) {
        int v=atoi(rhs); v=v<0 ? 0 : v>400 ? 400 : v;
        if (!strcmp(key,"ffb_gain")) ffb_gain=v; else ffb_vibration=v;
        return;
    }
    if (!strcmp(key,"as_stick") || !strcmp(key,"hat_dpad") || !strcmp(key,"ffb") || !strcmp(key,"ffb_invert")) {
        lower(rhs);
        const bool on=!strcmp(rhs,"true") || !strcmp(rhs,"on") || !strcmp(rhs,"yes") || !strcmp(rhs,"1");
        if (!strcmp(key,"as_stick")) wheel_as_stick=on;
        else if (!strcmp(key,"hat_dpad")) wheel_hat_dpad=on;
        else if (!strcmp(key,"ffb")) ffb_enabled=on;
        else ffb_invert=on;
        return;
    }
    for (int a=0;a<WAX_COUNT;++a) if (!strcmp(key,wheel_axis_names[a])) {
        /* <axis number>|none [, inverted|normal] */
        char what[16]="", flag[16]="";
        sscanf(rhs," %15[^, ] , %15s",what,flag);
        lower(what); lower(flag);
        if (!strncmp(what,"axis",4)) memmove(what,what+4,strlen(what+4)+1);
        wheel_axis[a].axis=(!*what || !strcmp(what,"none") || !strcmp(what,"off")) ? -1 : atoi(what);
        wheel_axis[a].invert=!strcmp(flag,"inverted") ? WINV_YES : !strcmp(flag,"normal") ? WINV_NO : a==WAX_STEER ? WINV_NO : WINV_AUTO;
        return;
    }
    if (!strcmp(key,"button")) {
        /* <wheel button number>, <PS4 button> */
        int index=-1; char name[32]="";
        if (sscanf(rhs," %d , %31s",&index,name)!=2 || index<0 || wheel_button_count>=(int)(sizeof wheel_buttons/sizeof *wheel_buttons)) return;
        lower(name);
        int slot=wheel_button_count;
        wheel_buttons[slot].index=index;
        if (!strcmp(name,"touchpad") || !strcmp(name,"touchpad_left") || !strcmp(name,"touchpad_right") || !strcmp(name,"touchpad_center")) {
            wheel_buttons[slot].out_kind=OUT_TOUCH;
            wheel_buttons[slot].touch_side=!strcmp(name,"touchpad_right") ? 2 : !strcmp(name,"touchpad_center") ? 1 : 0;
            ++wheel_button_count;
            return;
        }
        for (size_t i=0;i<sizeof(button_names)/sizeof(*button_names);++i) if (!strcmp(name,button_names[i].name)) {
            wheel_buttons[slot].out_kind=OUT_BUTTON; wheel_buttons[slot].button=button_names[i].ps;
            ++wheel_button_count;
            return;
        }
    }
}

#define WHEEL_SECTION_MARKER "# Steering wheel"
static const char wheel_default_config[]=
    "\n" WHEEL_SECTION_MARKER "\n"
    "# Any wheel that Windows lists as a game controller (Moza, Logitech, Thrustmaster, Fanatec...) is shown to the game\n"
    "# as a PS4 steering wheel. The log (logs\\driveclub_*.log) names the wheel that was found and, for the first moves,\n"
    "# the numbers of the axes and buttons you touch (\"Runtime: wheel axis 2 ...\", \"Runtime: wheel button 5\").\n"
    "# wheel_device: auto (a controller whose name says it is a wheel), off, or part of the name the log shows.\n"
    "wheel_device = auto\n"
    "# Lock to lock rotation in degrees: the same as in the wheel's own software (Moza Pit House: steering wheel angle).\n"
    "wheel_rotation = 900\n"
    "# Axis numbers (or none). Add \", inverted\" to turn an axis around. A pedal that reads pressed at rest when the\n"
    "# game starts is turned around by itself; \", normal\" stops that.\n"
    "wheel_steering = 0\n"
    "wheel_throttle = 1\n"
    "wheel_brake = 2\n"
    "wheel_clutch = none\n"
    "wheel_handbrake = none\n"
    "# Wheel buttons: wheel_button = <button number>, <PS4 button>; PS4 buttons as above (cross, r1, options,\n"
    "# pad_up, touchpad_left...). The paddles are usually the first two: shift down is l1, shift up is r1.\n"
    "wheel_button = 0, r1\n"
    "wheel_button = 1, l1\n"
    "# The wheel's hat switch works as the d-pad.\n"
    "wheel_hat_dpad = true\n"
    "# true also moves the left stick and the l2/r2 triggers with the wheel and pedals.\n"
    "wheel_as_stick = false\n"
    "# Force feedback: the game is told a Logitech wheel is connected and its forces go to this wheel.\n"
    "# wheel_ffb = off goes back to a wheel without forces. wheel_ffb_gain: percent of the game's forces (0..400;\n"
    "# above 100 the weaker forces get stronger and the strongest clip at the wheel's maximum).\n"
    "# wheel_ffb_vibration: the same for the road and kerb vibrations, which the game keeps weak (at most 25%).\n"
    "# wheel_ffb_invert = true if the wheel pulls the wrong way.\n"
    "# H-pattern shifter: wheel_shifter = auto (a controller named \"Shifter\"), wheel (its buttons are on the wheel),\n"
    "# off, or part of its name; then one line per gear: wheel_gear = <button number>, <1..6 or r>. WheelSetup.exe\n"
    "# finds the numbers. The game uses the shifter with a manual gearbox chosen in its settings.\n"
    "wheel_shifter = auto\n"
    "wheel_ffb = on\n"
    "wheel_ffb_gain = 100\n"
    "wheel_ffb_vibration = 100\n"
    "wheel_ffb_invert = false\n";

/* A default.ini from before wheel support keeps the player's lines and gets the wheel section added at its end. */
static void wheel_config_append(const char *path) {
    FILE *f=fopen(path,"rb");
    if (!f) return;
    char *text=calloc(1,65536); const size_t n=text ? fread(text,1,65535,f) : 0;
    fclose(f);
    const bool present=!text || strstr(text,"wheel_device")!=NULL;
    const bool newline=n && text[n-1]=='\n';
    free(text);
    if (present || !(f=fopen(path,"ab"))) return;
    if (!newline) fputc('\n',f);
    fwrite(wheel_default_config,1,sizeof(wheel_default_config)-1,f);
    fclose(f);
    printf("Runtime: steering wheel settings added to %s\n",path);
}
static bool wheel_name_matches(const char *name) {
    char n[128]; snprintf(n,sizeof n,"%s",name ? name : ""); lower(n);
    char want[64]; snprintf(want,sizeof want,"%s",wheel_device); lower(want); trim(want);
    if (strcmp(want,"auto")) return *want && strstr(n,want)!=NULL;
    static const char *const words[]={"wheel","moza","racing","fanatec","thrustmaster","simagic","simucube","cammus",
                                      "logitech g2","logitech g9","g29","g920","g923","t300","t248","t150","t500","tx racing"};
    for (size_t i=0;i<sizeof(words)/sizeof(*words);++i) if (strstr(n,words[i])) return true;
    return false;
}
static int axis_raw(SDL_Joystick *j, int axis) {
    return axis>=0 && axis<SDL_GetNumJoystickAxes(j) ? SDL_GetJoystickAxis(j,axis) : -32768;
}
/* The connected wheel, opened on first use and after a reconnect; called under lock. */
static SDL_Joystick *current_wheel(void) {
    if (sdl_ready<=0) return NULL;
    { char d[8]; snprintf(d,sizeof d,"%s",wheel_device); lower(d); trim(d); if (!strcmp(d,"off") || !strcmp(d,"none")) return NULL; }
    if (wheel && !SDL_JoystickConnected(wheel)) {
        printf("Runtime: wheel disconnected\n");
        SDL_CloseJoystick(wheel); wheel=NULL; wheel_id=0;
    }
    if (wheel) return wheel;
    static uint64_t last_scan;
    const uint64_t now=now_us();
    if (last_scan && now-last_scan<1000000) return NULL; /* look for a newly plugged wheel once a second */
    last_scan=now;
    int count=0;
    SDL_JoystickID *ids=SDL_GetJoysticks(&count);
    for (int i=0;ids && i<count && !wheel;++i) {
        const char *name=SDL_GetJoystickNameForID(ids[i]);
        const bool by_type=!SDL_strcasecmp(wheel_device,"auto") && SDL_GetJoystickTypeForID(ids[i])==SDL_JOYSTICK_TYPE_WHEEL;
        if (!by_type && !wheel_name_matches(name)) continue;
        if (!(wheel=SDL_OpenJoystick(ids[i]))) continue;
        wheel_id=ids[i];
        SDL_UpdateJoysticks();
        printf("Runtime: wheel connected: %s (%d axes, %d buttons, %d hats), %d degrees, reported to the game as a PS4 steering wheel\n",
               name ? name : "?",SDL_GetNumJoystickAxes(wheel),SDL_GetNumJoystickButtons(wheel),SDL_GetNumJoystickHats(wheel),wheel_rotation);
        printf("Runtime: wheel axes at rest:");
        for (int a=0;a<SDL_GetNumJoystickAxes(wheel);++a) printf(" %d=%d",a,SDL_GetJoystickAxis(wheel,a));
        printf("\n");
        for (int a=0;a<WAX_COUNT;++a) {
            /* A pedal at rest sits at one end of its axis; at the top end it reads backwards. */
            wheel_inverted[a]=wheel_axis[a].invert==WINV_YES ||
                              (wheel_axis[a].invert==WINV_AUTO && wheel_axis[a].axis>=0 && axis_raw(wheel,wheel_axis[a].axis)>16384);
            if (wheel_axis[a].axis>=0)
                printf("Runtime: wheel %s = axis %d%s\n",wheel_axis_names[a],wheel_axis[a].axis,wheel_inverted[a] ? ", inverted" : "");
        }
    }
    if (!wheel && count>0 && ids) {
        static bool listed;
        if (!listed) {
            listed=true;
            printf("Runtime: no wheel among the game controllers:");
            for (int i=0;i<count;++i) printf(" \"%s\"",SDL_GetJoystickNameForID(ids[i]) ? SDL_GetJoystickNameForID(ids[i]) : "?");
            printf(" (wheel_device in input_config\\default.ini picks one by name)\n");
        }
    }
    SDL_free(ids);
    return wheel;
}
/* The shifter's controller, looked for once a second while the wheel is connected; called under lock. NULL also
 * when the gears are buttons of the wheel (wheel_shifter = wheel). */
static SDL_Joystick *current_shifter(void) {
    char want[64]; snprintf(want,sizeof want,"%s",shifter_device); lower(want); trim(want);
    if (!wheel || !shifter_gear_count || !strcmp(want,"off") || !strcmp(want,"none") || !strcmp(want,"wheel")) return NULL;
    if (shifter && !SDL_JoystickConnected(shifter)) { printf("Runtime: shifter disconnected\n"); SDL_CloseJoystick(shifter); shifter=NULL; }
    if (shifter) return shifter;
    static uint64_t last_scan;
    const uint64_t now=now_us();
    if (last_scan && now-last_scan<1000000) return NULL;
    last_scan=now;
    if (!strcmp(want,"auto")) snprintf(want,sizeof want,"shifter");
    int count=0;
    SDL_JoystickID *ids=SDL_GetJoysticks(&count);
    for (int i=0;ids && i<count && !shifter;++i) {
        if (ids[i]==wheel_id) continue;
        char n[128]; snprintf(n,sizeof n,"%s",SDL_GetJoystickNameForID(ids[i]) ? SDL_GetJoystickNameForID(ids[i]) : ""); lower(n);
        if (!strstr(n,want) || !(shifter=SDL_OpenJoystick(ids[i]))) continue;
        printf("Runtime: shifter connected: %s (%d buttons), %d gears set\n",SDL_GetJoystickName(shifter),SDL_GetNumJoystickButtons(shifter),shifter_gear_count);
    }
    SDL_free(ids);
    return shifter;
}
static uint8_t shifter_gear(SDL_Joystick *w) {
    SDL_Joystick *s=current_shifter();
    char want[16]; snprintf(want,sizeof want,"%s",shifter_device); lower(want); trim(want);
    if (!s && !strcmp(want,"wheel")) s=w;
    if (!s) return 0;
    for (int i=0;i<shifter_gear_count;++i) if (SDL_GetJoystickButton(s,shifter_gears[i].index)) return shifter_gears[i].gear_bits;
    return 0;
}
static uint16_t wheel_value(SDL_Joystick *j, int a) {
    if (wheel_axis[a].axis<0) return a==WAX_STEER ? 32768 : 0;
    int v=axis_raw(j,wheel_axis[a].axis)+32768;   /* 0..65535 */
    if (wheel_inverted[a]) v=65535-v;
    return (uint16_t)(v<0 ? 0 : v>65535 ? 65535 : v);
}
/* Adds the wheel to pad data of the wheel port: its buttons, and its axes in the extension bytes. Under lock. */
static void wheel_sample(PadData *d, bool neutral) {
    SDL_Joystick *j=current_wheel();
    if (!j) return;
    WheelExtension w={32768,0,0,0,0,0,WHEEL_MAGIC};
    if (!neutral) {
        w.steer=wheel_value(j,WAX_STEER); w.throttle=wheel_value(j,WAX_THROTTLE); w.brake=wheel_value(j,WAX_BRAKE);
        w.clutch=wheel_value(j,WAX_CLUTCH); w.handbrake=wheel_value(j,WAX_HANDBRAKE);
        w.gear=shifter_gear(j);
        { static uint8_t last_gear; if (w.gear!=last_gear) { static unsigned n; if (n++<60) printf("Runtime: shifter gear %s\n",w.gear==0x80 ? "R" : !w.gear ? "N" : w.gear==1 ? "1" : w.gear==2 ? "2" : w.gear==4 ? "3" : w.gear==8 ? "4" : w.gear==16 ? "5" : "6"); last_gear=w.gear; } }
        for (int i=0;i<wheel_button_count;++i) {
            if (!SDL_GetJoystickButton(j,wheel_buttons[i].index)) continue;
            if (wheel_buttons[i].out_kind==OUT_BUTTON) d->buttons|=wheel_buttons[i].button;
            else touch_click(d,wheel_buttons[i].touch_side==2);
        }
        if (wheel_hat_dpad && SDL_GetNumJoystickHats(j)>0) {
            const uint8_t hat=SDL_GetJoystickHat(j,0);
            if (hat & SDL_HAT_UP) d->buttons|=BTN_UP;
            if (hat & SDL_HAT_DOWN) d->buttons|=BTN_DOWN;
            if (hat & SDL_HAT_LEFT) d->buttons|=BTN_LEFT;
            if (hat & SDL_HAT_RIGHT) d->buttons|=BTN_RIGHT;
        }
        if (wheel_as_stick) {
            const uint8_t x=(uint8_t)(w.steer>>8), l2=(uint8_t)(w.brake>>8), r2=(uint8_t)(w.throttle>>8);
            if (abs(x-128)>abs((int)d->left_x-128)) d->left_x=x;
            if (l2>d->l2) d->l2=l2;
            if (r2>d->r2) d->r2=r2;
            if (d->l2>30) d->buttons|=BTN_L2;
            if (d->r2>30) d->buttons|=BTN_R2;
        }
        /* For the wheel_* settings: which axis and button numbers the player touches. */
        static int last_axis[16]; static bool have_last; static unsigned logged; static uint64_t last_buttons;
        const int naxes=SDL_GetNumJoystickAxes(j), nbuttons=SDL_GetNumJoystickButtons(j);
        if (logged<300) {
            for (int a=0;a<naxes && a<16;++a) {
                const int v=SDL_GetJoystickAxis(j,a);
                if (!have_last) last_axis[a]=v;
                else if (abs(v-last_axis[a])>8000) { printf("Runtime: wheel axis %d = %d\n",a,v); last_axis[a]=v; ++logged; }
            }
            have_last=true;
            uint64_t b=0;
            for (int i=0;i<nbuttons && i<64;++i) if (SDL_GetJoystickButton(j,i)) b|=UINT64_C(1)<<i;
            for (int i=0;i<64;++i) if ((b & ~last_buttons)>>i & 1) { printf("Runtime: wheel button %d\n",i); ++logged; }
            last_buttons=b;
        }
    }
    memcpy(d->extension,&w,sizeof w);
}

static ABI int32_t pad_device_class_extended_info(int32_t handle, DeviceClassExtendedInfo *info) { PADLOG("scePadDeviceClassGetExtendedInformation");
    if (!handle_ok(handle)) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    pthread_mutex_lock(&lock);
    const bool is_wheel=is_wheel_port(handle) && current_wheel();
    pthread_mutex_unlock(&lock);
    info->device_class=is_wheel ? DEVICE_CLASS_STEERING_WHEEL : DEVICE_CLASS_STANDARD;
    if (is_wheel) info->max_physical_wheel_angle=(uint16_t)wheel_rotation;
    return 0;
}
static ABI int32_t pad_device_class_parse(int32_t handle, const PadData *data, DeviceClassData *out) {
    if (!handle_ok(handle)) return ERR_INVALID_HANDLE;
    if (!data || !out) return ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));
    WheelExtension w;
    memcpy(&w,data->extension,sizeof w);
    if (!is_wheel_port(handle) || w.magic!=WHEEL_MAGIC) { out->device_class=DEVICE_CLASS_STANDARD; return 0; }
    out->device_class=DEVICE_CLASS_STEERING_WHEEL; out->data_valid=1;
    out->steering_wheel_angle=((float)w.steer-32768.0f)/32767.0f*(float)wheel_rotation*0.5f;
    out->steering_wheel=w.steer; out->accelerator_pedal=w.throttle; out->brake_pedal=w.brake;
    out->clutch_pedal=w.clutch; out->hand_brake=w.handbrake; out->gear=w.gear;
    return 0;
}
