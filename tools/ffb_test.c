/* FfbTest: plays single force feedback effects on the wheel, the way the runtime passes the game's forces on
 * (src/runtime_logiwheel.inc), so the directions and strengths can be checked by hand without the game. It speaks
 * English, or Russian on a Russian Windows.
 *   clang -O2 -o FfbTest.exe tools/ffb_test.c -static $(pkg-config --static --libs sdl3) -mconsole */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#endif

static bool ru;
#define T(en, ru_text) (ru ? (ru_text) : (en))

static SDL_Joystick *joy;
static SDL_Haptic *haptic;

static bool looks_like_wheel(SDL_JoystickID id) {
    if (SDL_GetJoystickTypeForID(id)==SDL_JOYSTICK_TYPE_WHEEL) return true;
    char n[128]; snprintf(n,sizeof n,"%s",SDL_GetJoystickNameForID(id) ? SDL_GetJoystickNameForID(id) : "");
    for (char *p=n;*p;++p) *p=(char)tolower((unsigned char)*p);
    static const char *const words[]={"wheel","moza","racing","fanatec","thrustmaster","simagic","simucube","cammus","g29","g920","g923","t300","t150"};
    for (size_t i=0;i<sizeof(words)/sizeof(*words);++i) if (strstr(n,words[i])) return true;
    return false;
}
static void quit(int code) {
    printf("%s",T("\nPress any key to close this window.\n","\nНажми любую клавишу, чтобы закрыть окно.\n"));
#ifdef _WIN32
    while (_kbhit()) _getch();
    _getch();
#endif
    if (haptic) SDL_CloseHaptic(haptic);
    SDL_Quit();
    exit(code);
}
static int steer(void) { SDL_UpdateJoysticks(); return SDL_GetJoystickAxis(joy,0); }

/* Plays the effect for the given time and prints the wheel position as it goes. */
static void play(const char *what, SDL_HapticEffect *e, int ms) {
    const int id=SDL_CreateHapticEffect(haptic,e);
    if (id<0) { printf(T("  not created: %s\n","  не создан: %s\n"),SDL_GetError()); return; }
    printf("  %s... ",what);
    const int start=steer();
    SDL_RunHapticEffect(haptic,id,1);
    int lo=start, hi=start;
    for (int t=0;t<ms;t+=10) { SDL_Delay(10); const int s=steer(); if (s<lo) lo=s; if (s>hi) hi=s; }
    SDL_StopHapticEffect(haptic,id);
    SDL_DestroyHapticEffect(haptic,id);
    const int end=steer();
    printf(T("the wheel was at %d, now %d (from %d to %d; minus is left)\n","руль был %d, стал %d (от %d до %d; минус — влево)\n"),start,end,lo,hi);
}
static void constant(int percent, int ms) {
    SDL_HapticEffect e; memset(&e,0,sizeof e);
    e.type=SDL_HAPTIC_CONSTANT;
    e.constant.direction.type=SDL_HAPTIC_CARTESIAN; e.constant.direction.dir[0]=1;
    e.constant.length=SDL_HAPTIC_INFINITY;
    e.constant.level=(Sint16)(percent*32767/100);
    char t[64]; snprintf(t,sizeof t,T("constant force %+d%%","постоянная сила %+d%%"),percent);
    play(t,&e,ms);
}
static void condition(Uint16 type, int coeff_percent, int sat_percent, int ms) {
    SDL_HapticEffect e; memset(&e,0,sizeof e);
    e.type=type;
    e.condition.direction.type=SDL_HAPTIC_CARTESIAN; e.condition.direction.dir[0]=1;
    e.condition.length=SDL_HAPTIC_INFINITY;
    e.condition.right_coeff[0]=e.condition.left_coeff[0]=(Sint16)(coeff_percent*32767/100);
    e.condition.right_sat[0]=e.condition.left_sat[0]=(Uint16)(sat_percent*65535/100);
    char t[128];
    snprintf(t,sizeof t,T("%s, coefficient %+d%%, saturation %d%%","%s, коэффициент %+d%%, насыщение %d%%"),
             type==SDL_HAPTIC_SPRING ? T("spring","пружина") : T("damper","демпфер"),coeff_percent,sat_percent);
    play(t,&e,ms);
}
static void square(int percent, int period, int ms) {
    SDL_HapticEffect e; memset(&e,0,sizeof e);
    e.type=SDL_HAPTIC_SQUARE;
    e.periodic.direction.type=SDL_HAPTIC_CARTESIAN; e.periodic.direction.dir[0]=1;
    e.periodic.length=SDL_HAPTIC_INFINITY;
    e.periodic.period=(Uint16)period;
    e.periodic.magnitude=(Sint16)(percent*32767/100);
    char t[64]; snprintf(t,sizeof t,T("vibration %d%%, period %d ms","вибрация %d%%, период %d мс"),percent,period);
    play(t,&e,ms);
}

