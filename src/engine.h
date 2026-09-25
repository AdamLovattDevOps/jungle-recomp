/* engine.h — the 7th Level runtime, reimplemented.
 *
 * One Engine runs one scene container at a time, the way JUNGLE.EXE does:
 * loading a container runs its resource 0, scripts call each other through
 * frames on the variable stack, sprites run their own type 13 programs, and a
 * scene transition swaps the container on the next tick.
 */
#ifndef ENGINE_H
#define ENGINE_H
#include "core.h"
#include "synth.h"

#define ENG_W 800
#define ENG_H 600
#define ENG_MAX_SPRITES 512
#define ENG_MAX_CELS 2048
#define ENG_MAX_BINDS 64

typedef struct {
    int   used, res;                 /* type 15 resource index */
    int   z, x, y, flipx, flipy, visible, running;
    int   cel[ENG_MAX_CELS], ncel;   /* type 1/10/16 handles, filtered as FUN_1008_5fdc does */
    int   comp[4], ncomp;            /* op 2 composite cels, 0 = single-cel mode */
    int   cur, next, first, last, fwd, autocyc;
    u8   *prog; int plen, pc, inited, gen;   /* gen counts program loads */
    u32   fdue, fper, mdue, mper; int catchup;
    int   brk;                       /* +0x5a: LOOP records fall through (S_004) */
    int   frozen;                    /* S_072: animation held */
    s16   user;                      /* +0x3e: a word scripts keep on the sprite (S_078/S_079) */
    u8    hit_hidden, hit_rect;      /* +0x5d / +0x5e from type 15 +0x13 / +0x0f (S_058) */

} Sprite;

typedef struct { const s16 *pcm; u32 n, pos; int loop, res; u16 done, tag; } EngVoice;
typedef struct { u8 kbd, armed, code; } EngKey;
typedef struct { u16 script; u8 code, buttons; } EngPlayer;

typedef struct {
    const char *dir;                 /* disc directory holding the .BIN files */
    Container   c; int open;
    char        name[16];
    u16        *mem;                 /* DS image, indexed by byte address / 2 */
    u16         fp;                  /* DAT_1020_10ac, frame pointer */
    s16         result;              /* DAT_1020_40ae */
    int         depth;
    u16         post_script;         /* DAT_1020_14e0, run once after load */
    u16         focus_script;        /* DAT_1020_14e6 */
    char        pending[16];         /* op 18 target, loaded on the next tick */
    int         bg;                  /* background bitmap index, -1 none */
    int         bgfill;              /* -1, or palette index to fill with */
    int         bind_vk[ENG_MAX_BINDS], bind_to[ENG_MAX_BINDS], nbind;   /* kept for reports */
    struct { u16 down, shift, ctrl, up; u8 off; } keys[256];           /* DAT_1020_4AD9 + vk * 11 */
    Sprite      spr[ENG_MAX_SPRITES];
    /* input: FUN_1008_2b04 / 2d2a */
    u16        *click;               /* per resource: type 15 +0 click script, op 4 may rewrite */
    u8         *click_off;           /* per resource: type 15 +0x0E disabled flag */
    struct { s16 l, t, r, b; u16 script; u8 off; } hot[64]; int nhot;   /* DAT_1020_55d9 */
    struct { s16 l, t, r, b; u16 enter, leave; } reg[40]; int nreg, inreg;   /* DAT_1020_48F9, op 61 */
    u16         mouse_script;        /* DAT_1020_14e2, op 64 */
    u16         hover_script;        /* DAT_1020_1518, builtin 0x6F */
    int         hovered;             /* DAT_1020_151a: sprite under the pointer, 0 none */
    u16         key_filter;          /* DAT_1020_14e4, builtin 0x69 */
    int         drag_x, drag_y;      /* DAT_1020_10ae/10b0: last pointer position while dragging */
    int         pressed;             /* DAT_1020_14dc: press-and-release object, 0 none */
    struct { u16 script, a1, a2; int argc; } q[40]; int nq;             /* DAT_1020_5771 */
    struct { u16 id, script; u32 due, every; int repeat; } tm[20]; int ntm;   /* DAT_1020_55f1 */
    struct { u16 script, tag; u32 due; } snd[16]; int nsnd;    /* completions for clips that cannot play */
    EngVoice    voice[16];           /* JUNGA01 channels */
    struct { const u8 *ev; u32 n, idx; int loops; u16 notify, tag; u32 wait; int on; } mus;   /* JUNGA01 type 4 track, A_028 */
    Synth       synth;               /* stands in for the MIDI mapper */
    struct { int dir, n; u32 t0; u8 faded; float level; u8 *snap; } fade;
    struct { int on, ri; u16 strvar, done; u8 maxlen, digits; char saved[128]; } edit;
    struct { int pending, host, x, y; } warp;   /* op 73 SetCursorPos, canvas pixels; host = the window pointer still to move */   /* op 54 text entry, DAT_1020_14de */   /* ops 10/11 palette fades; faded = DAT_1020_5a46 */
    s16        *audio; u32 naudio, cap_audio;   /* mixed 22050 Hz mono output, drained by the host */
    u32         audio_clock;         /* engine time the mixer has reached, ms */
    unsigned long long audio_samples;   /* samples mixed since engine time 0 */
    int         last_rand;           /* DAT_1020_0ce0 */
    struct { int a, b; u16 script, arg1, arg2; u8 repeat, hid_a, hid_b, samez; } col[64]; int ncol;   /* DAT_1020_55e5 */
    u8          keydown[256];        /* GetKeyState: held virtual keys */
    int         cursor;              /* DAT_1020_14EE, op 31 */
    int         timers_paused; u32 pause_start;   /* DAT_1020_5A5E, builtin 0x6D */
    /* game input: FUN_1008_4fde / 4e6e / 4efc / 3fe4 */
    u8          layout[2][6][2];     /* DS 0x3C: per keyboard, six {vk, code} */
    EngKey      keymap[256];         /* DAT_1020_40ba */
    u8          dirmask[2];          /* DAT_1020_48d1 */
    u8          devplayer[8];        /* DAT_1020_48cb: device -> player */
    EngPlayer   player[4];           /* 0x44BA + player * 0x200 */
    struct { char name[16]; u16 *img; char (*str)[128]; int nstr; } saved[16]; int nsaved;   /* per-container globals and strings */
    char      (*str)[128]; int nstr;  /* string variables, 128 bytes each (FUN_1008_e9c2) */
    char        ini_path[512];       /* 7THLEVEL.INI; empty = not persisted */
    struct { char sec[64], key[64], val[128]; } *ini; int nini;
    u32         now;
    int         trace;
    unsigned    unimpl_rec[256], unimpl_builtin[256];
    u8          mheld;               /* mouse buttons held, bit 1 left, bit 2 right */
    struct { s16 x, y; u32 since; int tries; } unstick[3];   /* pinball_unstick, per ball */
} Engine;

