/* Force feedback: the game drives forces only for a Logitech wheel, through the bundled logiWheel.prx. Its exports
 * are replaced here (by NID; the names are not public) so that the game sees a Logitech wheel whenever a wheel is
 * connected, and the forces it plays go to that wheel through SDL's haptic API (DirectInput force feedback).
 *
 * What the game does (guest +0xe12e30 and the methods of its Logitech wheel class, vtable +0x165c020):
 *   logiWheelInit(0x1000c); every frame logiWheelReadState(state): 0 means connected, and the state is filled from
 *   the steering wheel device class data, as logiWheel.prx itself does (+0x2e61ed0);
 *   at connection it creates five effects with logiWheelPlayEffect(&id, params) and stops them, then changes them
 *   with logiWheelUpdateEffect(id, params) while driving:
 *     type 0 constant force   a = level
 *     type 7 spring            a = centre on the steering scale (-32768..32767), c/d = coefficients,
 *                              e/f = saturations: the road feel, a spring that follows the car
 *     type 8 damper            c/d = coefficients, e/f = saturations
 *     type 2 periodic (x2)     a = magnitude, c = period in ms
 *   Levels, coefficients and saturations are on Logitech's 8-bit scale (logiWheel.prx clamps them, +0x2e64080).
 *   Measured on a Moza R3 through DirectInput: a positive constant force turns the wheel left, a positive spring
 *   coefficient centres it.
 * DirectInput can take a long time to answer on some PCs, so the haptic device lives on a thread of its own: the
 * game only records what it wants, and that thread passes it on. Included by runtime_pad.c. */

#define LOGI_OK 0
#define LOGI_ERR_ARG ((int32_t)0x80000001)
#define LOGI_ERR_NOT_CONNECTED ((int32_t)0x80000004)
#define LOGI_EFFECTS 16
#pragma pack(push,1)
typedef struct {
    uint8_t type; int32_t duration, delay; uint8_t flags;
    int32_t a, b, c, d, e, f, g, h; uint8_t tail[4];
} LogiEffectParams;
#pragma pack(pop)
_Static_assert(sizeof(LogiEffectParams)==0x2e,"logiWheel effect parameters (guest +0xe12e9a)");
enum { LOGI_CONSTANT=0, LOGI_PERIODIC=2, LOGI_SPRING=7, LOGI_DAMPER=8 };

static struct {
    pthread_mutex_t m;
    bool started;
    struct { bool used, playing, dirty; LogiEffectParams p; } fx[LOGI_EFFECTS];
    bool autocenter, autocenter_dirty;
} ffb={.m=PTHREAD_MUTEX_INITIALIZER,.autocenter=true};