int main(void) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
#endif
    setvbuf(stdout,NULL,_IONBF,0);
    printf("%s",T("Steering wheel force feedback test\n==================================\n\n",
                  "Проверка обратной связи руля\n============================\n\n"));
    printf("%s",T("Looking for the wheel... On some PCs Windows takes up to a few minutes to answer, please wait.\n",
                  "Ищу руль... На некоторых ПК Windows отвечает несколько минут, подожди.\n"));
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    if (!SDL_Init(SDL_INIT_JOYSTICK|SDL_INIT_HAPTIC)) { printf("SDL: %s\n",SDL_GetError()); quit(1); }
    int count=0;
    SDL_JoystickID *ids=SDL_GetJoysticks(&count);
    for (int i=0;ids && i<count && !joy;++i) if (looks_like_wheel(ids[i])) joy=SDL_OpenJoystick(ids[i]);
    SDL_free(ids);
    if (!joy) { printf("%s",T("No wheel found.\n","Руль не найден.\n")); quit(1); }
    if (!(haptic=SDL_OpenHapticFromJoystick(joy))) {
        printf(T("The wheel takes no force feedback: %s\n","Руль не принимает обратную связь: %s\n"),SDL_GetError());
        quit(1);
    }
    SDL_SetHapticGain(haptic,100);
    SDL_SetHapticAutocenter(haptic,0);
    printf(T("Wheel: %s\n\n","Руль: %s\n\n"),SDL_GetJoystickName(joy));
    printf("%s",T("Hold the wheel lightly, with two fingers, so that it can turn by itself. Hold it straight before each test.\n\n",
                  "Держи руль легко, двумя пальцами, чтобы он мог повернуться сам. Перед каждой проверкой поставь его прямо.\n\n"));
    for (;;) {
        printf("%s",T("1  constant force +20% (2 s)          2  constant force -20% (2 s)\n"
                      "3  spring +50% (5 s): turn the wheel and let go\n"
                      "4  spring -50% (5 s): turn the wheel and let go\n"
                      "5  damper +60% (5 s): turn the wheel\n"
                      "6  vibration 30%, 82 ms (1 s)         7  vibration 30%, 30 ms (1 s)\n"
                      "q  quit\n> ",
                      "1  постоянная сила +20% (2 с)        2  постоянная сила -20% (2 с)\n"
                      "3  пружина +50% (5 с): поверни руль и отпусти\n"
                      "4  пружина -50% (5 с): поверни руль и отпусти\n"
                      "5  демпфер +60% (5 с): покрути руль\n"
                      "6  вибрация 30%, 82 мс (1 с)         7  вибрация 30%, 30 мс (1 с)\n"
                      "q  выход\n> "));
        int c;
        do c=_getch(); while (c=='\r' || c=='\n');
        printf("%c\n",c);
        if (c=='q' || c=='Q' || c==27) break;
        switch (c) {
        case '1': constant(20,2000); break;
        case '2': constant(-20,2000); break;
        case '3': condition(SDL_HAPTIC_SPRING,50,60,5000); break;
        case '4': condition(SDL_HAPTIC_SPRING,-50,60,5000); break;
        case '5': condition(SDL_HAPTIC_DAMPER,60,80,5000); break;
        case '6': square(30,82,1000); break;
        case '7': square(30,30,1000); break;
        }
        printf("\n");
    }
    quit(0);
}
