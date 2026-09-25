/* Controller mapping checks. No disc and no hardware: an SDL virtual pad is
 * pressed, and the engine calls pad.c makes are recorded by the stubs below.
 *
 *   make pad_test
 */
#include <stdio.h>
#include <string.h>
#include "../src/pad.h"

static u8 down[256];                         /* keys the engine was told are held */
static int mx, my, mbtn[3], nchars;
static char typed[64];
static int edit_len;

void engine_key(Engine *e, int vk) { down[vk & 0xFF] = 1; }
void engine_keystate(Engine *e, int vk, int d) { down[vk & 0xFF] = (u8)d; }
void engine_mouse(Engine *e, int x, int y, int b, int d) { mx = x; my = y; if (b) mbtn[b] = d; }
void engine_char(Engine *e, int c)
{
    if (c == 8) { if (nchars) typed[--nchars] = 0; }
    else if (c >= 0x20 && nchars < edit_len) typed[nchars++] = (char)c, typed[nchars] = 0;
}
int engine_edit_len(Engine *e) { return nchars; }

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } else printf("  ok %s\n", what); } while (0)

static SDL_Joystick *vj[2];
static void press(int pad, int b, int on) { SDL_JoystickSetVirtualButton(vj[pad], b, (Uint8)on); }
static void axis(int pad, int a, int v) { SDL_JoystickSetVirtualAxis(vj[pad], a, (Sint16)v); }
static void step(Engine *e, unsigned ms) { SDL_JoystickUpdate(); pad_update(e, ms, NULL); }
static void release_all(Engine *e, int npad)
{
    for (int p = 0; p < npad; p++) {
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) press(p, b, 0);
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) axis(p, a, 0);
    }
    step(e, 16);
}