static PadData wheel_last_read;   /* the last read of the wheel port, under lock */
static int32_t rd_clamp(int32_t v, int32_t lo, int32_t hi) { return v<lo ? lo : v>hi ? hi : v; }
static Sint16 level16(int32_t v) { return (Sint16)(rd_clamp(v,-255,255)*32767/255); }
static Uint16 sat16(int32_t v) { return (Uint16)(rd_clamp(v,0,255)*65535/255); }
static bool ffb_effect_from(const LogiEffectParams *p, SDL_HapticEffect *e) {
    memset(e,0,sizeof(*e));
    const int sign=ffb_invert ? -1 : 1;
    switch (p->type) {
    case LOGI_CONSTANT:
        e->type=SDL_HAPTIC_CONSTANT;
        e->constant.direction.type=SDL_HAPTIC_CARTESIAN; e->constant.direction.dir[0]=1;
        e->constant.length=SDL_HAPTIC_INFINITY;
        e->constant.level=(Sint16)(sign*level16(p->a*ffb_gain/100));
        return true;
    case LOGI_PERIODIC:
        e->type=SDL_HAPTIC_SQUARE;
        e->periodic.direction.type=SDL_HAPTIC_CARTESIAN; e->periodic.direction.dir[0]=1;
        e->periodic.length=SDL_HAPTIC_INFINITY;
        e->periodic.period=(Uint16)rd_clamp(p->c,1,10000);
        e->periodic.magnitude=level16((p->a<0 ? -p->a : p->a)*ffb_vibration/100);
        return true;
    case LOGI_SPRING: case LOGI_DAMPER:
        e->type=p->type==LOGI_SPRING ? SDL_HAPTIC_SPRING : SDL_HAPTIC_DAMPER;
        e->condition.direction.type=SDL_HAPTIC_CARTESIAN; e->condition.direction.dir[0]=1;
        e->condition.length=SDL_HAPTIC_INFINITY;
        e->condition.right_coeff[0]=level16(p->c); e->condition.left_coeff[0]=level16(p->d);
        /* the saturation is the strength of the force; wheel_ffb_gain above 100 lifts it, clipped at full */
        e->condition.right_sat[0]=sat16(p->e*ffb_gain/100); e->condition.left_sat[0]=sat16(p->f*ffb_gain/100);
        if (p->type==LOGI_SPRING) {
            /* The centre is where the tyres want the wheel, on the steering scale itself (-32768..32767, as the
             * wheel position the game reads): DriveClub's road feel is this spring following the car, with the
             * saturation as its strength. */
            e->condition.center[0]=(Sint16)rd_clamp(sign*p->a,-32767,32767);
            e->condition.deadband[0]=(Uint16)(rd_clamp(p->b,0,255)*65535/255);
        }
        return true;
    }
    return false;
}
/* BB_FFB_LOG=1: the effects as the game sets them, and ten lines a second of the forces while driving (for tuning). */
static bool ffb_log(void) { static int on=-1; if (on<0) { const char *e=getenv("BB_FFB_LOG"); on=e && *e && *e!='0'; } return on; }
static void ffb_log_params(const char *what, int id, const LogiEffectParams *p) {
    static unsigned n;
    if (ffb_log() && n++<400) printf("Runtime: FFB %s %d: type %u a %d b %d c %d d %d e %d f %d\n",what,id,p->type,p->a,p->b,p->c,p->d,p->e,p->f);
}

