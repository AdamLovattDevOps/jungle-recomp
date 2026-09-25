/* Game controllers: see pad.h, and docs/CONTROLLERS.md for the layout per game. */
#include <string.h>
#include "pad.h"

#define MAX_PADS 4
#define VK_BACK   0x08
#define VK_ENTER  0x0D
#define VK_ESC    0x1B
#define VK_SPACE  0x20
#define VK_LEFT   0x25
#define VK_UP     0x26
#define VK_RIGHT  0x27
#define VK_DOWN   0x28
#define VK_SLASH  0xBF

enum { D_UP = 1, D_DOWN = 2, D_LEFT = 4, D_RIGHT = 8 };
enum { SC_MENU, SC_HIPPO, SC_BURPER, SC_PINBALL, SC_BUGDROP, SC_SLING };

typedef struct {
    SDL_GameController *gc;
    SDL_JoystickID id;
    u8 lstick, rstick;               /* stick directions held, for hysteresis */
    u8 prev[SDL_CONTROLLER_BUTTON_MAX];
} Pad;

static Pad pads[MAX_PADS];
static int npads;
static float px = ENG_W / 2, py = ENG_H / 2;   /* the pointer, canvas pixels */
static u8 held[256];                   /* keys the pads are holding down in the engine */
static int mheld[3];                   /* mouse buttons 1 left, 2 right */
static struct { int pending, idx, dir; unsigned wait; } ed;   /* name entry */

static void add(int index)
{
    if (npads >= MAX_PADS || !SDL_IsGameController(index)) return;
    SDL_GameController *gc = SDL_GameControllerOpen(index);
    if (!gc) return;
    SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    for (int i = 0; i < npads; i++) if (pads[i].id == id) { SDL_GameControllerClose(gc); return; }
    memset(&pads[npads], 0, sizeof pads[npads]);
    pads[npads].gc = gc; pads[npads].id = id; npads++;
}

void pad_init(void)
{
    for (int j = 0; j < SDL_NumJoysticks(); j++) add(j);
}

void pad_event(const SDL_Event *ev)
{
    if (ev->type == SDL_CONTROLLERDEVICEADDED) add(ev->cdevice.which);
    else if (ev->type == SDL_CONTROLLERDEVICEREMOVED) {
        for (int i = 0; i < npads; i++) if (pads[i].id == ev->cdevice.which) {
            SDL_GameControllerClose(pads[i].gc);
            memmove(&pads[i], &pads[i + 1], (size_t)(npads - i - 1) * sizeof pads[0]);
            npads--; break;
        }
    }
}

void pad_pointer(int x, int y) { px = (float)x; py = (float)y; }

static int btn(Pad *p, int b) { return SDL_GameControllerGetButton(p->gc, (SDL_GameControllerButton)b); }
static int trig(Pad *p, int a) { return SDL_GameControllerGetAxis(p->gc, (SDL_GameControllerAxis)a) > 12000; }
static int pressed(Pad *p, int b) { return btn(p, b) && !p->prev[b]; }

/* A stick as four keys: on past half way, off again below a third, so a
 * thumb resting near the edge does not chatter. */
static int stick(Pad *p, int ax, int ay, u8 *st)
{
    int x = SDL_GameControllerGetAxis(p->gc, (SDL_GameControllerAxis)ax);
    int y = SDL_GameControllerGetAxis(p->gc, (SDL_GameControllerAxis)ay);
    int on = 16000, off = 11000, d = 0;
    if (y < -((*st & D_UP) ? off : on)) d |= D_UP;
    if (y > ((*st & D_DOWN) ? off : on)) d |= D_DOWN;
    if (x < -((*st & D_LEFT) ? off : on)) d |= D_LEFT;
    if (x > ((*st & D_RIGHT) ? off : on)) d |= D_RIGHT;
    *st = (u8)d;
    return d;
}