/* The frame as a list of draws, in painting order, for hosts that draw with the
 * GPU at any resolution (the high-resolution art cache). Canvas pixels. */
typedef struct {
    u8 kind;                         /* 0 fill the canvas, 1 bitmap, 2 text, 3 use engine_render (fade) */
    u8 flip, fill;                   /* flip: mirrored in x; fill: palette index for kind 0 */
    int res, x, y, w, h;             /* res: bitmap or text resource index */
    const u8 *px;                    /* 8-bit pixels, top-down, index 0 transparent */
    u32 version;                     /* text: changes when the text does */
} EngDraw;
int  engine_drawlist(Engine *e, EngDraw *out, int max);

int  engine_init(Engine *e, const char *disc_dir);
int  engine_load(Engine *e, const char *container);   /* FUN_1008_beca + bd72 */
void engine_tick(Engine *e, u32 now_ms);
void engine_key(Engine *e, int vk);                 /* key pressed: runs bindings */
void engine_keystate(Engine *e, int vk, int down);  /* held state for GetKeyState */
void engine_char(Engine *e, int c);
int  engine_edit_len(Engine *e);                    /* characters in the text field being typed into, -1 none */
s16  engine_builtin(Engine *e, int id, s16 *a, int argc);  /* call a script builtin, for tests */
int  engine_sprite_rect(Engine *e, int res, int *l, int *t, int *r, int *b);   /* canvas pixels; 0 if absent */                  /* a typed character (WM_CHAR): text entry */
void engine_mouse(Engine *e, int x, int y, int button, int down);  /* button 0 = move */
void engine_render(Engine *e, u8 *fb);                  /* ENG_W x ENG_H, 8-bit */
void engine_free(Engine *e);
u32  engine_audio(Engine *e, s16 *out, u32 max);         /* take mixed samples, 22050 Hz mono */
void engine_set_ini(Engine *e, const char *path);        /* load and persist settings and scores */
void engine_palette(Engine *e, u8 pal[256][3]);          /* the scene palette as displayed, with any fade applied */

#endif
