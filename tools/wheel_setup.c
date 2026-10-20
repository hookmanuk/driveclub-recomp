/* WheelSetup: finds the steering wheel's axes and buttons and the shifter's gears by asking the player to move them,
 * and writes the wheel_* lines of data\user\input_config\default.ini (see src/runtime_wheel.inc). It speaks English,
 * or Russian on a Russian Windows. A console program, one exe:
 *   clang -O2 -o WheelSetup.exe tools/wheel_setup.c -static $(pkg-config --static --libs sdl3) -mconsole
 * (-mconsole after the SDL flags: they bring -mwindows, and a GUI program has no console to ask in). */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#endif

static bool ru;
#define T(en, ru_text) (ru ? (ru_text) : (en))

enum { KEY_NONE, KEY_ENTER, KEY_ESC };
static int key(void) {
#ifdef _WIN32
    while (_kbhit()) {
        int c=_getch();
        if (c==13) return KEY_ENTER;
        if (c==27) return KEY_ESC;
        if (c==0 || c==224) _getch();
    }
#endif
    return KEY_NONE;
}
static void quit(int code) {
    printf("%s",T("\nPress any key to close this window.\n","\nНажми любую клавишу, чтобы закрыть окно.\n"));
#ifdef _WIN32
    while (_kbhit()) _getch();
    _getch();
#endif
    SDL_Quit();
    exit(code);
}
static void aborted(void) { printf("%s",T("\n\nCancelled, the settings were not changed.\n","\n\nОтменено, настройки не изменены.\n")); quit(1); }

static SDL_Joystick *joy;
static int naxes, nbuttons;
static int base[64];
static bool base_pressed[256];

static void update(void) { SDL_UpdateJoysticks(); SDL_Delay(5); }
static void take_baseline(void) {
    update();
    for (int a=0;a<naxes && a<64;++a) base[a]=SDL_GetJoystickAxis(joy,a);
    for (int b=0;b<nbuttons && b<256;++b) base_pressed[b]=SDL_GetJoystickButton(joy,b);
}

/* Waits for an axis not in used[] to leave its rest value by most of its travel. Returns the axis or -1 (skipped).
 * *sign: +1 when it moved up from rest. */
static int wait_axis(const bool *used, bool can_skip, int *sign) {
    for (;;) {
        const int k=key();
        if (k==KEY_ESC) aborted();
        if (k==KEY_ENTER && can_skip) return -1;
        update();
        int best=-1, best_d=0;
        for (int a=0;a<naxes && a<64;++a) {
            if (used[a]) continue;
            const int d=SDL_GetJoystickAxis(joy,a)-base[a];
            if (abs(d)>abs(best_d)) { best=a; best_d=d; }
        }
        if (best>=0 && abs(best_d)>22000) { *sign=best_d>0 ? 1 : -1; return best; }
    }
}
static void wait_axis_back(int a) {
    while (abs(SDL_GetJoystickAxis(joy,a)-base[a])>5000) { if (key()==KEY_ESC) aborted(); update(); }
}
/* Waits for a button that was not held at the start to be pressed, then for its release. -1: skipped. */
static int wait_button(void) {
    for (;;) {
        const int k=key();
        if (k==KEY_ESC) aborted();
        if (k==KEY_ENTER) return -1;
        update();
        for (int b=0;b<nbuttons && b<256;++b) {
            if (base_pressed[b] || !SDL_GetJoystickButton(joy,b)) continue;
            while (SDL_GetJoystickButton(joy,b)) { if (key()==KEY_ESC) aborted(); update(); }
            return b;
        }
    }
}

static bool looks_like_wheel(SDL_JoystickID id) {
    if (SDL_GetJoystickTypeForID(id)==SDL_JOYSTICK_TYPE_WHEEL) return true;
    char n[128]; snprintf(n,sizeof n,"%s",SDL_GetJoystickNameForID(id) ? SDL_GetJoystickNameForID(id) : "");
    for (char *p=n;*p;++p) *p=(char)tolower((unsigned char)*p);
    static const char *const words[]={"wheel","moza","racing","fanatec","thrustmaster","simagic","simucube","cammus",
                                      "logitech g2","logitech g9","g29","g920","g923","t300","t248","t150","t500","tx racing"};
    for (size_t i=0;i<sizeof(words)/sizeof(*words);++i) if (strstr(n,words[i])) return true;
    return false;
}