static int dpad(Pad *p)
{
    return (btn(p, SDL_CONTROLLER_BUTTON_DPAD_UP) ? D_UP : 0) | (btn(p, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ? D_DOWN : 0)
         | (btn(p, SDL_CONTROLLER_BUTTON_DPAD_LEFT) ? D_LEFT : 0) | (btn(p, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? D_RIGHT : 0);
}

/* The face buttons as a second d-pad, for the right-hand player of a shared pad. */
static int diamond(Pad *p)
{
    return (btn(p, SDL_CONTROLLER_BUTTON_Y) ? D_UP : 0) | (btn(p, SDL_CONTROLLER_BUTTON_A) ? D_DOWN : 0)
         | (btn(p, SDL_CONTROLLER_BUTTON_X) ? D_LEFT : 0) | (btn(p, SDL_CONTROLLER_BUTTON_B) ? D_RIGHT : 0);
}

static void arrows(u8 *want, int d)
{
    if (d & D_UP) want[VK_UP] = 1;
    if (d & D_DOWN) want[VK_DOWN] = 1;
    if (d & D_LEFT) want[VK_LEFT] = 1;
    if (d & D_RIGHT) want[VK_RIGHT] = 1;
}

static int scene_of(const Engine *e)
{
    static const struct { const char *n; int s; } t[] = {
        { "JUNGHIPP", SC_HIPPO }, { "JUNGBURP", SC_BURPER }, { "JUNGPINB", SC_PINBALL },
        { "JUNGBUGD", SC_BUGDROP }, { "JUNGSHOT", SC_SLING } };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!strncmp(e->name, t[i].n, 8)) return t[i].s;
    return SC_MENU;
}

/* Bug Drop: the players' keys are the two redefinable keyboard layouts
 * (FUN_1008_4fde). A layout is live when the scene has armed its keys; both
 * live is a two-player game. Keys are looked up, never assumed, since the
 * Options screen can change them. */
static void layouts_live(const Engine *e, int live[2])
{
    live[0] = live[1] = 0;
    for (int v = 1; v < 256; v++) if (e->keymap[v].code) live[e->keymap[v].kbd & 1] = 1;
}

static void layout_key(const Engine *e, u8 *want, int kbd, int code)
{
    for (int j = 0; j < 6; j++)
        if (e->layout[kbd][j][1] == code && e->layout[kbd][j][0]) want[e->layout[kbd][j][0]] = 1;
}

static void bug_player(const Engine *e, u8 *want, int kbd, int d, int ccw, int cw)
{
    if (d & D_UP) layout_key(e, want, kbd, 0x10);
    if (d & D_DOWN) layout_key(e, want, kbd, 0x20);    /* drop faster */
    if (d & D_LEFT) layout_key(e, want, kbd, 0x80);
    if (d & D_RIGHT) layout_key(e, want, kbd, 0x40);
    if (ccw) layout_key(e, want, kbd, 0x01);           /* joystick button 1: counterclockwise */
    if (cw) layout_key(e, want, kbd, 0x02);            /* button 2: clockwise */
}

/* A high-score name, arcade style: up/down picks the letter in place,
 * A or right keeps it, B or left rubs out, Start finishes. */
static const char LETTERS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
static const char DIGITS[] = "0123456789";

static void ed_put(Engine *e, const char *set)
{
    int before = engine_edit_len(e);
    engine_char(e, (unsigned char)set[ed.idx]);
    ed.pending = engine_edit_len(e) > before;          /* a full field takes nothing */
}

static void ed_step(Engine *e, int by)
{
    const char *set = e->edit.digits ? DIGITS : LETTERS;
    int n = (int)strlen(set);
    if (ed.idx >= n) ed.idx = 0;
    if (ed.pending) { engine_char(e, VK_BACK); ed.idx = (ed.idx + by + n) % n; }
    ed_put(e, set);
}

static void ed_enter(Engine *e, int vk)
{
    engine_key(e, vk); engine_char(e, vk); engine_keystate(e, vk, 0);
}

static void name_entry(Engine *e, unsigned dt)
{
    int d = 0, keep = 0, rub = 0, done = 0, esc = 0;
    for (int i = 0; i < npads; i++) {
        Pad *p = &pads[i];
        d |= dpad(p) | stick(p, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &p->lstick);
        keep |= pressed(p, SDL_CONTROLLER_BUTTON_A);
        rub |= pressed(p, SDL_CONTROLLER_BUTTON_B);
        done |= pressed(p, SDL_CONTROLLER_BUTTON_START) || pressed(p, SDL_CONTROLLER_BUTTON_Y);
        esc |= pressed(p, SDL_CONTROLLER_BUTTON_BACK);
    }
    int fire = 0;                                    /* the held direction, with key repeat */
    if (d != ed.dir) { ed.dir = d; fire = d; ed.wait = 350; }
    else if (d && (ed.wait = ed.wait > dt ? ed.wait - dt : 0) == 0) { fire = d; ed.wait = 120; }
    if (fire & D_UP) ed_step(e, 1);
    else if (fire & D_DOWN) ed_step(e, -1);
    if ((fire & D_RIGHT) || keep) {
        if (ed.pending) ed.pending = 0;
        else if (keep) ed_put(e, e->edit.digits ? DIGITS : LETTERS);
    }
    if ((fire & D_LEFT) || rub) { engine_char(e, VK_BACK); ed.pending = 0; }
    if (done) { ed.pending = 0; ed_enter(e, VK_ENTER); }
    else if (esc) { ed.pending = 0; ed_enter(e, VK_ESC); }
}

static float axis_speed(int v)
{
    float a = v / 32767.0f, s = a < 0 ? -1.0f : 1.0f, m = a * s;
    if (m < 0.18f) return 0;
    m = (m - 0.18f) / 0.82f;
    return s * m * m * 900.0f;                         /* canvas pixels per second at full tilt */
}

void pad_update(Engine *e, unsigned dt, const PadHost *host)
{
    u8 want[256]; memset(want, 0, sizeof want);
    int wm[3] = { 0, 0, 0 };
    float vx = 0, vy = 0;
    int scene = scene_of(e), live[2] = { 0, 0 };
    if (scene == SC_BUGDROP) layouts_live(e, live);
    int split = live[0] && live[1] && npads == 1;    /* two players, one pad: a Joy-Con each side */

    if (e->edit.on) name_entry(e, dt);
    else ed.pending = 0, ed.dir = 0;

    for (int i = 0; i < npads && !e->edit.on; i++) {
        Pad *p = &pads[i];
        int l = dpad(p) | stick(p, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &p->lstick);
        int A = btn(p, SDL_CONTROLLER_BUTTON_A), B = btn(p, SDL_CONTROLLER_BUTTON_B);
        int X = btn(p, SDL_CONTROLLER_BUTTON_X), Y = btn(p, SDL_CONTROLLER_BUTTON_Y);
        int LB = btn(p, SDL_CONTROLLER_BUTTON_LEFTSHOULDER), RB = btn(p, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        int LT = trig(p, SDL_CONTROLLER_AXIS_TRIGGERLEFT), RT = trig(p, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        int lx = SDL_GameControllerGetAxis(p->gc, SDL_CONTROLLER_AXIS_LEFTX), ly = SDL_GameControllerGetAxis(p->gc, SDL_CONTROLLER_AXIS_LEFTY);
        int rx = SDL_GameControllerGetAxis(p->gc, SDL_CONTROLLER_AXIS_RIGHTX), ry = SDL_GameControllerGetAxis(p->gc, SDL_CONTROLLER_AXIS_RIGHTY);
        if (btn(p, SDL_CONTROLLER_BUTTON_START)) want[VK_ENTER] = 1;
        if (btn(p, SDL_CONTROLLER_BUTTON_BACK)) want[VK_ESC] = 1;

        if (split) {                                 /* left half Timon, right half Pumbaa */
            int r = diamond(p) | stick(p, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, &p->rstick);
            bug_player(e, want, 0, l, LB, LT);
            bug_player(e, want, 1, r, RB, RT);
            continue;
        }
        /* everywhere else: the right stick is the mouse, the triggers its buttons */
        vx += axis_speed(rx); vy += axis_speed(ry);
        if (RT || btn(p, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) wm[1] = 1;
        if (LT) wm[2] = 1;
        switch (scene) {
        case SC_HIPPO:                               /* arrows walk and hop, X jumps, Z runs */
        case SC_BURPER:                              /* arrows move and mondo-burp, X burps, Z+arrow tail swipes */
            arrows(want, l);
            if (A) want['X'] = 1;
            if (X) want[VK_SPACE] = 1;
            if (B || LB || RB) want['Z'] = 1;
            if (Y) want[VK_ENTER] = 1;
            break;
        case SC_PINBALL:                             /* shoulders and triggers are the flippers */
            wm[1] = wm[2] = 0;
            if (LB || LT) want['Z'] = 1;
            if (RB || RT) want[VK_SLASH] = 1;
            if (l & D_UP) want[VK_UP] = 1;           /* both flippers */
            if (A || Y) want[VK_ENTER] = 1;          /* launch */
            if (B || X || (l & D_DOWN)) want[VK_SPACE] = 1;   /* shake */
            if (btn(p, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) wm[1] = 1;
            break;
        case SC_BUGDROP: {
            int kbd = (live[0] && live[1]) ? (i & 1) : live[1] ? 1 : 0;   /* one pad each, or every pad the one player */
            if (live[0] && live[1] && i > 1) break;
            bug_player(e, want, kbd, l, A || LB, B || RB);
            if (!live[0] && !live[1]) { vx += axis_speed(lx); vy += axis_speed(ly); if (A) wm[1] = 1; }
            if (X) want[VK_SPACE] = 1;
            if (Y) want[VK_ENTER] = 1;
            break;
        }
        case SC_SLING:                               /* a mouse game: either stick aims, A fires */
            vx += axis_speed(lx); vy += axis_speed(ly);
            if (A) wm[1] = 1;
            if (B) wm[2] = 1;
            if (X) want[VK_SPACE] = 1;
            if (Y) want[VK_ENTER] = 1;
            break;
        default:                                     /* menus, Options, scores, credits */
            vx += axis_speed(lx); vy += axis_speed(ly);
            arrows(want, dpad(p));
            if (A) wm[1] = 1;
            if (B) want[VK_ESC] = 1;
            if (X) want[VK_SPACE] = 1;
            if (Y) want[VK_ENTER] = 1;
            break;
        }
    }
    for (int i = 0; i < npads; i++)
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) pads[i].prev[b] = (u8)btn(&pads[i], b);

    if (vx || vy) {
        float s = dt / 1000.0f;
        px += vx * s; py += vy * s;
        if (px < 0) px = 0; else if (px > ENG_W - 1) px = ENG_W - 1;
        if (py < 0) py = 0; else if (py > ENG_H - 1) py = ENG_H - 1;
        engine_mouse(e, (int)px, (int)py, 0, 0);
        if (host && host->warp) host->warp(host->ctx, (int)px, (int)py);
    }
    for (int v = 1; v < 256; v++) {                  /* only the changes reach the engine */
        if (want[v] && !held[v]) {
            if (e->fade.dir) continue;               /* the engine drops keys during a fade: press after it */
            engine_key(e, v); held[v] = 1;
        } else if (!want[v] && held[v]) { engine_keystate(e, v, 0); held[v] = 0; }
    }
    for (int b = 1; b <= 2; b++) if (wm[b] != mheld[b]) {
        if (wm[b] && e->fade.dir) continue;
        engine_mouse(e, (int)px, (int)py, b, wm[b]); mheld[b] = wm[b];
    }
}