static int attach(void)
{
    SDL_VirtualJoystickDesc d; SDL_zero(d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX; d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.name = "virtual pad";
    return SDL_JoystickAttachVirtualEx(&d);
}

int main(void)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) { printf("SDL: %s\n", SDL_GetError()); return 1; }
    int i0 = attach();
    vj[0] = SDL_JoystickOpen(i0);
    pad_init();
    static Engine eng; Engine *e = &eng;

    printf("menu\n");
    snprintf(e->name, sizeof e->name, "JUNGMAIN.BIN");
    axis(0, SDL_CONTROLLER_AXIS_LEFTX, 32767); step(e, 16); step(e, 500);
    CHECK(mx > ENG_W / 2 + 100, "left stick moves the pointer right");
    axis(0, SDL_CONTROLLER_AXIS_LEFTX, 0);
    press(0, SDL_CONTROLLER_BUTTON_A, 1); step(e, 16);
    CHECK(mbtn[1] == 1, "A clicks");
    press(0, SDL_CONTROLLER_BUTTON_A, 0); step(e, 16);
    CHECK(mbtn[1] == 0, "A releases the click");
    press(0, SDL_CONTROLLER_BUTTON_B, 1); step(e, 16);
    CHECK(down[0x1B], "B is Escape");
    release_all(e, 1);

    printf("hippo hop\n");
    snprintf(e->name, sizeof e->name, "JUNGHIPP.BIN");
    press(0, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 1); press(0, SDL_CONTROLLER_BUTTON_A, 1); step(e, 16);
    CHECK(down[0x27] && down['X'], "d-pad right + A = right arrow + X (jump right)");
    release_all(e, 1);
    CHECK(!down[0x27] && !down['X'], "keys come back up");
    axis(0, SDL_CONTROLLER_AXIS_LEFTY, -30000); step(e, 16);
    CHECK(down[0x26], "left stick up = up arrow (jump forward)");
    axis(0, SDL_CONTROLLER_AXIS_LEFTY, -13000); step(e, 16);
    CHECK(down[0x26], "stick held past a third stays on (hysteresis)");
    axis(0, SDL_CONTROLLER_AXIS_LEFTY, -5000); step(e, 16);
    CHECK(!down[0x26], "stick back to centre lets go");
    release_all(e, 1);
    axis(0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767); step(e, 16);
    CHECK(mbtn[1] == 1, "right trigger clicks in a game (rules panels)");
    release_all(e, 1);

    printf("pinball\n");
    snprintf(e->name, sizeof e->name, "JUNGPINB.BIN");
    axis(0, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767); press(0, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 1); step(e, 16);
    CHECK(down['Z'] && down[0xBF], "LT = left flipper (Z), RB = right flipper (/)");
    CHECK(mbtn[2] == 0, "triggers are flippers here, not the mouse");
    release_all(e, 1);
    press(0, SDL_CONTROLLER_BUTTON_A, 1); step(e, 16);
    CHECK(down[0x0D], "A launches (Enter)");
    release_all(e, 1);

    printf("bug drop, one player\n");
    snprintf(e->name, sizeof e->name, "JUNGBUGD.BIN");
    static const u8 lay[2][6][2] = {             /* as the disc sets them */
        { {0, 0x10}, {0x28, 0x20}, {0x25, 0x80}, {0x27, 0x40}, {0xBC, 1}, {0xBE, 2} },
        { {0, 0x10}, {'X', 0x20}, {'Z', 0x80}, {'C', 0x40}, {'2', 1}, {'1', 2} } };
    memcpy(e->layout, lay, sizeof lay);
    for (int j = 1; j < 6; j++) { e->keymap[lay[1][j][0]].kbd = 1; e->keymap[lay[1][j][0]].code = lay[1][j][1]; }
    press(0, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 1); press(0, SDL_CONTROLLER_BUTTON_A, 1); step(e, 16);
    CHECK(down['Z'] && down['2'], "the live layout's keys: left and rotate");
    release_all(e, 1);

    printf("bug drop, two players on one pad (a Joy-Con each side)\n");
    for (int j = 1; j < 6; j++) { e->keymap[lay[0][j][0]].kbd = 0; e->keymap[lay[0][j][0]].code = lay[0][j][1]; }
    press(0, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 1); press(0, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 1);
    press(0, SDL_CONTROLLER_BUTTON_A, 1); axis(0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767); step(e, 16);
    CHECK(down[0x27] && down[0xBC], "player 1: d-pad right, LB rotates (, key)");
    CHECK(down['X'] && down['1'], "player 2: A is down (drop), RT rotates clockwise");
    CHECK(mbtn[1] == 0, "no mouse clicks while split");
    release_all(e, 1);
    axis(0, SDL_CONTROLLER_AXIS_RIGHTX, -32767); axis(0, SDL_CONTROLLER_AXIS_LEFTY, 32767); step(e, 16);
    CHECK(down['Z'] && down[0x28], "right stick moves player 2, left stick player 1");
    release_all(e, 1);

    printf("bug drop, two pads\n");
    vj[1] = SDL_JoystickOpen(attach());
    SDL_Event ev; while (SDL_PollEvent(&ev)) pad_event(&ev);
    press(0, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 1); press(1, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 1);
    press(1, SDL_CONTROLLER_BUTTON_B, 1); step(e, 16);
    CHECK(down[0x25] && down['Z'] && down['1'], "each pad is a whole player");
    CHECK(!down[0xBE], "pad 2's B does not rotate player 1");
    release_all(e, 2);

    printf("high-score name\n");
    snprintf(e->name, sizeof e->name, "JUNGHIPP.BIN");
    e->edit.on = 1; edit_len = 3;
    press(0, SDL_CONTROLLER_BUTTON_DPAD_UP, 1); step(e, 16);
    press(0, SDL_CONTROLLER_BUTTON_DPAD_UP, 0); step(e, 16);
    press(0, SDL_CONTROLLER_BUTTON_DPAD_UP, 1); step(e, 16);
    press(0, SDL_CONTROLLER_BUTTON_DPAD_UP, 0); step(e, 16);
    CHECK(!strcmp(typed, "B"), "up, up: the letter rolls A -> B in place");
    press(0, SDL_CONTROLLER_BUTTON_A, 1); step(e, 16); press(0, SDL_CONTROLLER_BUTTON_A, 0); step(e, 16);
    press(0, SDL_CONTROLLER_BUTTON_DPAD_DOWN, 1); step(e, 16); press(0, SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0); step(e, 16);
    CHECK(!strcmp(typed, "BB"), "A keeps it; the next letter starts from the last one");
    press(0, SDL_CONTROLLER_BUTTON_A, 1); step(e, 16); press(0, SDL_CONTROLLER_BUTTON_A, 0); step(e, 16);
    press(0, SDL_CONTROLLER_BUTTON_A, 1); step(e, 16); press(0, SDL_CONTROLLER_BUTTON_A, 0); step(e, 16);
    press(0, SDL_CONTROLLER_BUTTON_A, 1); step(e, 16); press(0, SDL_CONTROLLER_BUTTON_A, 0); step(e, 16);
    press(0, SDL_CONTROLLER_BUTTON_DPAD_UP, 1); step(e, 16); press(0, SDL_CONTROLLER_BUTTON_DPAD_UP, 0); step(e, 16);
    CHECK(!strcmp(typed, "BBB"), "a full field: rolling the letter never eats one already kept");
    press(0, SDL_CONTROLLER_BUTTON_B, 1); step(e, 16); press(0, SDL_CONTROLLER_BUTTON_B, 0); step(e, 16);
    CHECK(!strcmp(typed, "BB"), "B rubs out");
    press(0, SDL_CONTROLLER_BUTTON_START, 1); step(e, 16);
    CHECK(!down[0x0D], "Start sends Enter as a tap (key and character)");
    e->edit.on = 0; release_all(e, 1);

    SDL_Quit();
    printf(fails ? "pad_test: %d FAILED\n" : "pad_test: all passed\n", fails);
    return fails != 0;
}