/* The haptic thread: opens the wheel's force feedback and keeps its effects as the game last asked. */
static void *ffb_thread(void *unused) {
    (void)unused;
    SDL_Haptic *haptic=NULL;
    int sdl_id[LOGI_EFFECTS]; bool running[LOGI_EFFECTS];
    for (int i=0;i<LOGI_EFFECTS;++i) { sdl_id[i]=-1; running[i]=false; }
    for (;;) {
        compat_sleep_ns(4000000); /* 4 ms */
        if (!haptic) {
            pthread_mutex_lock(&lock);
            SDL_Joystick *j=wheel;
            pthread_mutex_unlock(&lock);
            if (!j) { compat_sleep_ns(100000000); continue; }
            /* SDL's haptic subsystem starts once the wheel is open, so SDL itself is up by then. */
            if (!SDL_WasInit(SDL_INIT_HAPTIC) && !SDL_InitSubSystem(SDL_INIT_HAPTIC)) {
                printf("Runtime: FFB off: %s\n",SDL_GetError());
                return NULL;
            }
            pthread_mutex_lock(&lock);
            if ((j=wheel)) haptic=SDL_OpenHapticFromJoystick(j);
            pthread_mutex_unlock(&lock);
            if (!j) continue;
            if (!haptic) {
                static bool said;
                if (!said) { said=true; printf("Runtime: FFB: the wheel offers no force feedback to Windows (%s)\n",SDL_GetError()); }
                compat_sleep_ns(1000000000);
                continue;
            }
            const Uint32 f=SDL_GetHapticFeatures(haptic);
            printf("Runtime: FFB on %s: constant %s, spring %s, damper %s, square %s, gain %s\n",SDL_GetHapticName(haptic),
                   f&SDL_HAPTIC_CONSTANT ? "yes" : "no",f&SDL_HAPTIC_SPRING ? "yes" : "no",f&SDL_HAPTIC_DAMPER ? "yes" : "no",
                   f&SDL_HAPTIC_SQUARE ? "yes" : "no",f&SDL_HAPTIC_GAIN ? "yes" : "no");
            if (f&SDL_HAPTIC_GAIN) SDL_SetHapticGain(haptic,100);  /* the gains are applied to the forces themselves */
            pthread_mutex_lock(&ffb.m);
            for (int i=0;i<LOGI_EFFECTS;++i) ffb.fx[i].dirty=ffb.fx[i].used;
            ffb.autocenter_dirty=true;
            pthread_mutex_unlock(&ffb.m);
        }
        pthread_mutex_lock(&lock);
        const bool gone=!wheel;
        pthread_mutex_unlock(&lock);
        if (gone) {
            SDL_CloseHaptic(haptic); haptic=NULL;
            for (int i=0;i<LOGI_EFFECTS;++i) { sdl_id[i]=-1; running[i]=false; }
            continue;
        }
        LogiEffectParams want[LOGI_EFFECTS]; bool dirty[LOGI_EFFECTS], used[LOGI_EFFECTS], playing[LOGI_EFFECTS];
        bool autocenter, autocenter_dirty;
        pthread_mutex_lock(&ffb.m);
        for (int i=0;i<LOGI_EFFECTS;++i) {
            want[i]=ffb.fx[i].p; dirty[i]=ffb.fx[i].dirty; used[i]=ffb.fx[i].used; playing[i]=ffb.fx[i].playing;
            ffb.fx[i].dirty=false;
        }
        autocenter=ffb.autocenter; autocenter_dirty=ffb.autocenter_dirty; ffb.autocenter_dirty=false;
        pthread_mutex_unlock(&ffb.m);
        if (autocenter_dirty && (SDL_GetHapticFeatures(haptic)&SDL_HAPTIC_AUTOCENTER)) SDL_SetHapticAutocenter(haptic,autocenter ? 100 : 0);
        if (ffb_log()) {   /* ten lines a second while the game plays a force, up to ten minutes of driving */
            static uint64_t last; static unsigned lines;
            const uint64_t now=now_us();
            bool any=false;
            for (int i=0;i<LOGI_EFFECTS;++i) if (used[i] && playing[i] && (want[i].a || (want[i].type==LOGI_SPRING && want[i].e))) any=true;
            if (any && now-last>=100000 && lines<6000) {
                last=now; ++lines;
                pthread_mutex_lock(&lock);
                WheelExtension w; memcpy(&w,wheel_last_read.extension,sizeof w);
                pthread_mutex_unlock(&lock);
                printf("FFB: steer %+6d",w.magic==WHEEL_MAGIC ? (int)w.steer-32768 : 0);
                for (int i=0;i<LOGI_EFFECTS;++i) if (used[i]) {
                    const LogiEffectParams *p=&want[i];
                    if (p->type==LOGI_CONSTANT) printf(" | const%s %+4d",playing[i] ? "" : "(off)",p->a);
                    else if (p->type==LOGI_SPRING) printf(" | spring%s ctr %+4d k %d/%d sat %d/%d",playing[i] ? "" : "(off)",p->a,p->c,p->d,p->e,p->f);
                    else if (p->type==LOGI_DAMPER) printf(" | damper%s k %d/%d sat %d/%d",playing[i] ? "" : "(off)",p->c,p->d,p->e,p->f);
                    else printf(" | wave%s %d ms %d",playing[i] ? "" : "(off)",p->c,p->a);
                }
                printf("\n");
            }
        }
        for (int i=0;i<LOGI_EFFECTS;++i) {
            if (!used[i]) {
                if (sdl_id[i]>=0) { SDL_DestroyHapticEffect(haptic,sdl_id[i]); sdl_id[i]=-1; running[i]=false; }
                continue;
            }
            if (dirty[i]) {
                SDL_HapticEffect e;
                if (ffb_effect_from(&want[i],&e)) {
                    if (sdl_id[i]<0) {
                        sdl_id[i]=SDL_CreateHapticEffect(haptic,&e);
                        if (sdl_id[i]<0) { static unsigned n; if (n++<8) printf("Runtime: FFB effect type %u not created: %s\n",want[i].type,SDL_GetError()); }
                    } else if (!SDL_UpdateHapticEffect(haptic,sdl_id[i],&e)) {
                        static unsigned n; if (n++<8) printf("Runtime: FFB effect %d not updated: %s\n",i,SDL_GetError());
                    }
                }
            }
            if (sdl_id[i]<0) continue;
            if (playing[i] && !running[i]) running[i]=SDL_RunHapticEffect(haptic,sdl_id[i],1);
            else if (!playing[i] && running[i]) { SDL_StopHapticEffect(haptic,sdl_id[i]); running[i]=false; }
        }
    }
    return NULL;
}
static void ffb_start(void) {
    pthread_mutex_lock(&ffb.m);
    const bool start=ffb_enabled && !ffb.started;
    ffb.started=true;
    pthread_mutex_unlock(&ffb.m);
    if (!start) return;
    pthread_t t;
    if (pthread_create(&t,NULL,ffb_thread,NULL)==0) pthread_detach(t);
}