static void ini_path(char *out, size_t size) {
    char dir[1024]="";
    const char *b=SDL_GetBasePath();
    if (b) snprintf(dir,sizeof dir,"%s",b);
    static const char *const tries[]={"data\\user\\input_config\\default.ini","..\\data\\user\\input_config\\default.ini"};
    for (size_t i=0;i<2;++i) {
        snprintf(out,size,"%s%s",dir,tries[i]);
        FILE *f=fopen(out,"rb");
        if (f) { fclose(f); return; }
    }
    out[0]=0;
}

int main(void) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout,NULL,_IONBF,0);
    ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
#endif
    printf("%s",T("Steering wheel setup for DriveClub Recomp\n=========================================\n\n",
                  "Настройка руля для DriveClub Recomp\n===================================\n\n"));
    char path[1200];
    ini_path(path,sizeof path);
    if (!path[0]) {
        printf("%s",T("data\\user\\input_config\\default.ini was not found.\n"
                      "Put WheelSetup.exe next to DriveClub.exe and start the game once.\n",
                      "Не найден data\\user\\input_config\\default.ini.\n"
                      "Положи WheelSetup.exe рядом с DriveClub.exe и хотя бы раз запусти игру.\n"));
        quit(1);
    }
    printf("%s",T("Looking for controllers... On some PCs Windows takes up to a few minutes to answer, please wait.\n\n",
                  "Ищу контроллеры... На некоторых ПК Windows отвечает несколько минут (как окно «Игровые устройства»), подожди.\n\n"));
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    if (!SDL_Init(SDL_INIT_JOYSTICK)) { printf("SDL: %s\n",SDL_GetError()); quit(1); }
    for (int i=0;i<40;++i) update(); /* the controllers are listed shortly after start */

    int count=0;
    SDL_JoystickID *ids=SDL_GetJoysticks(&count);
    if (!ids || count==0) {
        printf("%s",T("Windows lists no game controller. Is the wheel connected and switched on?\n",
                      "Windows не видит ни одного игрового контроллера. Руль подключён и включён?\n"));
        quit(1);
    }
    int pick=-1;
    printf("%s",T("Controllers found:\n","Найденные контроллеры:\n"));
    for (int i=0;i<count;++i) {
        const bool w=looks_like_wheel(ids[i]);
        printf("  %d. %s%s\n",i+1,SDL_GetJoystickNameForID(ids[i]) ? SDL_GetJoystickNameForID(ids[i]) : "?",
               w ? T("  <- wheel","  <- руль") : "");
        if (w && pick<0) pick=i;
    }
    if (pick<0 || count>1) {
        printf(T("\nWhich one is the wheel? Type its number%s: ","\nКакой из них руль? Введи номер%s: "),
               pick>=0 ? T(" (Enter: the marked one)"," (Enter — отмеченный)") : "");
        char line[32]="";
        if (fgets(line,sizeof line,stdin) && atoi(line)>=1 && atoi(line)<=count) pick=atoi(line)-1;
        if (pick<0) { printf("%s",T("No wheel chosen.\n","Руль не выбран.\n")); quit(1); }
    }
    const SDL_JoystickID id=ids[pick];
    char name[128]; snprintf(name,sizeof name,"%s",SDL_GetJoystickNameForID(id) ? SDL_GetJoystickNameForID(id) : "");
    /* wheel_device = auto opens the first controller that looks like a wheel; otherwise the name picks this one. */
    int first_wheel=-1;
    for (int i=0;i<count && first_wheel<0;++i) if (looks_like_wheel(ids[i])) first_wheel=i;
    const bool auto_finds=first_wheel==pick;
    SDL_free(ids);
    if (!(joy=SDL_OpenJoystick(id))) { printf(T("The wheel could not be opened: %s\n","Не удалось открыть руль: %s\n"),SDL_GetError()); quit(1); }
    naxes=SDL_GetNumJoystickAxes(joy); nbuttons=SDL_GetNumJoystickButtons(joy);
    printf(T("\nWheel: %s (%d axes, %d buttons)\n","\nРуль: %s (%d осей, %d кнопок)\n"),name,naxes,nbuttons);
    printf("%s",T("Esc at any time leaves without saving.\n\n","Esc в любой момент — выйти без сохранения.\n\n"));

    printf("%s",T("Release the pedals, hold the wheel straight and press Enter...","Отпусти педали, поставь руль прямо и нажми Enter..."));
    for (int k=key(); k!=KEY_ENTER; k=key()) { if (k==KEY_ESC) aborted(); update(); }
    take_baseline();
    printf("%s",T(" done.\n\n"," готово.\n\n"));

    bool used[64]={false};
    int sign;
    printf("%s",T("[1] Turn the wheel all the way LEFT...","[1] Поверни руль до упора ВЛЕВО..."));
    const int steer=wait_axis(used,false,&sign);
    const bool steer_inverted=sign>0;   /* left must read below the centre */
    used[steer]=true;
    printf(T(" axis %d%s.\n    Back to the centre."," ось %d%s.\n    Верни руль в центр."),steer,steer_inverted ? T(" (reversed)"," (перевёрнута)") : "");
    wait_axis_back(steer);
    printf("\n\n");

    static const char *const pedal_key[]={"throttle","brake","clutch","handbrake"};
    const char *const pedal_text[]={T("Press the THROTTLE all the way","Выжми до конца педаль ГАЗА"),
                                    T("Press the BRAKE all the way","Выжми до конца педаль ТОРМОЗА"),
                                    T("Press the CLUTCH all the way (Enter: there is none)","Выжми до конца педаль СЦЕПЛЕНИЯ (Enter — её нет)"),
                                    T("Pull the HANDBRAKE all the way (Enter: there is none)","Потяни до конца РУЧНИК (Enter — его нет)")};
    int pedal[4]; bool pedal_inverted[4];
    for (int p=0;p<4;++p) {
        printf("[%d] %s...",p+2,pedal_text[p]);
        pedal[p]=wait_axis(used,p>=2,&sign);
        pedal_inverted[p]=false;
        if (pedal[p]<0) { printf("%s",T(" skipped.\n\n"," пропущено.\n\n")); continue; }
        used[pedal[p]]=true;
        pedal_inverted[p]=sign<0;        /* pressed must read above rest */
        printf(T(" axis %d%s.\n    Release it."," ось %d%s.\n    Отпусти."),pedal[p],pedal_inverted[p] ? T(" (reversed)"," (перевёрнута)") : "");
        wait_axis_back(pedal[p]);
        printf("\n\n");
    }

    const struct { const char *ps, *text; } actions[]={
        {"r1",T("Shift UP (usually the right paddle)","Передача ВВЕРХ (обычно правый лепесток)")},
        {"l1",T("Shift DOWN (usually the left paddle)","Передача ВНИЗ (обычно левый лепесток)")},
        {"cross",T("Cross: accept in menus","Крестик: выбрать в меню")},
        {"circle",T("Circle: back in menus, handbrake in a race","Кружок: назад в меню, ручник в гонке")},
        {"triangle",T("Triangle: change camera","Треугольник: смена камеры")},
        {"square",T("Square","Квадрат")},
        {"options",T("Options: pause","Options: пауза")},
        {"touchpad_left",T("Touchpad left: reset to track","Тачпад слева: сброс на трассу")},
        {"touchpad_right",T("Touchpad right","Тачпад справа")},
        {"l3","L3"},
        {"r3","R3"},
        {"pad_up",T("D-pad up (only if the wheel's own d-pad does not work)","Крестовина вверх (если крестовина на руле не работает сама)")},
        {"pad_down",T("D-pad down","Крестовина вниз")},
        {"pad_left",T("D-pad left","Крестовина влево")},
        {"pad_right",T("D-pad right","Крестовина вправо")},
    };
    enum { NACT=sizeof(actions)/sizeof(*actions) };
    int button[NACT];
    printf("%s",T("Now the buttons. Press the wheel button you want for each action. Enter skips one.\n\n",
                  "Теперь кнопки. Нажимай кнопку на руле, которую хочешь для действия. Enter — пропустить.\n\n"));
    for (int i=0;i<NACT;++i) {
        for (;;) {
            printf("  %-62s",actions[i].text);
            fflush(stdout);
            button[i]=wait_button();
            int dup=-1;
            for (int j=0;j<i && button[i]>=0;++j) if (button[j]==button[i]) dup=j;
            if (button[i]<0) { printf("%s",T("skipped\n","пропущено\n")); break; }
            if (dup<0) { printf(T("button %d\n","кнопка %d\n"),button[i]); break; }
            printf(T("button %d is already taken (\"%s\"), choose another\n","кнопка %d уже занята («%s»), выбери другую\n"),button[i],actions[dup].text);
        }
    }

    /* H-pattern shifter: a controller of its own or buttons of the wheel; each gear holds one button down. */
    SDL_Joystick *dev[9]={joy}; int ndev=1;
    {
        int cnt=0;
        SDL_JoystickID *all=SDL_GetJoysticks(&cnt);
        for (int i=0;all && i<cnt && ndev<9;++i) if (all[i]!=id && (dev[ndev]=SDL_OpenJoystick(all[i]))) ++ndev;
        SDL_free(all);
    }
    static const char *const gear_name[]={"1","2","3","4","5","6","r"};
    const char *const gear_text[]={T("1st","1-ю"),T("2nd","2-ю"),T("3rd","3-ю"),T("4th","4-ю"),T("5th","5-ю"),T("6th","6-ю"),
                                   T("reverse","заднюю")};
    int gear_button[7], shifter_dev=-1;
    for (int g=0;g<7;++g) gear_button[g]=-1;
    printf("%s",T("\nH-pattern shifter. Put it in 1st gear (Enter: there is no shifter)...",
                  "\nH-шифтер. Включи 1-ю передачу (Enter — шифтера нет)..."));
    for (int g=0;g<7;++g) {
        if (g) printf(T("  Put it in %s gear (Enter: there is none)...","  Включи %s передачу (Enter — нет такой)..."),gear_text[g]);
        bool base_b[9][256];
        update();
        for (int d=0;d<ndev;++d) for (int b=0;b<SDL_GetNumJoystickButtons(dev[d]) && b<256;++b) base_b[d][b]=SDL_GetJoystickButton(dev[d],b);
        int got_d=-1, got_b=-1;
        while (got_b<0) {
            const int k=key();
            if (k==KEY_ESC) aborted();
            if (k==KEY_ENTER) break;
            update();
            for (int d=0;d<ndev && got_b<0;++d) {
                if (shifter_dev>=0 && d!=shifter_dev) continue;
                for (int b=0;b<SDL_GetNumJoystickButtons(dev[d]) && b<256;++b)
                    if (!base_b[d][b] && SDL_GetJoystickButton(dev[d],b)) { got_d=d; got_b=b; break; }
            }
        }
        if (got_b<0) { printf("%s",T(" skipped.\n"," пропущено.\n")); if (!g) break; continue; }
        shifter_dev=got_d; gear_button[g]=got_b;
        printf(T(" button %d (%s). Back to neutral."," кнопка %d (%s). Верни в нейтраль."),got_b,
               got_d ? SDL_GetJoystickName(dev[got_d]) : T("the wheel","руль"));
        while (SDL_GetJoystickButton(dev[got_d],got_b)) { if (key()==KEY_ESC) aborted(); update(); }
        printf("\n");
    }
    char shifter_setting[128]="off";
    if (shifter_dev==0) snprintf(shifter_setting,sizeof shifter_setting,"wheel");
    else if (shifter_dev>0) {
        char low[128]; snprintf(low,sizeof low,"%s",SDL_GetJoystickName(dev[shifter_dev]) ? SDL_GetJoystickName(dev[shifter_dev]) : "");
        for (char *p=low;*p;++p) *p=(char)tolower((unsigned char)*p);
        snprintf(shifter_setting,sizeof shifter_setting,"%s",strstr(low,"shifter") ? "auto" : SDL_GetJoystickName(dev[shifter_dev]));
    }

    printf("%s",T("\nThe wheel's rotation in degrees, as set in its own software (Enter: 900): ",
                  "\nУгол поворота руля в градусах, как в его программе (Pit House, G Hub...) (Enter — 900): "));
    char line[32]="";
    int rotation=900;
    if (fgets(line,sizeof line,stdin) && atoi(line)>=90 && atoi(line)<=3600) rotation=atoi(line);

    /* default.ini: the old wheel_* lines go, everything else stays; the new ones are added at the end. */
    FILE *f=fopen(path,"rb");
    if (!f) { printf(T("Cannot open %s\n","Не открыть %s\n"),path); quit(1); }
    char *text=calloc(1,1<<20); size_t n=text ? fread(text,1,(1<<20)-1,f) : 0; fclose(f);
    if (!text) quit(1);
    char backup[1300]; snprintf(backup,sizeof backup,"%s.bak",path);
    if ((f=fopen(backup,"wb"))) { fwrite(text,1,n,f); fclose(f); }
    if (!(f=fopen(path,"wb"))) { printf(T("Cannot write %s (is the game running?)\n","Не записать %s (игра открыта?)\n"),path); quit(1); }
    bool kept_ffb=false;   /* the force feedback lines are the player's tuning: they stay as they are */
    for (char *l=text; l && *l;) {
        char *next=strchr(l,'\n');
        const size_t len=next ? (size_t)(next-l)+1 : strlen(l);
        const char *s=l; while (*s==' ' || *s=='\t') ++s;
        const bool ffb=!SDL_strncasecmp(s,"wheel_ffb",9);
        if (ffb) kept_ffb=true;
        if (ffb || SDL_strncasecmp(s,"wheel_",6)) fwrite(l,1,len,f);
        l=next ? next+1 : NULL;
    }
    if (n && text[n-1]!='\n') fputc('\n',f);
    time_t now=time(NULL); char when[32]; strftime(when,sizeof when,"%Y-%m-%d %H:%M",localtime(&now));
    fprintf(f,"\n# Wheel settings written by WheelSetup.exe on %s for %s\n",when,name);
    fprintf(f,"wheel_device = %s\n",auto_finds ? "auto" : name);
    fprintf(f,"wheel_rotation = %d\n",rotation);
    fprintf(f,"wheel_steering = %d%s\n",steer,steer_inverted ? ", inverted" : "");
    for (int p=0;p<4;++p) {
        if (pedal[p]<0) fprintf(f,"wheel_%s = none\n",pedal_key[p]);
        else fprintf(f,"wheel_%s = %d, %s\n",pedal_key[p],pedal[p],pedal_inverted[p] ? "inverted" : "normal");
    }
    for (int i=0;i<NACT;++i) if (button[i]>=0) fprintf(f,"wheel_button = %d, %s\n",button[i],actions[i].ps);
    fprintf(f,"wheel_shifter = %s\n",shifter_setting);
    for (int g=0;g<7;++g) if (gear_button[g]>=0) fprintf(f,"wheel_gear = %d, %s\n",gear_button[g],gear_name[g]);
    fprintf(f,"wheel_hat_dpad = true\nwheel_as_stick = false\n");
    if (!kept_ffb) fprintf(f,"wheel_ffb = on\nwheel_ffb_gain = 100\nwheel_ffb_vibration = 100\nwheel_ffb_invert = false\n");
    fclose(f);
    free(text);
    printf(T("\nDone! The settings are in\n  %s\n(the previous file is kept as default.ini.bak). You can start the game.\n",
             "\nГотово! Настройки записаны в\n  %s\n(прежний файл сохранён как default.ini.bak). Можно запускать игру.\n"),path);
    quit(0);
}