/* ---- logiWheel.prx exports ---------------------------------------------------------------------- */
static bool logi_connected(void) {   /* wheel_ffb = off: no Logitech wheel, as before force feedback */
    pthread_mutex_lock(&lock);
    input_load();
    const bool c=ffb_enabled && current_wheel()!=NULL;
    pthread_mutex_unlock(&lock);
    return c;
}
static ABI int32_t logi_init(uint32_t version) {
    pthread_mutex_lock(&lock); input_load(); pthread_mutex_unlock(&lock);
    printf("Runtime: logiWheelInit(0x%x): Logitech wheel library replaced, force feedback %s\n",version,ffb_enabled ? "to the wheel" : "off");
    ffb_start();
    return LOGI_OK;
}
static ABI int32_t logi_term(void) { return LOGI_OK; }
/* The state the game reads (guest +0xe13260..+0xe13350): steering at +8, accelerator, brake and clutch at +0xa..+0xe
 * (0..65535), shifter gear at +0x10, the pad buttons from +0x11. Filled from the wheel port's last read. */
static ABI int32_t logi_read_state(uint8_t *state) {
    if (!state) return LOGI_ERR_ARG;
    if (!logi_connected()) return LOGI_ERR_NOT_CONNECTED;
    /* The wheel now, with the buttons of the pad last read on the wheel port (gamepad, keyboard and wheel). */
    PadData d; memset(&d,0,sizeof d);
    pthread_mutex_lock(&lock);
    wheel_sample(&d,bbgpu_overlay_captures_input());
    d.buttons|=wheel_last_read.buttons;
    pthread_mutex_unlock(&lock);
    WheelExtension w; memcpy(&w,d.extension,sizeof w);
    memset(state,0,0x25);
    if (w.magic!=WHEEL_MAGIC) { w.steer=32768; w.throttle=w.brake=w.clutch=0; w.gear=0; }
    memcpy(state+8,&w.steer,2); memcpy(state+0xa,&w.throttle,2); memcpy(state+0xc,&w.brake,2); memcpy(state+0xe,&w.clutch,2);
    state[0x10]=w.gear;
    memcpy(state+0x11,&d.buttons,4);
    return LOGI_OK;
}
static ABI int32_t logi_play_effect(int32_t *id, const LogiEffectParams *p) {
    if (!id || !p) return LOGI_ERR_ARG;
    if (!logi_connected()) return LOGI_ERR_NOT_CONNECTED;
    pthread_mutex_lock(&ffb.m);
    int slot=-1;
    for (int i=0;i<LOGI_EFFECTS && slot<0;++i) if (!ffb.fx[i].used) slot=i;
    if (slot>=0) { ffb.fx[slot].used=true; ffb.fx[slot].playing=true; ffb.fx[slot].dirty=true; ffb.fx[slot].p=*p; }
    ffb.autocenter=false; ffb.autocenter_dirty=true;   /* logiWheel turns the centring spring off for a force */
    pthread_mutex_unlock(&ffb.m);
    if (slot<0) return LOGI_ERR_ARG;
    *id=slot;
    ffb_log_params("play",slot,p);
    return LOGI_OK;
}
static ABI int32_t logi_update_effect(int32_t id, const LogiEffectParams *p) {
    if (!p || id<0 || id>=LOGI_EFFECTS) return LOGI_ERR_ARG;
    pthread_mutex_lock(&ffb.m);
    const bool ok=ffb.fx[id].used;
    if (ok) {
        if (memcmp(&ffb.fx[id].p,p,sizeof(*p))) { ffb.fx[id].p=*p; ffb.fx[id].dirty=true; }
        ffb.fx[id].playing=true;
    }
    pthread_mutex_unlock(&ffb.m);
    if (ok) {
        /* the first changes of each effect, and then every 300th, for the force scale */
        static unsigned count[LOGI_EFFECTS];
        if (count[id]++%300<3) ffb_log_params("update",id,p);
    }
    return ok ? LOGI_OK : LOGI_ERR_ARG;
}
static ABI int32_t logi_stop_effect(int32_t id) {
    if (id<0 || id>=LOGI_EFFECTS) return LOGI_ERR_ARG;
    pthread_mutex_lock(&ffb.m);
    ffb.fx[id].playing=false;
    pthread_mutex_unlock(&ffb.m);
    return LOGI_OK;
}
static ABI int32_t logi_set_autocenter(uint8_t on) {
    pthread_mutex_lock(&ffb.m);
    ffb.autocenter=on!=0; ffb.autocenter_dirty=true;
    pthread_mutex_unlock(&ffb.m);
    return logi_connected() ? LOGI_OK : LOGI_ERR_NOT_CONNECTED;
}
static ABI int32_t logi_set_range(int32_t degrees, uint8_t flag) {
    static unsigned n; if (n++<20) printf("Runtime: logiWheel operating range %d degrees (%u); the wheel keeps its own setting\n",degrees,flag);
    return logi_connected() ? LOGI_OK : LOGI_ERR_NOT_CONNECTED;
}
static ABI int32_t logi_byte_call(uint8_t v) {   /* two calls taking one byte: the rev LEDs and an unknown one */
    (void)v;
    return logi_connected() ? LOGI_OK : LOGI_ERR_NOT_CONNECTED;
}
static ABI int32_t logi_unknown_byte(uint8_t v) {
    static unsigned n; if (n++<4) printf("Runtime: logiWheel call with %u\n",v);
    return logi_connected() ? LOGI_OK : LOGI_ERR_NOT_CONNECTED;
}
static const struct { const char *nid; void *function; } logi_exports[]={
    {"rqu39g1mv38",(void *)logi_init},          /* logiWheelInit */
    {"MjWOyyyHMbw",(void *)logi_term},
    {"pkk2E4Izy-Q",(void *)logi_read_state},    /* logiWheelReadState */
    {"OO6XBeuXhsg",(void *)logi_play_effect},
    {"mPcvqcI2O50",(void *)logi_update_effect},
    {"0gsOoL2NyqQ",(void *)logi_stop_effect},
    {"-8HzgEWqpx4",(void *)logi_set_autocenter},
    {"rFRMV3lLSJs",(void *)logi_set_range},
    {"xqPuCy0mIrc",(void *)logi_byte_call},     /* rev LEDs: 0x1f >> n (guest +0xe13870) */
    {"MubUr+Qm4M4",(void *)logi_unknown_byte},
};
