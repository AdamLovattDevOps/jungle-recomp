/* engine.c — the 7th Level runtime, reimplemented from JUNGLE.EXE and JUNGS01.DLL.
 *
 * Addresses in comments are the original functions each part reproduces. The
 * script VM keeps the original's data model: variables live in a 64 KB image of
 * the data segment, globals at 0x151E + i*2 and call-frame locals below the
 * frame pointer, so pointer arithmetic in scripts means what it meant.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "engine.h"
#include "blit.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#pragma GCC diagnostic pop

#define HANDLE_BASE  (0x10000 - 0x7531)     /* handle = index + 0x8ACF */
#define VAR_BASE     0x151E
#define VAR_SPLIT    0x13FE
#define IMM_BASE     0x159F
#define IMM_BIAS     0x7531
#define FRAME_BASE   0x27FC
#define FP_INIT      0x4058                 /* FUN_1008_bd72: DAT_1020_10ac = 0x4058 */
#define G_OWNER      0x1399                 /* sprite owner tag for op 20 statements */
#define ORG_X        (ENG_W / 2)            /* JUNGS01: SetViewportOrg(cx/2, cy/2) */
#define ORG_Y        (ENG_H / 2)

static struct { u8 *px; int w, h; } *g_bm;  /* decoded bitmap cache, per resource */
static int g_nbm;
static struct { s16 *pcm; u32 n; } *g_snd;          /* decoded clips, per resource */
typedef struct { char text[128]; u8 *px; int w, h, ox, oy, done; u32 version; } TextObj;
static TextObj *g_txt;                              /* type 16 text objects */
static unsigned char *g_font; static stbtt_fontinfo g_fi; static int g_font_ok;
static s16 g_sin[901];                              /* JUNGU01 DS:0x38, quarter-wave sine, scale 10000 */
static int32_t g_tan[1801];                         /* JUNGU01 DS:0x742, tangent 0..180 degrees, scale 10000 */

/* Load JUNGU01's sine table from the user's own DLL: segment 6 is its data
 * segment, and the table sits at offset 0x38 there. */
static void load_sine(const char *dir)
{
    char path[600]; snprintf(path, sizeof path, "%s/JUNGU01.DLL", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    u8 buf[0x10000]; size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
    if (n < 0x40) return;
    u32 ne = rd32(buf + 0x3C);
    if (ne + 0x40 > n) return;
    u16 segtab = rd16(buf + ne + 0x22), nseg = rd16(buf + ne + 0x1C), shift = rd16(buf + ne + 0x32);
    if (nseg < 6) return;
    const u8 *se = buf + ne + segtab + 5 * 8;        /* segment 6 */
    u32 off = (u32)rd16(se) << shift;
    if (off + 0x38 + 901 * 2 > n) return;
    for (int i = 0; i < 901; i++) g_sin[i] = (s16)rd16(buf + off + 0x38 + i * 2);
    if (off + 0x742 + 1801 * 4 <= n)
        for (int i = 0; i < 1801; i++) g_tan[i] = (int32_t)rd32(buf + off + 0x742 + i * 4);
}

static int norm_angle(int a) { a %= 3600; if (a < 0) a += 3600; return a; }   /* JUNGU01 1000:0dec */

static int jl_sine(int a)                            /* SINE, JUNGU01 1000:0e12 */
{
    a = norm_angle(a);
    int neg = a >= 1800; if (neg) a -= 1800;
    int r = a % 900; if (a >= 900) r = 900 - r;
    return neg ? -g_sin[r] : g_sin[r];
}

static long jl_tangent(int a)                        /* TANGENT, JUNGU01 1000:0e6e */
{
    a = norm_angle(a);
    int neg = a > 1800; if (neg) a -= 1800;
    return neg ? -(long)g_tan[a] : (long)g_tan[a];
}

static int round10k(long v)                          /* FUN_1008_6aa6: /10000, half away from zero */
{
    return (int)(v > 0 ? (v + 5000) / 10000 : (v - 5000) / 10000);
}

/* FUN_1008_6aec: where a ray from (x, y) at angle a leaves the box
 * [xmin, xmax] x [ymin, ymax]. Angles are the engine's: 0 is +y, 900 is +x. */
static void ray_to_box(int ymax, int xmax, int ymin, int xmin, int y, int x, int a, int *ox, int *oy)
{
    if (a == 0) { *ox = x; *oy = ymax; return; }
    if (a == 900) { *ox = xmax; *oy = y; return; }
    if (a == 1800) { *ox = x; *oy = ymin; return; }
    if (a == 2700) { *ox = xmin; *oy = y; return; }
    int q = a < 900 ? 0 : a < 1800 ? 1 : a < 2700 ? 2 : 3;
    int cx = q > 1 ? xmin : xmax, cy = (q != 1 && q != 2) ? ymax : ymin;
    int dy = cy - y, dx = cx - x;
    if (dx == 0 && dy == 0) { *ox = x; *oy = y; return; }
    double r = atan2((double)dx, (double)dy) * 572.9746936176986;   /* GETANGLE(dx, dy) */
    int ca = (int)r; if (ca < 0) ca += 3600;
    if (ca == a) { *ox = cx; *oy = cy; return; }
    int b = ca < a, a2 = a % 900;
    if (q == 1 || q == 3) { b = !b; if (!b) a2 = 900 - a2; dy = -dy; dx = -dx; }
    else if (b) a2 = 900 - a2;
    long t = jl_tangent(a2);
    if (b) { *ox = cx; *oy = round10k((long)dx * t) + y; }
    else   { *ox = round10k((long)dy * t) + x; *oy = cy; }
}

static int jl_cosine(int a)                          /* COSINE, JUNGU01 1000:0d18 */
{
    a = norm_angle(a - 900);
    int r = a % 900, neg = a < 1800;
    if (!neg) a -= 1800;
    if (a >= 900) r = 900 - r;
    return neg ? -g_sin[r] : g_sin[r];
}

/* ---- memory and operands ----------------------------------------------- */

static u16 addr_of(Engine *e, u16 raw)
{
    if (raw < VAR_SPLIT) return (u16)(VAR_BASE + raw * 2);
    return (u16)(e->fp + FRAME_BASE - raw * 2);
}
static s16 rdw(Engine *e, u16 a)          { return (s16)e->mem[a >> 1]; }
static void wrw(Engine *e, u16 a, s16 v)  { e->mem[a >> 1] = (u16)v; }

/* The operand rule used everywhere: below 0x159F a variable, else immediate. */
static s16 vdec(Engine *e, u16 raw)
{
    if (raw < IMM_BASE) return rdw(e, addr_of(e, raw));
    return (s16)(raw + IMM_BIAS);
}
static int as_index(s16 v) { return (u16)v; }   /* handle + 0x7531 wraps to index */

static int res_type(Engine *e, int idx)
{
    return (idx >= 0 && idx < e->c.ndir) ? e->c.dir[idx].type : -1;
}

static int run_script(Engine *e, u16 raw);
static void enqueue(Engine *e, u16 script, u16 a1, u16 a2);
static void engine_render_scene(Engine *e, u8 *fb);
static void fade_update(Engine *e);
static void edit_finish(Engine *e, int cancel);
static int msc_rand(void);
static void sprite_program(Engine *e, Sprite *s, int res13);
static void keyboard_enable(Engine *e, int on, int kbd);
static char *str_at(Engine *e, u16 h);
static int ci_cmp(const char *a, const char *b);
static s16 call_with(Engine *e, u16 script, int argc, const u16 *args);
static void sprite_rect(Engine *e, Sprite *s, int *l, int *t, int *r, int *b);
static int exec_record(Engine *e, const u8 *b, size_t pc, size_t n, int *adv);

/* ---- builtins (FUN_1008_0c52) ------------------------------------------ */

static u32 g_rand = 1;
static int msc_rand(void) { g_rand = g_rand * 214013u + 2531011u; return (int)((g_rand >> 16) & 0x7FFF); }

static Sprite *sprite_find(Engine *e, int res);
static Sprite *sprite_load(Engine *e, int res);
static int clone_resource(Engine *e, int src);
static int sprites_collide(Engine *e, Sprite *A, Sprite *B, int samez, int hid_a, int hid_b);
static int sprite_opaque_at(Engine *e, Sprite *s, int x, int y);

static void sprite_program_bytes(Engine *e, Sprite *s, const u8 *rec, int len, int replace);
static int set_range(Sprite *s, int first, int last, int start);
static void step_cel(Sprite *s);

/* FUN_1000_57d6: a script-issued sprite command. Mode 0 (S_069) appends it to
 * the sprite's program as a type 13 record with raw operands, starting the
 * program if it is idle; mode 1 (S_067) acts at once, replacing the program
 * for commands that run over time. */
static void sprite_command(Engine *e, Sprite *s, int now, int cmd, const s16 *v, int n)
{
    if (e->trace) printf("%*s     sprite %d cmd %d %s n %d\n", e->depth * 2, "", s->res, cmd, now ? "now" : "append", n);
    u8 r[32]; memset(r, 0, sizeof r);
#define RAW(k) (u16)((k) < n ? (u16)(v[k] - IMM_BIAS) : 0xFFFF)
#define PUT(o, x) do { u16 _x = (x); r[o] = (u8)_x; r[(o) + 1] = (u8)(_x >> 8); } while (0)
    PUT(0, cmd);
    switch (cmd) {
    case 1:                                          /* FUN_1000_5344: first, last, ms */
        PUT(2, RAW(0)); PUT(4, RAW(1)); PUT(8, n > 2 ? RAW(2) : 0xFFFF);
        sprite_program_bytes(e, s, r, 10, now); return;
    case 2: {                                        /* FUN_1000_53dc: cels..., ms */
        int nc = n - 1; if (nc < 1 || nc > 4) return;
        for (int k = 0; k < 4; k++) PUT(2 + k * 2, k < nc ? RAW(k) : 0);
        PUT(0x0A, RAW(nc)); r[0x0D] = (u8)nc;
        sprite_program_bytes(e, s, r, 14, now); return;
    }
    case 7:                                          /* FUN_1000_55f0 */
        if (now) { s->flipx = n > 0 && v[0]; s->flipy = n > 1 && v[1]; return; }
        PUT(2, n > 0 && v[0] ? 0x8AD0 : 0x8ACF); PUT(4, n > 1 && v[1] ? 0x8AD0 : 0x8ACF); PUT(6, 0x8ACF); PUT(8, 0x8ACF);
        sprite_program_bytes(e, s, r, 10, 0); return;
    case 8:                                          /* FUN_1000_5694 */
        if (now) { s->visible = 0; return; }
        sprite_program_bytes(e, s, r, 2, 0); return;
    case 15:                                         /* FUN_1000_5b04: start, first, last */
        if (now) { set_range(s, n > 1 ? v[1] : 0, n > 2 ? v[2] : 0, n > 0 ? v[0] : 0); return; }
        PUT(2, RAW(0)); PUT(4, RAW(1)); PUT(6, RAW(2));
        sprite_program_bytes(e, s, r, 8, 0); return;
    case 17: {                                       /* FUN_1000_5cb6 */
        u16 c = n > 0 ? (u16)v[0] : 0xFFFF;
        if (now) { if (c != 0xFFFF && c < s->ncel) { s->next = c; step_cel(s); } s->visible = 1; return; }
        PUT(2, c); sprite_program_bytes(e, s, r, 4, 0); return;
    }
    case 5:                                          /* FUN_1000_54b8: midX, midY, endX, endY, steps, rel */
        PUT(6, RAW(0)); PUT(8, RAW(1)); PUT(0x0A, RAW(2)); PUT(0x0C, RAW(3)); PUT(2, RAW(4));
        r[4] = (u8)(n > 5 && v[5]);
        sprite_program_bytes(e, s, r, 30, now); return;
    case 9:                                          /* FUN_1000_56c8: x, y, steps, speedmode, rel */
        PUT(6, RAW(0)); PUT(8, RAW(1)); PUT(2, RAW(2));
        r[4] = (u8)(n > 3 && v[3]); r[5] = (u8)(n > 4 && v[4]);
        sprite_program_bytes(e, s, r, 22, now); return;
    case 11:                                         /* FUN_1000_59d4: ms */
        if (now) return;
        PUT(2, RAW(0)); sprite_program_bytes(e, s, r, 6, 0); return;
    case 12: {                                       /* FUN_1000_5a1a: x, y [, rel] */
        if (n < 2) return;
        int rel = n > 2 && v[2];
        if (now) { if (rel) { s->x += v[0]; s->y += v[1]; } else { s->x = v[0]; s->y = v[1]; } return; }
        PUT(2, RAW(0)); PUT(4, RAW(1)); r[6] = (u8)rel;
        sprite_program_bytes(e, s, r, 8, 0); return;
    }
    case 13:                                         /* FUN_1000_5aaa: catch-up flag */
        if (now) { s->catchup = n > 0 && v[0]; s->fdue = s->mdue = e->now; return; }
        r[2] = (u8)(n > 0 && v[0]); sprite_program_bytes(e, s, r, 4, 0); return;
    case 14:                                         /* FUN_1000_5b78: frame period */
        if (now) { s->fper = (u16)(n > 0 ? v[0] : 0); s->fdue = e->now; return; }
        PUT(2, RAW(0)); sprite_program_bytes(e, s, r, 4, 0); return;
    case 16:                                         /* FUN_1000_5bec: move period [, frame period] */
        if (now) { s->mper = (u16)(n > 0 ? v[0] : 0); if (n > 1) s->fper = (u16)v[1]; return; }
        PUT(2, RAW(0)); PUT(4, n > 1 ? RAW(1) : 0);
        sprite_program_bytes(e, s, r, 6, 0); return;
    case 21: {                                       /* FUN_1000_592e: event 0x15, a call */
        if (n < 1) return;
        int argc = n - 1 > 4 ? 4 : n - 1;
        PUT(2, RAW(0)); PUT(4, argc);
        for (int k = 0; k < argc; k++) PUT(6 + k * 2, RAW(1 + k));
        sprite_program_bytes(e, s, r, 14, now); return;
    }
    default: e->unimpl_builtin[0x5A + !now]++; return;
    }
#undef RAW
#undef PUT
}

/* Each builtin pops its own arguments and leaves its result where the
 * function id was. Only the ones scripts have been seen to need are here;
 * the rest return 0 and are counted, so the gaps stay visible. */
static s16 builtin(Engine *e, int id, s16 *a, int argc)
{
    switch (id) {
    case 0x2E: {                                     /* random integer in [lo, hi] */
        int lo = argc > 0 ? a[0] : 0, hi = argc > 1 ? a[1] : 0;
        if (lo > hi) { int t = lo; lo = hi; hi = t; }
        return (s16)(lo + msc_rand() % (hi - lo + 1));
    }
    case 0x2F: return (s16)(argc > 0 && e->keydown[(u8)a[0]]);   /* GetKeyState(vk) < 0 */
    case 0x5A: case 0x5B: {                          /* S_067 / S_069: a sprite command, FUN_1000_57d6 */
        Sprite *sp = argc > 0 ? sprite_load(e, (u16)a[0]) : NULL;   /* FUN_1008_669c faults it in */
        if (!sp || argc < 2) return 0;
        sprite_command(e, sp, id == 0x5A, a[1], a + 2, argc - 2);
        return 0;
    }
    case 0x69: {                                     /* swap the keyboard filter, returns the old one */
        s16 old = e->key_filter ? (s16)(e->key_filter + IMM_BIAS) : 0;
        e->key_filter = (argc > 0 && a[0]) ? (u16)(a[0] - IMM_BIAS) : 0;
        return old;
    }
    case 0x6F: {                                     /* swap the hover script, FUN_1008_3604 */
        s16 old = e->hover_script ? (s16)(e->hover_script + IMM_BIAS) : 0;
        if (e->hover_script && e->hovered) {         /* FUN_1008_2278: leave the current one */
            enqueue(e, e->hover_script, (u16)(e->hovered - IMM_BIAS), (u16)(0 - IMM_BIAS));
            e->hovered = 0;
        }
        e->hover_script = (argc > 0 && a[0]) ? (u16)(a[0] - IMM_BIAS) : 0;
        return old;
    }
    case 0x6B: {                                     /* redefine a layout key, FUN_1008_5066 */
        if (argc < 3) return 0;
        int kbd = (u8)a[0] & 1, which = a[1], slot = -1;
        if (e->trace) printf("builtin 0x6B kbd %d which %d vk 0x%02x\n", kbd, which, (u8)a[2]);
        switch (which) { case 3: slot = 0; break; case 5: slot = 1; break; case 7: slot = 2; break;
                         case 6: slot = 3; break; case 1: slot = 4; break; case 2: slot = 5; break; }
        if (slot >= 0) e->layout[kbd][slot][0] = (u8)a[2];
        return 0;
    }
    case 0x65: {                                     /* RESCOPYRESOURCE: clone a type 15, FUN_1008_6418 */
        int src = argc > 0 ? (u16)a[0] : -1;
        if (res_type(e, src) != 15) return 0;
        int ni = clone_resource(e, src);
        if (ni >= 0) sprite_load(e, ni);
        return (s16)(ni >= 0 ? ni : 0);
    }
    case 0x44: {                                     /* S_063: the sprite's visible flag (+0x4E) */
        Sprite *sp = argc > 0 ? sprite_find(e, (u16)a[0]) : NULL;
        return (s16)(sp && sp->visible);
    }
    case 0x83: {                                     /* S_078: the sprite's user word (+0x3E) */
        Sprite *sp = argc > 0 ? sprite_find(e, (u16)a[0]) : NULL;
        return sp ? sp->user : 0;
    }
    case 0x84: {                                     /* S_079: set the sprite's user word */
        Sprite *sp = argc > 0 ? sprite_find(e, (u16)a[0]) : NULL;
        if (sp && argc > 1) sp->user = a[1];
        return 0;
    }
    case 0x79: case 0x7A: case 0x7B: {               /* long arithmetic on a base-10000 pair */
        if (argc < 4) return 0;                      /* value = hi * 10000 + lo, as two 16-bit globals */
        u16 ph = (u16)a[2], pl = (u16)a[3];
        long A = (long)a[0] * 10000 + a[1];
        long P = (long)rdw(e, ph) * 10000 + rdw(e, pl), L;
        if (id == 0x79) L = P + A;
        else if (id == 0x7A) L = (long)(int)((unsigned)A * (unsigned)P);   /* 32-bit wrap, as __aFlmul */
        else L = A ? P / A : 0;
        wrw(e, ph, (s16)(L / 10000)); wrw(e, pl, (s16)(L % 10000));
        return 0;
    }
    case 0x31: {                                     /* GETANGLE(x1,y1,x2,y2), JUNGU01 1000:0d76 */
        if (argc < 4) return 0;
        double r = atan2((double)(a[2] - a[0]), (double)(a[3] - a[1])) * 572.9746936176986;
        int v = (int)r; if (v < 0) v += 3600;        /* fistp via __ftol truncates */
        return (s16)v;
    }
    case 0x41: return (s16)(argc > 0 ? jl_cosine(a[0]) : 0);
    case 0x42: return (s16)(argc > 0 ? jl_sine(a[0]) : 0);
    case 0x7E: {                                     /* DISTANCE, JUNGU01 1000:107a */
        if (argc < 4) return 0;
        long dx = a[3] - a[1], dy = a[2] - a[0];
        return (s16)(int)sqrt((double)(dx * dx + dy * dy));
    }
    case 0x7F: return (s16)(argc > 1 ? norm_angle(a[1] * 2 - a[0]) : 0);   /* REFLECTANGLE */
    case 0x85: {                                     /* GETQUADRANT */
        int q = argc > 0 ? a[0] : 0;
        return (s16)(q < 900 ? 0 : q < 1800 ? 1 : q < 2700 ? 2 : 3);
    }
    case 0x72: case 0x73: {                          /* S_074 / S_075: a point on a line or an arc */
        int need = id == 0x72 ? 8 : 10;              /* (&x, &y, i, n, x0, y0, [midX, midY,] x1, y1) */
        if (argc < need) return 0;
        long i = a[2], n = a[3], x0 = a[4], y0 = a[5], X, Y;
        if (n == 0) { X = x0; Y = y0; }
        else if (id == 0x72) {                       /* FUN_1000_3e82: start + round(delta * i / n) */
            long dx = (long)(a[6] - x0) * i, dy = (long)(a[7] - y0) * i;
            X = x0 + (dx >= 0 ? (dx + n / 2) / n : -((-dx + n / 2) / n));
            Y = y0 + (dy >= 0 ? (dy + n / 2) / n : -((-dy + n / 2) / n));
        } else {                                     /* FUN_1000_1754: the quadratic through mid */
            long t = (i * 1024 + n / 2) / n;
            long mx = a[6] - x0, my = a[7] - y0, ex = a[8] - x0, ey = a[9] - y0;
            X = x0 + ((t * (1024 - t) * (4 * mx - ex) + t * t * ex) >> 20);
            Y = y0 + ((t * (1024 - t) * (4 * my - ey) + t * t * ey) >> 20);
        }
        wrw(e, (u16)a[0], (s16)X); wrw(e, (u16)a[1], (s16)Y);
        return 0;
    }
    case 0x7C: {                                     /* COLLIDE, JUNGU01 1000:0ebc: two bodies, equal masses */
        if (argc < 8) return 0;                      /* (x1, y1, x2, y2, &speed1, &angle1, &speed2, &angle2) */
        u16 ps1 = (u16)a[4], pa1 = (u16)a[5], ps2 = (u16)a[6], pa2 = (u16)a[7];
        int sp1 = rdw(e, ps1), sp2 = rdw(e, ps2);
        if (!sp1 && !sp2) return 0;
        int th;
        if (a[1] != 30000) {                         /* normal from the centres, else given in a[0] */
            double r = atan2((double)(a[2] - a[0]), (double)(a[3] - a[1])) * 572.9746936176986;
            th = (int)r; if (th < 0) th += 3600; th = norm_angle(th);
        } else th = a[0];
        wrw(e, pa1, (s16)(rdw(e, pa1) - th));        /* rotate both into the normal's frame */
        wrw(e, pa2, (s16)(rdw(e, pa2) - th));
        int an1 = rdw(e, pa1), an2 = rdw(e, pa2);
        double c1 = (double)jl_cosine(an1) * sp1 / 10000.0, s1 = (double)jl_sine(an1) * sp1 / 10000.0;
        double c2 = (double)jl_cosine(an2) * sp2 / 10000.0, s2 = (double)jl_sine(an2) * sp2 / 10000.0;
        wrw(e, ps1, (s16)(int)sqrt(c2 * c2 + s1 * s1));   /* swap the normal components */
        wrw(e, ps2, (s16)(int)sqrt(s2 * s2 + c1 * c1));
        int n1 = (int)(atan2(s1, c2) * 572.9746936176986), n2 = (int)(atan2(s2, c1) * 572.9746936176986);
        wrw(e, pa1, (s16)norm_angle(n1 + th));
        wrw(e, pa2, (s16)norm_angle(n2 + th));
        return 0;
    }
    case 0x8A: {                                     /* S_080: is (x, y) on the sprite? */
        Sprite *sp = argc > 2 ? sprite_find(e, (u16)a[0]) : NULL;
        if (!sp || !(sp->visible || sp->hit_hidden)) return 0;
        int l, t, rr, bb; sprite_rect(e, sp, &l, &t, &rr, &bb);
        int x = a[1], y = a[2];
        if (x < l || x >= rr || y < t || y >= bb) return 0;
        return (s16)(sp->hit_rect ? 1 : sprite_opaque_at(e, sp, x, y));
    }
    case 0x86: {                                     /* S_048 on two sprites: do they overlap? */
        if (argc < 5) return 0;                      /* (spriteA, spriteB, hidA, hidB, samez) */
        Sprite *A = sprite_find(e, (u16)a[0]), *B = sprite_find(e, (u16)a[1]);
        if (!A || !B) return 0;
        return (s16)sprites_collide(e, B, A, a[4] != 0, a[3] != 0, a[2] != 0);
    }
    case 0x39: case 0x3A: {                          /* lstrcmp / lstrcmpi */
        char *x = argc > 0 ? str_at(e, (u16)a[0]) : NULL, *y = argc > 1 ? str_at(e, (u16)a[1]) : NULL;
        if (!x || !y) return 0;
        int c = id == 0x39 ? strcmp(x, y) : ci_cmp(x, y);
        if (e->trace) fprintf(stderr, "strcmp \"%s\" \"%s\" = %d\n", x, y, c);
        return (s16)(c < 0 ? -1 : c > 0);
    }
    case 0x63: { char *x = argc > 0 ? str_at(e, (u16)a[0]) : NULL; return (s16)(x ? (int)strlen(x) : 0); }
    case 0x74: {                                     /* character at an index */
        char *x = argc > 0 ? str_at(e, (u16)a[0]) : NULL;
        if (!x || argc < 2 || (u16)a[1] > strlen(x)) return 0;
        return (s16)(signed char)x[(u16)a[1]];
    }
    case 0x75: {                                     /* set a character */
        if (argc < 3 || ((u16)a[0] & 0x8000)) return 0;
        char *x = str_at(e, (u16)a[0]);
        if (x && (u16)a[1] < 0x7F) x[(u16)a[1]] = (char)a[2];
        return 0;
    }
    case 0x76: { char *x = argc > 0 ? str_at(e, (u16)a[0]) : NULL; return (s16)(x ? atoi(x) : 0); }
    case 0x66: {                                     /* RESDELETERESOURCE on a clone, FUN_1008_645a */
        Sprite *sp = argc > 0 ? sprite_find(e, (u16)a[0]) : NULL;
        if (!sp) return 0;
        free(sp->prog); memset(sp, 0, sizeof *sp);
        return 1;
    }
    case 0x6C: {                                     /* S_072 (sprite, on): hold or release animation, 0 = all */
        int which = argc > 0 ? (u16)a[0] : 0, on = argc > 1 && a[1] != 0;
        for (int i = 0; i < ENG_MAX_SPRITES; i++) {  /* JUNGS01 FUN_1000_60f0 */
            Sprite *s = &e->spr[i];
            if (!s->used || (which && s->res != which) || s->frozen == on) continue;
            if (on) s->held_at = e->now;
            else s->fdue += e->now - s->held_at;     /* the frame timer resumes where it was held */
            s->frozen = on;
        }
        return 0;
    }
    case 0x6D: {                                     /* pause or resume the timers, FUN_1008_e04a */
        int on = argc > 0 && a[0] != 0;
        if (on && !e->timers_paused) e->pause_start = e->now;
        if (!on && e->timers_paused) {
            u32 d = e->now - e->pause_start;
            for (int k = 0; k < e->ntm; k++) e->tm[k].due += d;
        }
        e->timers_paused = on;
        return 0;
    }
    case 0x81: return e->mouse_script ? (s16)(e->mouse_script + IMM_BIAS) : 0;
    case 0x62: return (s16)(argc > 0 ? (a[0] < 0 ? -a[0] : a[0]) : 0);
    case 0x80: return (s16)(argc > 0 && a[0] > 0 ? (int)sqrt((double)a[0]) : 0);   /* SQUAREROOT */
    case 0x87: return (s16)(argc > 1 ? (a[0] < a[1] ? a[0] : a[1]) : 0);
    case 0x88: return (s16)(argc > 1 ? (a[0] > a[1] ? a[0] : a[1]) : 0);
    case 0x8B: return 5;                             /* GetDriveType of the disc: CD-ROM */
    case 0x8C: return 256;                           /* free memory, 64 KB units: 16 MB */
    default:
        if (id >= 0 && id < 256) e->unimpl_builtin[id]++;
        return 0;
    }
}

/* ---- strings and the INI (FUN_1008_e9c2, 9b20, aa68) ---------------------- */

static int ci_cmp(const char *a, const char *b)     /* lstrcmpi, portable */
{
    for (;; a++, b++) {
        int x = (u8)*a, y = (u8)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
}

/* A handle with bit 0x8000 is a constant at that byte offset in the string
 * table (key resource 4); otherwise it names string variable (h - 1). */
static char *str_at(Engine *e, u16 h)
{
    static char empty[128];
    if (!h) return NULL;
    if (h & 0x8000) {
        u32 off = rd32(e->c.data + OFF_TABLES + 4 * 8), len = rd32(e->c.data + OFF_TABLES + 4 * 8 + 4);
        u32 o = h & 0x7FFF;
        if (o >= len) return NULL;
        return (char *)(e->c.data + off + o);
    }
    int i = (h & 0x7FFF) - 1;
    if (i < 0 || i >= e->nstr) { empty[0] = 0; return empty; }
    return e->str[i];
}

/* A minimal wsprintf: %[-][0][width]{d,i,u,x,X,c,s,ld,lu}. */
static void wsprintf16(char *out, size_t cap, const char *fmt, const s16 *num, char **strs, int nargs, const u8 *kinds)
{
    size_t o = 0; int ai = 0;
    for (const char *f = fmt; *f && o + 1 < cap; f++) {
        if (*f != '%') { out[o++] = *f; continue; }
        char spec[16]; int k = 0; spec[k++] = '%'; f++;
        while (*f && strchr("-0123456789", *f) && k < 12) spec[k++] = *f++;
        int is_long = 0; if (*f == 'l') { is_long = 1; f++; }
        char c = *f; if (!c) break;
        if (c == '%') { out[o++] = '%'; continue; }
        spec[k++] = c == 'X' || c == 'x' || c == 'u' || c == 'd' || c == 'i' || c == 'c' || c == 's' ? c : 'd'; spec[k] = 0;
        char tmp[160]; tmp[0] = 0;
        if (ai < nargs) {
            if (c == 's') snprintf(tmp, sizeof tmp, spec, kinds[ai] == 0 && strs[ai] ? strs[ai] : "");
            else if (c == 'u' || c == 'x' || c == 'X') snprintf(tmp, sizeof tmp, spec, (unsigned)(u16)num[ai]);
            else if (c == 'c') snprintf(tmp, sizeof tmp, spec, (int)(u8)num[ai]);
            else snprintf(tmp, sizeof tmp, spec, (int)num[ai]);
            (void)is_long; ai++;
        }
        for (char *t = tmp; *t && o + 1 < cap; t++) out[o++] = *t;
    }
    out[o] = 0;
}

static void ini_save(Engine *e)
{
    if (!e->ini_path[0]) return;
    FILE *f = fopen(e->ini_path, "w");
    if (!f) return;
    for (int i = 0; i < e->nini; i++) {
        int first = 1;
        for (int j = 0; j < i; j++) if (!strcmp(e->ini[j].sec, e->ini[i].sec)) { first = 0; break; }
        if (!first) continue;
        fprintf(f, "[%s]\n", e->ini[i].sec);
        for (int j = i; j < e->nini; j++) if (!strcmp(e->ini[j].sec, e->ini[i].sec)) fprintf(f, "%s=%s\n", e->ini[j].key, e->ini[j].val);
        fprintf(f, "\n");
    }
    fclose(f);
}

static const char *ini_get(Engine *e, const char *sec, const char *key)
{
    for (int i = 0; i < e->nini; i++)
        if (!ci_cmp(e->ini[i].sec, sec) && !ci_cmp(e->ini[i].key, key)) return e->ini[i].val;
    return NULL;
}

static void ini_put(Engine *e, const char *sec, const char *key, const char *val)
{
    int i;
    for (i = 0; i < e->nini; i++) if (!ci_cmp(e->ini[i].sec, sec) && !ci_cmp(e->ini[i].key, key)) break;
    if (i == e->nini) {
        void *n = realloc(e->ini, sizeof *e->ini * (size_t)(e->nini + 1));
        if (!n) return;
        e->ini = n; e->nini++;
        snprintf(e->ini[i].sec, 64, "%s", sec); snprintf(e->ini[i].key, 64, "%s", key);
    }
    snprintf(e->ini[i].val, 128, "%s", val);
    ini_save(e);
}

void engine_set_ini(Engine *e, const char *path)
{
    snprintf(e->ini_path, sizeof e->ini_path, "%s", path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512], sec[64] = "";
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') { char *z = strchr(line, ']'); if (z) *z = 0; strncpy(sec, line + 1, sizeof sec - 1); sec[sizeof sec - 1] = 0; continue; }
        char *eq = strchr(line, '=');
        if (!eq || !sec[0]) continue;
        *eq = 0;
        char saved[512]; snprintf(saved, sizeof saved, "%s", e->ini_path); e->ini_path[0] = 0;   /* no save while loading */
        ini_put(e, sec, line, eq + 1);
        snprintf(e->ini_path, sizeof e->ini_path, "%s", saved);
    }
    fclose(f);
}

/* ---- expression evaluator (FUN_1008_1bf2) ------------------------------ */

static s16 eval(Engine *e, const u8 *code, size_t n)
{
    s16 S[96]; int sp = 0; size_t p = 0;
    memset(S, 0, sizeof S);
#define T S[sp - 1]
#define U S[sp - 2]
#define NEED(k) do { if (sp < (k)) return 0; } while (0)
    while (p < n) {
        int op = code[p++];
        if (op == 0) return sp ? T : 0;
        if (sp >= 94) return 0;
        u16 w = (p + 2 <= n) ? rd16(code + p) : 0;
        switch (op) {
        case 1:  S[sp++] = (s16)w; p += 2; break;
        case 2:  S[sp++] = vdec(e, w); p += 2; break;
        case 4:  S[sp++] = (w < IMM_BASE) ? (s16)addr_of(e, w) : (s16)(w + IMM_BIAS); p += 2; break;
        case 3:  NEED(2); U = rdw(e, (u16)(U + T * 2)); sp--; break;
        case 5:  NEED(1); T = rdw(e, (u16)T); break;
        case 6:  NEED(2); U = (s16)(U + T * 2); sp--; break;
        case 8:  NEED(2); wrw(e, (u16)U, T); U = T; sp--; break;
        case 12: {                                   /* builtin / external call, FUN_1008_1ad4 */
            int argc = (p + 4 <= n) ? rd16(code + p + 2) : 0;
            p += 4;
            if (sp < argc + 1) return 0;
            int id = (u16)S[sp - 1 - argc];
            s16 *a = &S[sp - argc];                  /* a[0] is the first argument */
            s16 v = builtin(e, id, a, argc);
            if (e->trace && getenv("ENGINE_BTRACE")) { printf("B%02x(", id); for (int q = 0; q < argc; q++) printf(q ? ",%d" : "%d", a[q]); printf(")=%d\n", v); }
            sp -= argc;
            T = v;
            break;
        }
        case 13: {                                   /* script call, FUN_1008_1b5e */
            int locals = (p + 4 <= n) ? rd16(code + p) : 0;
            int argc   = (p + 4 <= n) ? rd16(code + p + 2) : 0;
            p += 4;
            if (sp < argc + 1) return 0;
            u16 a = (u16)(e->fp - (locals + argc) * 2);
            for (int k = 0; k < argc; k++) { a += 2; wrw(e, a, S[--sp]); }
            s16 saved = e->result;
            e->fp = (u16)(e->fp - locals * 2);
            e->result = 0;
            s16 target = S[--sp];
            run_script(e, (u16)(target - IMM_BIAS));
            S[sp++] = e->result;
            e->result = saved;
            e->fp = (u16)(e->fp + locals * 2);
            break;
        }
        case 14: NEED(1); T = (s16)-T; break;
        case 15: NEED(1); T = (s16)(T == 0); break;
        case 16: NEED(1); T = (s16)~T; break;
        case 33: NEED(1); { u16 a = (u16)T; wrw(e, a, rdw(e, a) + 1); T = rdw(e, a); } break;
        case 34: NEED(1); { u16 a = (u16)T; T = rdw(e, a); wrw(e, a, T + 1); } break;
        case 35: NEED(1); { u16 a = (u16)T; wrw(e, a, rdw(e, a) - 1); T = rdw(e, a); } break;
        case 36: NEED(1); { u16 a = (u16)T; T = rdw(e, a); wrw(e, a, T - 1); } break;
        case 37:                                     /* short-circuit: jump if true */
            NEED(1);
            if (T != 0) { p += (s16)w; S[sp++] = 1; } else p += 2;
            break;
        case 38:                                     /* short-circuit: jump if false */
            NEED(1);
            if (T == 0) { p += (s16)w; S[sp++] = 0; } else p += 2;
            break;
        case 39: NEED(1); S[sp] = rdw(e, (u16)T); sp++; break;
        default:
            if (op >= 17 && op <= 32) {
                NEED(2);
                int x = U, y = T, r = 0;
                switch (op) {
                case 17: r = x + y; break;
                case 18: r = x - y; break;
                case 19: r = x * y; break;
                case 20: r = y ? x / y : 0; break;
                case 21: r = y ? x % y : 0; break;
                case 22: r = x == y; break;
                case 23: r = x <= y; break;          /* from the assembly, not the decompile */
                case 24: r = x <  y; break;
                case 25: r = x >= y; break;
                case 26: r = x >  y; break;
                case 27: r = x != y; break;
                case 28: r = x && y; break;
                case 29: r = x || y; break;
                case 30: r = x & y;  break;
                case 31: r = x | y;  break;
                case 32: r = x ^ y;  break;
                }
                sp--; T = (s16)r;
            } else return sp ? T : 0;                /* unknown byte: stop quietly */
        }
    }
#undef T
#undef U
#undef NEED
    return sp ? S[sp - 1] : 0;
}

/* ---- bitmaps ------------------------------------------------------------ */

static const u8 *bitmap(Engine *e, int idx, int *w, int *h)
{
    if (idx < 0 || idx >= g_nbm || res_type(e, idx) != 1) return NULL;
    if (!g_bm[idx].px) {
        if (!bitmap_decode(resource_bytes(&e->c, &e->c.dir[idx]), e->c.dir[idx].size,
                           &g_bm[idx].w, &g_bm[idx].h, &g_bm[idx].px)) return NULL;
        /* Decoded bitmaps are bottom-up, as a DIB is. The engine composes
         * top-down, so flip once here. */
        int bw = g_bm[idx].w, bh = g_bm[idx].h;
        u8 *tmp = malloc((size_t)bw);
        for (int y = 0; y < bh / 2 && tmp; y++) {
            u8 *a = g_bm[idx].px + (size_t)y * bw, *b = g_bm[idx].px + (size_t)(bh - 1 - y) * bw;
            memcpy(tmp, a, (size_t)bw); memcpy(a, b, (size_t)bw); memcpy(b, tmp, (size_t)bw);
        }
        free(tmp);
    }
    *w = g_bm[idx].w; *h = g_bm[idx].h;
    return g_bm[idx].px;
}

/* ---- text objects (type 16, FUN_1008_d662 / d346 / d25e) ------------------ */

/* Render a type 16's text as an 8-bit cel: GDI draws white text on black and
 * MAPBYTES turns white into the record's colour index; black is transparent.
 * The record: +0 colour, +2 face, +4 initial text, +6 size, +8/+10 origin,
 * +12/+14 box, +16 DrawText alignment (1 centre, 2 right, 4 vcentre, 8 bottom). */
static void text_render(Engine *e, int ri)
{
    const u8 *r = resource_bytes(&e->c, &e->c.dir[ri]);
    if (e->c.dir[ri].size < 18) return;
    TextObj *t = &g_txt[ri];
    if (!t->done) {                                  /* first use: the record's own text */
        char *s0 = str_at(e, rd16(r + 4));
        snprintf(t->text, sizeof t->text, "%s", s0 ? s0 : "");
        t->done = 1;
    }
    free(t->px); t->px = NULL; t->w = t->h = 0; t->version++;
    const char *txt = t->text[0] ? t->text : " ";
    int size = vdec(e, rd16(r + 6)); if (size <= 0) size = 12;
    u8 colour = r[0];                                /* copied raw into the object (+4), not decoded */
    if (!g_font_ok) return;
    float sc = stbtt_ScaleForPixelHeight(&g_fi, (float)size);
    int gl[128], n = 0;                              /* symbol-encoded fonts sit at U+F000 */
    for (const char *c = txt; *c && n < 127; c++) {
        int g = stbtt_FindGlyphIndex(&g_fi, (u8)*c);
        if (!g) g = stbtt_FindGlyphIndex(&g_fi, 0xF000 + (u8)*c);
        gl[n++] = g;
    }
    int asc, desc, gap; stbtt_GetFontVMetrics(&g_fi, &asc, &desc, &gap);
    int base = (int)(asc * sc + 0.5f), h = (int)((asc - desc) * sc + 0.5f) + 1, w = 0;
    for (int i = 0; i < n; i++) {
        int adv, lsb; stbtt_GetGlyphHMetrics(&g_fi, gl[i], &adv, &lsb);
        w += (int)(adv * sc + 0.5f);
        if (i + 1 < n) w += (int)(stbtt_GetGlyphKernAdvance(&g_fi, gl[i], gl[i + 1]) * sc);
    }
    w += 2; if (w < 1) w = 1;
    u8 *px = calloc((size_t)w * h, 1);
    if (!px) return;
    int x = 0;
    for (int i = 0; i < n; i++) {
        int gw = 0, gh = 0, xo = 0, yo = 0;
        unsigned char *g = stbtt_GetGlyphBitmap(&g_fi, sc, sc, gl[i], &gw, &gh, &xo, &yo);
        if (g) for (int yy = 0; yy < gh; yy++) for (int xx = 0; xx < gw; xx++) {
            int dx = x + xo + xx, dy = base + yo + yy;
            if (dx >= 0 && dx < w && dy >= 0 && dy < h && g[yy * gw + xx] > 127) px[(size_t)dy * w + dx] = colour;
        }
        stbtt_FreeBitmap(g, NULL);
        int adv, lsb; stbtt_GetGlyphHMetrics(&g_fi, gl[i], &adv, &lsb);
        x += (int)(adv * sc + 0.5f);
        if (i + 1 < n) x += (int)(stbtt_GetGlyphKernAdvance(&g_fi, gl[i], gl[i + 1]) * sc);
    }
    int flags = r[16];                               /* FUN_1008_d25e: anchor by alignment */
    int ox = (s16)rd16(r + 8), oy = (s16)rd16(r + 10);
    if (flags & 2) ox -= w / 2; else if (!(flags & 1)) ox += (w - 1) / 2;
    if (flags & 8) oy -= h / 2; else if (!(flags & 4)) oy += (h - 1) / 2;
    t->px = px; t->w = w; t->h = h;
    t->ox = ox - (w - 1) / 2; t->oy = oy - (h - 1) / 2;   /* top-left, as frame_resolve reports */
}

static const u8 *text_cel(Engine *e, int ri, int *w, int *h, int *ox, int *oy)
{
    if (ri < 0 || ri >= g_nbm || res_type(e, ri) != 16 || !g_txt) return NULL;
    if (!g_txt[ri].done) text_render(e, ri);
    if (!g_txt[ri].px) return NULL;
    *w = g_txt[ri].w; *h = g_txt[ri].h; *ox = g_txt[ri].ox; *oy = g_txt[ri].oy;
    return g_txt[ri].px;
}

/* ---- sprites (JUNGS01) --------------------------------------------------- */

static Sprite *sprite_find(Engine *e, int res)
{
    for (int i = 0; i < ENG_MAX_SPRITES; i++)
        if (e->spr[i].used && e->spr[i].res == res) return &e->spr[i];
    return NULL;
}

/* FUN_1008_5e9a: fault in a type 15 and create its sprite. */
static Sprite *sprite_load(Engine *e, int res)
{
    Sprite *s = sprite_find(e, res);
    if (s) return s;
    if (res_type(e, res) != 15) return NULL;
    const u8 *r = resource_bytes(&e->c, &e->c.dir[res]);
    size_t sz = e->c.dir[res].size;
    if (sz < 0x14) return NULL;
    for (int i = 0; i < ENG_MAX_SPRITES && !s; i++) if (!e->spr[i].used) s = &e->spr[i];
    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    s->used = 1; s->res = res;
    s->x = 0; s->y = 0;                              /* logical origin: the canvas centre */
    s->z = vdec(e, rd16(r + 0x0C));
    int count = rd16(r + 2);
    for (int i = 0; i < count && 0x14 + (size_t)i * 2 + 2 <= sz && s->ncel < ENG_MAX_CELS; i++) {
        u16 raw = rd16(r + 0x14 + i * 2);
        int t = res_type(e, as_index(vdec(e, raw)));
        if (t == 1 || t == 10 || t == 16) s->cel[s->ncel++] = as_index(vdec(e, raw));
    }
    s->hit_hidden = r[0x13]; s->hit_rect = r[0x0F];  /* S_058 at creation */
    s->running = 1;                              /* S_011(1) after creation */
    return s;
}

/* FUN_1000_6364: set the cel range, return its length, 0 if out of range. */
static int set_range(Sprite *s, int first, int last, int start)
{
    if (first < 0 || last < 0 || first >= s->ncel || last >= s->ncel) return 0;
    s->first = first; s->last = last; s->next = start;
    s->fwd = first <= last;
    s->autocyc = first != last;
    return (s->fwd ? last - first : first - last) + 1;
}

/* FUN_1000_63ee: cur <- next, next steps one with wrap to first. */
static void step_cel(Sprite *s)
{
    s->cur = s->next;
    s->ncomp = 0;
    if (s->fwd) { if (++s->next > s->last) s->next = s->first; }
    else        { if (--s->next < s->last) s->next = s->first; }
}

static int due(Engine *e, u32 d) { return (int)(e->now - d) >= 0; }
static void rearm(Engine *e, Sprite *s, u32 *d, u32 per) { *d = (s->catchup ? *d : e->now) + per; }

/* Auto-cycle during moves: cels advance on the frame timer. */
static void autocycle(Engine *e, Sprite *s)
{
    if (s->autocyc && due(e, s->fdue)) { step_cel(s); rearm(e, s, &s->fdue, s->fper); }
}

static int t13_len(const u8 *p, size_t left)
{
    static const int L[23] = { 0, 10, 14, 2, 2, 30, 8, 10, 2, 22, 8, 6, 8, 4, 4, 8, 6, 4, 4, 2, 0, 14, 12 };
    if (left < 2) return 0;
    int op = rd16(p);
    if (op == 20) return left >= 4 ? rd16(p + 2) : 0;
    return (op > 0 && op < 23) ? L[op] : 0;
}

/* One type 13 record: init once when the PC arrives, then exec until done. */
static int t13_exec(Engine *e, Sprite *s, u8 *r, int len)
{
    int op = rd16(r), init = !s->inited;
    s->inited = 1;
#define V(o) vdec(e, rd16(r + (o)))
    switch (op) {
    case 1: {                                        /* PLAY_RANGE */
        if (init) { int n = set_range(s, V(2), V(4), V(2)); r[6] = (u8)n; r[7] = (u8)(n >> 8); }
        int count = rd16(r + 6);
        if (!due(e, s->fdue)) return 0;
        if (!count) return 1;
        if (rd16(r + 8) != 0xFFFF) s->fper = (u16)V(8);
        rearm(e, s, &s->fdue, s->fper);
        s->visible = 1;
        step_cel(s);
        count--; r[6] = (u8)count; r[7] = (u8)(count >> 8);
        return s->fper == 0 && count == 0;
    }
    case 2: {                                        /* SHOW_CELS */
        if (init) r[0x0C] = 0;
        if (!due(e, s->fdue)) return 0;
        if (r[0x0C]) return 1;
        if (rd16(r + 0x0A) != 0xFFFF) s->fper = (u16)V(0x0A);
        rearm(e, s, &s->fdue, s->fper);
        int nc = r[0x0D] > 4 ? 4 : r[0x0D], k = 0;
        for (int i = 0; i < nc; i++) {
            u16 cv = (u16)V(2 + i * 2);
            if (cv == 0xFFFF) continue;
            if (cv >= s->ncel) return 1;
            s->comp[k++] = cv;
        }
        s->ncomp = k;
        s->visible = 1;
        r[0x0C] = 1;
        return s->fper == 0;
    }
    case 5: case 9: {                                /* ARC, MOVE_TO */
        s16 *rt = (s16 *)(void *)(r + (op == 5 ? 0x0E : 0x0A));
        if (init) {
            int rel = op == 5 ? r[4] : r[5];
            rt[0] = (s16)s->x; rt[1] = (s16)s->y;
            if (op == 5) {
                rt[2] = V(6); rt[3] = V(8); rt[4] = V(0x0A); rt[5] = V(0x0C);
                if (rel) { rt[2] += s->x; rt[3] += s->y; rt[4] += s->x; rt[5] += s->y; }
                rt[6] = 0; rt[7] = V(2);
            } else {
                rt[2] = V(6); rt[3] = V(8);
                if (rel) { rt[2] += s->x; rt[3] += s->y; }
                int n = V(2);
                if (r[4] && n > 0) {
                    int dx = abs(rt[2] - rt[0]), dy = abs(rt[3] - rt[1]), m = dx > dy ? dx : dy;
                    n = (m + n - 1) / n;
                }
                rt[4] = 0; rt[5] = (s16)n;
            }
            s->mdue = e->now;
        }
        autocycle(e, s);
        s16 *ip = op == 5 ? &rt[6] : &rt[4], *np = op == 5 ? &rt[7] : &rt[5];
        if (*ip >= *np) return 1;
        if (!due(e, s->mdue)) return 0;
        rearm(e, s, &s->mdue, s->mper);
        int i = ++*ip, n = *np;
        if (op == 5) {
            long t = ((long)i * 1024 + n / 2) / n;
            for (int a = 0; a < 2; a++) {
                long d0 = rt[a], dm = rt[2 + a] - d0, de = rt[4 + a] - d0;
                long v = d0 + ((t * (1024 - t) * (4 * dm - de) + t * t * de) >> 20);
                if (a == 0) s->x = (int)v; else s->y = (int)v;
            }
        } else {
            for (int a = 0; a < 2; a++) {
                long d = (long)(rt[2 + a] - rt[a]) * i, q = d >= 0 ? (d + n / 2) / n : -((-d + n / 2) / n);
                if (a == 0) s->x = rt[0] + (int)q; else s->y = rt[1] + (int)q;
            }
        }
        return *ip >= *np;
    }
    case 7:                                          /* FLIP_OFFSET */
        s->flipx = V(2) != 0; s->flipy = V(4) != 0;
        s->x += V(6); s->y += V(8);
        return 1;
    case 8: s->visible = 0; return 1;                /* HIDE */
    case 10:                                         /* JUMP_POS */
        if (init) s->mdue = e->now + s->mper;
        autocycle(e, s);
        if (!due(e, s->mdue)) return 0;
        if (rd16(r + 2)) { s->x += V(4); s->y += V(6); } else { s->x = V(4); s->y = V(6); }
        return 1;
    case 11:                                         /* WAIT */
        if (init) { s->fdue = e->now + (u16)V(2); }
        return due(e, s->fdue);
    case 12:                                         /* SET_POS */
        if (r[6]) { s->x += V(2); s->y += V(4); } else { s->x = V(2); s->y = V(4); }
        return 1;
    case 13: s->catchup = r[2]; s->fdue = s->mdue = e->now; return 1;
    case 14: s->fper = (u16)V(2); s->fdue = s->mdue = e->now; return 1;
    case 15: set_range(s, V(4), V(6), V(2)); return 1;
    case 16:
        s->mper = (u16)V(2);
        if (rd16(r + 4)) s->fper = (u16)V(4);
        return 1;
    case 17: {
        u16 c = rd16(r + 2);
        if (c != 0xFFFF && c < s->ncel) { s->next = c; step_cel(s); }
        s->visible = 1;
        return 1;
    }
    case 20: {                                       /* SCRIPT: one type 14 statement */
        wrw(e, addr_of(e, G_OWNER), (s16)s->res);   /* FUN_1008_c59e stores the decoded tag: the index */
        int adv;
        if (len > 4) exec_record(e, r + 4, 0, (size_t)len - 4, &adv);
        return 1;
    }
    case 21: {                                       /* event 0x15: a call, FUN_1008_c4a6 */
        u16 args[4]; int n = rd16(r + 4) > 4 ? 4 : rd16(r + 4);
        for (int k = 0; k < n; k++) args[k] = rd16(r + 6 + k * 2);
        if (e->trace) printf("sprite %d event call script %d\n", s->res, (u16)(rd16(r + 2) + IMM_BIAS));
        call_with(e, rd16(r + 2), n, args);
        return 1;
    }
    default: return 1;                               /* 3, 4, 18, 19, 22: not needed yet */
    }
#undef V
}

/* FUN_1000_4164: move the PC on, handling LOOP records inline. */
static void t13_advance(Sprite *s, int len)
{
    s->pc += len; s->inited = 0;
    for (int guard = 0; guard < 64 && s->pc + 8 <= s->plen && rd16(s->prog + s->pc) == 6; guard++) {
        u8 *r = s->prog + s->pc;
        int counter = rd16(r + 2), count = rd16(r + 4), target = rd16(r + 6);
        if (s->brk || counter == 1) {
            r[2] = (u8)count; r[3] = (u8)(count >> 8);
            s->pc += 8;
        } else {
            if (counter) { counter--; r[2] = (u8)counter; r[3] = (u8)(counter >> 8); }
            s->pc = target;
        }
    }
}


static void program_end(Engine *e, Sprite *s)
{
    s->running = 0;                                  /* the PC stays at the end: an appended record resumes it */
}

static void sprite_tick(Engine *e, Sprite *s)
{
    if (!s->running || !s->prog || s->frozen) return;
    for (int guard = 0; guard < 64; guard++) {
        if (s->pc >= s->plen) { program_end(e, s); return; }   /* S_010 on run-off */
        u8 *r = s->prog + s->pc;
        int len = t13_len(r, (size_t)(s->plen - s->pc));
        if (len <= 0 || s->pc + len > s->plen) { s->running = 0; return; }
        int gen = s->gen;
        if (!t13_exec(e, s, r, len)) return;
        if (s->gen != gen) continue;           /* an op 20 statement loaded a new program */
        t13_advance(s, len);
    }
}

static void sprite_program_bytes(Engine *e, Sprite *s, const u8 *rec, int len, int replace)
{
    if (replace || !s->prog) {
        free(s->prog); s->prog = malloc((size_t)len); s->plen = 0; s->pc = 0; s->inited = 0; s->gen++;
        s->brk = 0; s->fdue = s->mdue = e->now;
    } else {
        u8 *np = realloc(s->prog, (size_t)(s->plen + len));
        if (!np) return;
        s->prog = np;
    }
    memcpy(s->prog + s->plen, rec, (size_t)len);
    s->plen += len;
    if (!s->running) { s->running = 1; s->inited = 0; }   /* FUN_1000_45b2 if idle */
}

static void sprite_program(Engine *e, Sprite *s, int res13)
{
    free(s->prog); s->prog = NULL; s->plen = s->pc = s->inited = 0;
    s->brk = 0; s->gen++;
    if (res_type(e, res13) != 13) return;
    s->plen = (int)e->c.dir[res13].size;
    s->prog = malloc((size_t)s->plen);
    memcpy(s->prog, resource_bytes(&e->c, &e->c.dir[res13]), (size_t)s->plen);
    s->fdue = s->mdue = e->now;
    s->running = 1;
    t13_advance(s, 0);                               /* a program may open with a LOOP */
}

/* ---- script records (FUN_1008_c724) -------------------------------------- */

/* A type 7 clip, decoded once: 28-byte header (u32 encoded length, then a
 * WAVEFORMATEX for the decoded output, 22050 Hz mono 16-bit on this disc),
 * then 4-bit ADPCM decoded with JUNGA01's tables. */
static int clip(Engine *e, int ri, const s16 **pcm, u32 *n)
{
    if (res_type(e, ri) != 7 || ri >= g_nbm || e->c.dir[ri].size < 28) return 0;
    if (!g_snd[ri].pcm) {
        const u8 *r = resource_bytes(&e->c, &e->c.dir[ri]);
        u32 enc = rd32(r);
        if (enc + 28 > e->c.dir[ri].size) enc = e->c.dir[ri].size - 28;
        g_snd[ri].pcm = malloc((size_t)enc * 2 * sizeof(s16) + 2);
        if (!g_snd[ri].pcm) return 0;
        g_snd[ri].n = (u32)adpcm_decode(r + 28, enc, g_snd[ri].pcm);
    }
    *pcm = g_snd[ri].pcm; *n = g_snd[ri].n;
    return *n > 0;
}

/* The type 4 sequencer, FUN_1000_1b20: each dword is a MIDI short message,
 * or, with any of the top four bits set, a delay of (w & 0x7FFFFFFF) ms that
 * JUNGA01 arms as a one-shot timer (1-2 ms are raised to 3). At the end of the
 * events a loop count of 0 stops and posts the notify, above 0 counts down and
 * rewinds, below 0 repeats for ever. `wait` counts thousandths of a sample. */
static void music_stop(Engine *e)
{
    e->mus.on = 0;
    synth_reset(&e->synth);                          /* FUN_1000_1796: notes off, midiOutReset */
}

static void music_render(Engine *e, s16 *out, u32 ns)
{
    u32 pos = 0;
    while (pos < ns) {
        while (e->mus.on && e->mus.wait < 1000) {
            if (e->mus.idx >= e->mus.n) {
                if (e->mus.loops == 0) {
                    e->mus.on = 0;
                    if (e->mus.notify) enqueue(e, e->mus.notify, e->mus.tag, 0);   /* 0x52C at a natural end */
                    break;
                }
                if (e->mus.loops > 0) e->mus.loops--;
                e->mus.idx = 0;
            }
            u32 w = rd32(e->mus.ev + e->mus.idx++ * 4);
            if ((w >> 16) & 0xF000) {
                u32 ms = w & 0x7FFFFFFF;
                if (ms && ms < 3) ms = 3;
                e->mus.wait += ms * 22050;
            } else synth_msg(&e->synth, w);
        }
        u32 chunk = ns - pos;
        if (e->mus.on && e->mus.wait / 1000 < chunk) chunk = e->mus.wait / 1000;
        if (chunk == 0 && e->mus.on) chunk = 1;
        synth_render(&e->synth, out + pos, chunk);
        if (e->mus.on) e->mus.wait -= e->mus.wait >= chunk * 1000 ? chunk * 1000 : e->mus.wait;
        pos += chunk;
    }
}

/* Mix voices up to engine time t, 22050 samples a second. A voice that runs
 * out fires its completion script, the JUNGA01 notify that op 8 asked for. */
static void mix_to(Engine *e, u32 t)
{
    /* The target is a sample count derived from engine time, so rounding never
     * accumulates: stepping a millisecond clock by whole samples had run the
     * mixer 6% fast, putting sound ever further behind the picture. */
    unsigned long long target = (unsigned long long)t * 22050 / 1000;
    if (target <= e->audio_samples) return;
    u32 ns = (u32)(target - e->audio_samples);
    e->audio_samples += ns;
    e->audio_clock = t;
    if (e->naudio + ns > e->cap_audio) {
        u32 cap = e->naudio + ns + 22050;
        s16 *na = realloc(e->audio, cap * sizeof(s16));
        if (!na) return;
        e->audio = na; e->cap_audio = cap;
    }
    s16 *out = e->audio + e->naudio;
    memset(out, 0, ns * sizeof(s16));
    for (int k = 0; k < 16; k++) {
        EngVoice *v = &e->voice[k];
        if (!v->pcm) continue;
        for (u32 i = 0; i < ns; i++) {
            if (v->pos >= v->n) {
                if (v->loop) v->pos = 0;
                else {
                    if (v->done) enqueue(e, v->done, v->tag, 0);
                    v->pcm = NULL; break;
                }
            }
            int m = out[i] + v->pcm[v->pos++];
            out[i] = (s16)(m > 32767 ? 32767 : m < -32768 ? -32768 : m);
        }
    }
    music_render(e, out, ns);
    e->naudio += ns;
    if (e->naudio > 22050 * 4) {                     /* nobody draining: keep the newest 4 s */
        memmove(e->audio, e->audio + e->naudio - 22050 * 2, 22050 * 2 * sizeof(s16));
        e->naudio = 22050 * 2;
    }
}

u32 engine_audio(Engine *e, s16 *out, u32 max)
{
    u32 n = e->naudio < max ? e->naudio : max;
    memcpy(out, e->audio, n * sizeof(s16));
    memmove(e->audio, e->audio + n, (e->naudio - n) * sizeof(s16));
    e->naudio -= n;
    return n;
}

/* FUN_1008_8646: the comparison shared by ops 17 and 38. */
static int compare(int A, int B, int f)
{
    int c;
    if (f & 1) c = A == B; else if (f & 2) c = A > B; else if (f & 4) c = A < B;
    else if (f & 8) c = (A | B) != 0; else if (f & 0x10) c = (A & B) != 0;
    else if (f & 0x20) c = B ? (A % B) != 0 : 0; else c = A != 0;
    if (f & 0x80) c = !c;
    return c;
}

/* Executes the record at b+pc. Returns 0 to stop the script. *adv is the
 * advance, relative to this record, as the original's local_8. */
static int exec_record(Engine *e, const u8 *b, size_t pc, size_t n, int *adv)
{
    int targets[8], nt = 0;
    int len = record_len(b, pc, n, targets, &nt);
    const u8 *r = b + pc;
    int op = (pc + 2 <= n) ? rd16(r) : 0;
    *adv = len;
    if (op == 37 && len == -2) {                     /* jump: record_len reports it specially */
        if (e->trace) printf("%*s%04zx op37 jump %d\n", e->depth * 2, "", pc, (s16)rd16(r + 2));
        *adv = (s16)rd16(r + 2);
        return *adv != 0;
    }
    if (len <= 0) { *adv = 0; return 0; }
#define W(o) rd16(r + (o))
#define V(o) vdec(e, W(o))
    if (e->trace) printf("%*s%04zx op%d len %d\n", e->depth * 2, "", pc, op, len);
    switch (op) {
    case 1: {                                        /* call with arguments, FUN_1008_c63e */
        int frame = r[4], argc = r[5];
        u16 a = (u16)(e->fp - frame * 2);
        for (int k = 0; k < argc && 6 + (size_t)k * 2 + 2 <= (size_t)len; k++) { wrw(e, a, V(6 + k * 2)); a -= 2; }
        e->fp = (u16)(e->fp - frame * 2);
        run_script(e, W(2));
        e->fp = (u16)(e->fp + frame * 2);
        return 1;
    }
    case 5: {                                        /* place sprite, FUN_1008_a312 */
        int flag = r[6];
        int s15 = as_index(V(2)), s13 = W(4) ? as_index(V(4)) : -1;
        Sprite *s = sprite_load(e, s15);
        if (e->trace) printf("%*s     sprite %d program %d flag %d\n", e->depth * 2, "", s15, s13, flag);
        if (!s) return 1;
        if (flag) s->visible = 0;
        if (s13 < 0) { s->running = 0; } else sprite_program(e, s, s13);
        return 1;
    }
    case 6: {                                        /* background, FUN_1008_032c */
        int mode = W(2);
        if (mode == 0) { e->bg = as_index(V(4)); e->bgfill = -1; }
        else if (mode == 1) { e->bg = -1; e->bgfill = W(8) & 0xFF; }
        else { e->bg = -1; e->bgfill = 0; }
        return 1;
    }
    case 4: {                                        /* hotspot setup, FUN_1008_2dc0 / 27e0 */
        if (r[0x0E]) return 1;                       /* global enable toggles: not modelled */
        if (W(2)) {
            int o = as_index(V(2));
            if (o < 0 || o >= e->c.ndir) return 1;
            e->click_off[o] = 0;
            if (r[0x10]) e->click_off[o] = 1;
            else if (!r[0x0F]) e->click[o] = W(4);
            return 1;
        }
        s16 l = V(6), t = V(8), rr = V(10), bb = V(12);
        int k;
        for (k = 0; k < e->nhot; k++)
            if (e->hot[k].l == l && e->hot[k].t == t && e->hot[k].r == rr && e->hot[k].b == bb) break;
        if (k == e->nhot) { if (e->nhot >= 64) return 1; e->nhot++; }
        if (r[0x10]) { e->hot[k].off = 1; return 1; }
        if (!r[0x0F]) {
            e->hot[k].l = l; e->hot[k].t = t; e->hot[k].r = rr; e->hot[k].b = bb;
            e->hot[k].script = W(4) ? (u16)(V(4) - IMM_BIAS) : 0;
        }
        e->hot[k].off = 0;
        return 1;
    }
    case 64: e->mouse_script = W(2); return 1;       /* DAT_1020_14e2 */
    case 61: {                                       /* hover region, FUN_1008_35a6 / 2020 / 2426 */
        if (r[14]) { e->nreg = 0; e->inreg = 0; return 1; }
        s16 l = V(2), t = V(4), rr = V(6), bb = V(8);
        int k;
        for (k = 0; k < e->nreg; k++) if (e->reg[k].l == l && e->reg[k].t == t && e->reg[k].r == rr && e->reg[k].b == bb) break;
        if (!W(10)) {                                /* no enter script: remove it */
            if (k < e->nreg) { memmove(&e->reg[k], &e->reg[k + 1], sizeof e->reg[0] * (size_t)(e->nreg - k - 1)); e->nreg--; e->inreg = 0; }
            return 1;
        }
        if (k == e->nreg) { if (e->nreg >= 40) return 1; e->nreg++; }
        e->reg[k].l = l; e->reg[k].t = t; e->reg[k].r = rr; e->reg[k].b = bb;
        e->reg[k].enter = (u16)(V(10) - IMM_BIAS);
        e->reg[k].leave = W(12) ? (u16)(V(12) - IMM_BIAS) : 0;
        return 1;
    }
    case 2: {                                        /* load a resource, FUN_1008_669c */
        int ri = as_index(V(2));
        if (res_type(e, ri) == 15) sprite_load(e, ri);     /* creates the sprite, hidden */
        return 1;                                    /* everything else is resident already */
    }
    case 3: {                                        /* unload, FUN_1008_64b8: a type 15 loses its sprite */
        int ri = as_index(V(2));
        Sprite *sp = res_type(e, ri) == 15 ? sprite_find(e, ri) : NULL;
        if (sp) { free(sp->prog); memset(sp, 0, sizeof *sp); if (e->pressed == ri) e->pressed = 0; }
        return 1;
    }
    case 8: {                                        /* play a clip, FUN_1008_88b2 -> JUNGA01 A_029 */
        int si = as_index(V(2));
        u16 done = r[7] ? W(0x1A) : 0;               /* the record from +4 goes to JUNGA01; +3 there is */
        const s16 *pcm = NULL; u32 n = 0;            /* "notify", +0x16 there the completion script */
        if (!clip(e, si, &pcm, &n)) {                /* FUN_1008_88b2: cannot play, complete now */
            if (done && e->nsnd < 16) { e->snd[e->nsnd].script = done; e->snd[e->nsnd].tag = W(2); e->snd[e->nsnd].due = e->now; e->nsnd++; }
            return 1;
        }
        int k, free_k = -1;
        for (k = 0; k < 16; k++) { if (e->voice[k].res == si && e->voice[k].pcm) break; if (!e->voice[k].pcm && free_k < 0) free_k = k; }
        if (k == 16) k = free_k;
        if (k < 0) return 1;
        e->voice[k].pcm = pcm; e->voice[k].n = n; e->voice[k].pos = 0; e->voice[k].res = si;
        e->voice[k].loop = (s16)W(4) == -1; e->voice[k].done = done; e->voice[k].tag = W(2);
        {   /* ENGINE_AVLOG: where each clip starts in the mix, for sync checks */
            static int avlog = -1; if (avlog < 0) avlog = getenv("ENGINE_AVLOG") != NULL;
            if (avlog) printf("av: clip %d at %u ms sample %llu\n", si, e->now, e->audio_samples);
        }
        return 1;
    }
    case 38:                                         /* conditional jump, FUN_1008_927c */
        *adv = compare(V(4), V(6), r[8]) ? 10 : (s16)W(2);
        return 1;
    case 80: {                                       /* play a music track, FUN_1008_952a -> JUNGA01 A_028 */
        int ri = as_index(V(2));
        music_stop(e);                               /* one track at a time, always from event 0 */
        if (e->trace) fprintf(stderr, "music: op 80 res %d type %d\n", ri, res_type(e, ri));
        if (res_type(e, ri) != 4 || e->c.dir[ri].size < 4) return 1;
        const u8 *b = resource_bytes(&e->c, &e->c.dir[ri]);
        u32 len = rd16(b);
        if (len + 4 > e->c.dir[ri].size) len = e->c.dir[ri].size - 4;
        e->mus.ev = b + 4; e->mus.n = len / 4; e->mus.idx = 0; e->mus.wait = 0;
        e->mus.loops = (s16)W(4);                    /* 0 once, N extra passes, 0xFFFF for ever */
        e->mus.notify = r[6] ? (u16)V(8) : 0;        /* nonzero: a script, queued like op 8 completions */
        e->mus.tag = rd16(b + 2);                    /* header id, the message's wParam */
        e->mus.on = 1;
        if (e->trace) fprintf(stderr, "music: track %d, %u events, loops %d, notify %u\n", ri, e->mus.n, e->mus.loops, e->mus.notify);
        return 1;
    }
    case 81: music_stop(e); return 1;
    case 10: case 11: {                              /* fades, FUN_1008_3bb2 */
        /* On a palette display (RC_PALETTE, DAT_1020_5a5c) both run FUN_1008_397a:
         * S_021 scales the palette from index 10 by step/steps, one step a frame,
         * and the engine does nothing else until it finishes. Steps = speed, 1-100. */
        int n = W(2); if (n == 0 || n > 100) n = 100;
        fade_update(e);
        if (e->trace) fprintf(stderr, "fade: op %d steps %d at %u in %s, faded %d\n", op, n, e->now, e->name, e->fade.faded);
        if (op == 11) {                              /* FUN_1008_3abe: out, unless already faded */
            if (e->fade.faded) return 1;
            if (!e->fade.snap) e->fade.snap = malloc((size_t)ENG_W * ENG_H);
            if (e->fade.snap) { e->fade.dir = 0; engine_render(e, e->fade.snap); }   /* the screen as the fade began */
            e->fade.dir = -1;
        } else {                                     /* FUN_1008_39c4: in, only after an out */
            if (!e->fade.faded) return 1;
            e->fade.dir = 1;
        }
        e->fade.faded = op == 11; e->fade.n = n; e->fade.t0 = e->now;
        fade_update(e);
        return 1;
    }
    case 68: {                                       /* FUN_1008_a12e: var = sprite loaded and visible (S_063, +0x4E) */
        Sprite *sp = sprite_find(e, as_index(V(4)));
        wrw(e, addr_of(e, W(2)), (s16)(sp && sp->visible));
        return 1;
    }
    case 54: {                                       /* start or end text entry, FUN_1008_dbdc */
        int ri = W(2) ? as_index(V(2)) : -1;
        if (ri >= 0 && (res_type(e, ri) != 16 || !g_txt)) return 0;
        if (e->edit.on) edit_finish(e, 0);           /* FUN_1008_d98a: the open field keeps its text */
        if (ri < 0) return 1;
        e->edit.ri = ri; e->edit.strvar = W(4); e->edit.done = W(6);
        e->edit.maxlen = r[10] ? r[10] : 0x7F; e->edit.digits = r[11];
        if (!g_txt[ri].done) text_render(e, ri);     /* the record's own text, if never set */
        char *t = g_txt[ri].text;
        snprintf(e->edit.saved, sizeof e->edit.saved, "%s", t);
        size_t n = strlen(t); if (n + 2 <= sizeof g_txt[0].text) { t[n] = '_'; t[n + 1] = 0; }   /* FUN_1008_d9aa */
        g_txt[ri].done = 1; e->edit.on = 1;
        text_render(e, ri);
        return 1;
    }
    case 73:                                         /* move the pointer (SetCursorPos), centre-relative */
        e->warp.x = ORG_X + V(2); e->warp.y = ORG_Y + V(4);   /* viewport origin (DAT_10b6 x, 10b8 y) plus +2, +4 */
        e->warp.pending = e->warp.host = 1;
        return 1;
    case 71: return 1;                               /* FUN_1008_8b00 -> JUNGS01 S_065: repaint now; every frame is repainted here */                /* A_038: every script stops the current track */
    case 14: memset(e->voice, 0, sizeof e->voice); return 1;   /* FUN_1008_a40e: taken as stop all sounds */
    case 15: {                                       /* FUN_1008_a422 -> A_039: taken as stop a sound (0 = all) */
        int si = W(2) ? as_index(V(2)) : -1;
        for (int k = 0; k < 16; k++) if (si < 0 || e->voice[k].res == si) e->voice[k].pcm = NULL;
        return 1;
    }
    case 41: {                                       /* set a text object's text, FUN_1008_d846 */
        int ri = as_index(V(2));
        if (res_type(e, ri) != 16 || !g_txt) return 0;
        char *fmt = str_at(e, W(4));
        s16 num[1] = { V(6) }; char *strs[1] = { NULL }; u8 kinds[1] = { 1 };
        wsprintf16(g_txt[ri].text, sizeof g_txt[ri].text, fmt ? fmt : "", num, strs, 1, kinds);
        g_txt[ri].done = 1;
        text_render(e, ri);
        if (e->trace) printf("%*s     text %d = \"%s\" (%dx%d)\n", e->depth * 2, "", ri, g_txt[ri].text, g_txt[ri].w, g_txt[ri].h);
        return 1;
    }
    case 50: {                                       /* step along an angle to a box edge, FUN_1008_7044 */
        int xa = V(0x0C), xb = V(0x10), ya = V(0x0E), yb = V(0x12);
        int xmin = xa < xb ? xa : xb, xmax = xa < xb ? xb : xa, ymin = ya < yb ? ya : yb, ymax = ya < yb ? yb : ya;
        int x = V(6), y = V(8);
        if (x < xmin || x > xmax) x = x < xmin ? xmin : xmax;
        if (y < ymin) y = ymin; else if (y > ymax) y = ymax;
        int rx, ry;
        ray_to_box(ymax, xmax, ymin, xmin, y, x, norm_angle(V(0x0A)), &rx, &ry);
        if (rx < xmin) rx = xmin; else if (rx > xmax) rx = xmax;
        if (ry < ymin) ry = ymin; else if (ry > ymax) ry = ymax;
        wrw(e, addr_of(e, W(2)), (s16)rx); wrw(e, addr_of(e, W(4)), (s16)ry);
        return 1;
    }
    case 75: {                                       /* the bitmaps a sprite shows, FUN_1008_8dde */
        wrw(e, addr_of(e, W(4)), 0);
        Sprite *sp = sprite_find(e, as_index(V(2)));
        if (!sp) return 1;
        int cels[4], nc = 0;
        if (sp->ncomp) for (int k = 0; k < sp->ncomp; k++) cels[nc++] = sp->comp[k]; else cels[nc++] = sp->cur;
        for (int k = 0; k < nc && k < 4; k++) {
            u16 var = W(4 + k * 2);
            if (!var || cels[k] < 0 || cels[k] >= sp->ncel) continue;
            int ox, oy, bi = frame_resolve(&e->c, sp->cel[cels[k]] + HANDLE_BASE, &ox, &oy);
            if (bi >= 0) wrw(e, addr_of(e, var), (s16)bi);  /* header +0x0E + 0x7531 = the bitmap's index */
        }
        return 1;
    }
    case 16: {                                       /* arithmetic fold into a variable, FUN_1008_977c / 8732 */
        long acc = V(2);
        int steps = W(4);
        for (int k = 0; k < steps && 6 + (k + 1) * 6 <= len; k++) {
            const u8 *st = r + 6 + k * 6;
            long v = vdec(e, rd16(st));
            if (st[4]) v = -v;
            switch (st[2]) {
            case 0: acc = v; break;
            case 1: acc = v + acc; break;
            case 2: acc = acc - v; break;
            case 3: acc = v * acc; break;
            case 4: if (v) { long h = v < 0 ? -((-v) / 2) : v / 2; acc = (acc + h) / v; } else acc = 0; break;
            case 5: acc = v | acc; break;
            case 6: acc = v & acc; break;
            case 7: acc = v ? acc % v : 0; break;
            default: acc = 0;
            }
        }
        wrw(e, addr_of(e, W(2)), (s16)acc);
        return 1;
    }
    case 28: {                                       /* S_066: re-sort a sprite to a new z, FUN_1008_9394 */
        Sprite *sp = sprite_find(e, as_index(V(2)));
        if (sp) sp->z = V(4);
        return 1;
    }
    case 36: {                                       /* S_051: move a sprite by dx, dy, FUN_1008_9444 */
        Sprite *sp = sprite_find(e, as_index(V(2)));
        if (!sp) return 1;
        int dx = V(4), dy = V(6);
        sp->x += dx; sp->y += dy;
        if (sp->prog && sp->inited && sp->pc + 2 <= sp->plen) {   /* shift an op 5 / op 9 in flight */
            u8 *pr = sp->prog + sp->pc; int pop = rd16(pr);
            if (pop == 5 || pop == 9) {
                s16 *rt = (s16 *)(void *)(pr + (pop == 5 ? 0x0E : 0x0A));
                int pts = pop == 5 ? 3 : 2;
                for (int k = 0; k < pts; k++) { rt[k * 2] += (s16)dx; rt[k * 2 + 1] += (s16)dy; }
            }
        }
        return 1;
    }
    case 63: {                                       /* S_038: set a sprite's position */
        Sprite *sp = sprite_find(e, as_index(V(2)));
        if (sp) { sp->x = V(4); sp->y = V(6); }
        return 1;
    }
    case 17: {                                       /* conditional call, FUN_1008_919c / 8646 */
        int c = compare(V(8), V(10), r[12]);
        u16 target;
        if (c) target = W(2); else if (r[6]) target = W(4); else return 1;
        if (!target) { *adv = 0; return 0; }
        return run_script(e, target);
    }
    case 19: {                                       /* set timer, FUN_1008_ddd6 */
        u32 ms = r[10] ? (u16)V(6) : ((u32)W(6) | ((u32)W(8) << 16));
        int k;
        for (k = 0; k < e->ntm; k++) if (e->tm[k].id == W(2)) break;
        if (k >= 20) return 1;
        if (k == e->ntm) e->ntm++;
        e->tm[k].id = W(2); e->tm[k].script = W(4); e->tm[k].every = ms;
        e->tm[k].due = e->now + ms; e->tm[k].repeat = r[11];
        return 1;
    }
    case 20: {                                       /* kill timer, FUN_1008_dd62 */
        if (r[4]) { e->ntm = 0; return 1; }
        for (int k = 0; k < e->ntm; k++)
            if (e->tm[k].id == W(2)) { memmove(&e->tm[k], &e->tm[k + 1], sizeof e->tm[0] * (size_t)(e->ntm - k - 1)); e->ntm--; break; }
        return 1;
    }
    case 22: case 23: {                              /* pick a script: random (22) or in turn (23), FUN_1008_96e2 / 9d4a */
        u8 *rw = (u8 *)(uintptr_t)r;                 /* the record keeps its own state at +3 */
        int count, pick;
        if (!r[0x10]) {
            count = r[2]; if (count < 1) return 1; if (count > 6) count = 6;
            if (op == 22) { pick = msc_rand() % count; if (pick == rw[3] && ++pick >= count) pick = 0; rw[3] = (u8)pick; }
            else { if (rw[3] >= count) rw[3] = 0; pick = rw[3]++; }
            int ret = run_script(e, W(4 + pick * 2));
            if (!ret) { *adv = 0; return 0; }
            return 1;
        }
        int li = as_index(V(4));                     /* list form: one statement from a type 9 */
        if (res_type(e, li) != 9) return 1;
        const u8 *lb = resource_bytes(&e->c, &e->c.dir[li]);
        size_t ln = e->c.dir[li].size, q = 0; int tg[8], nt;
        count = 0; while (q < ln) { int L = record_len(lb, q, ln, tg, &nt); if (L <= 0) break; q += (size_t)L; count++; }
        if (!count) return 1;
        if (op == 22) { pick = msc_rand() % count; if (pick == rw[3] && ++pick >= count) pick = 0; rw[3] = (u8)pick; }
        else { pick = rw[3]; if (++rw[3] >= count) rw[3] = 0; if (pick >= count) pick = 0; }
        q = 0; for (int k = 0; k < pick; k++) q += (size_t)record_len(lb, q, ln, tg, &nt);
        int adv2; if (!exec_record(e, lb, q, ln, &adv2)) { *adv = 0; return 0; }
        return 1;
    }
    case 24: {                                       /* swap sprite, FUN_1008_e7a6 / e0ca */
        int o = as_index(V(2)), nw = as_index(V(4));
        Sprite *old = sprite_find(e, o);
        if (!old || res_type(e, nw) != 15) return 1;
        Sprite *ns = old;
        if (nw != o) {
            int x = old->x, y = old->y, fx = old->flipx, fy = old->flipy;
            old->running = 0; old->visible = 0;      /* S_010 + S_006 */
            ns = sprite_load(e, nw);
            if (!ns) return 1;
            ns->x = x; ns->y = y; ns->flipx = fx; ns->flipy = fy;
            if (r[10]) { free(old->prog); memset(old, 0, sizeof *old); }   /* release the old one */
        }
        int s13 = W(6) ? as_index(V(6)) : -1;        /* then place it as op 5 would */
        if (r[11]) ns->visible = 0;
        if (s13 < 0) ns->running = 0; else sprite_program(e, ns, s13);
        if (e->trace) printf("%*s     swap sprite %d -> %d program %d\n", e->depth * 2, "", o, nw, s13);
        if (W(8)) return run_script(e, W(8));
        return 1;
    }
    case 25: {                                       /* break a sprite's loops, S_004 */
        Sprite *sp = sprite_find(e, as_index(V(2)));
        if (sp) sp->brk = r[4];
        return 1;
    }
    case 30:                                         /* queue a call, FUN_1008_ea92 */
        enqueue(e, (u16)(V(2) - IMM_BIAS), (u16)(V(4) - IMM_BIAS), (u16)(V(6) - IMM_BIAS));
        return 1;
    case 35: return 1;                               /* DAT_1020_14e8: pause handler, not modelled */
    case 47: wrw(e, addr_of(e, W(2)), (s16)(e->keydown[W(4) & 0xFF] != 0)); return 1;   /* FUN_1008_934a */
    case 40: {                                       /* sprite position into two variables, S_054 */
        Sprite *sp = sprite_find(e, as_index(V(2)));
        if (!sp) return 1;
        wrw(e, addr_of(e, W(4)), (s16)sp->x); wrw(e, addr_of(e, W(6)), (s16)sp->y);
        if (e->trace && getenv("ENGINE_BTRACE")) printf("POS %d = %d,%d\n", sp->res, sp->x, sp->y);
        return 1;
    }
    case 55: {                                       /* sprite bounding rect into four variables, S_060 */
        Sprite *sp = sprite_find(e, as_index(V(2)));
        if (!sp) return 1;
        int l, t, rr, bb; sprite_rect(e, sp, &l, &t, &rr, &bb);
        wrw(e, addr_of(e, W(4)), (s16)l); wrw(e, addr_of(e, W(6)), (s16)t);
        wrw(e, addr_of(e, W(8)), (s16)rr); wrw(e, addr_of(e, W(10)), (s16)bb);
        if (e->trace && getenv("ENGINE_BTRACE")) printf("RECT %d = %d,%d,%d,%d\n", sp->res, l, t, rr, bb);
        return 1;
    }
    case 45: return 1;                               /* no handler in the original: 2-byte no-op */
    case 31: e->cursor = r[2]; return 1;             /* cursor mode, FUN_1008_e902; 3 and 4 are busy */
    case 33: {                                       /* collision pair, FUN_1008_31b8 */
        if (r[0x0D]) { e->ncol = 0; return 1; }      /* FUN_1008_084c: clear them all */
        int a = as_index(V(2)), b = as_index(V(4));
        if (!sprite_find(e, a) || !sprite_find(e, b)) return 0;
        /* +0E goes with the first sprite, +0F with the second (31b8 swaps them
         * with the sprites; 347e hands them to S_048 as hidA, hidB). Pinball's
         * GRUB lane sensors are hidden until lit and rely on theirs. */
        u8 fa = r[0x0E], fb = r[0x0F];
        if (b < a) { int t = a; a = b; b = t; fa = r[0x0F]; fb = r[0x0E]; }
        int k;
        for (k = 0; k < e->ncol; k++) if (e->col[k].a == a && e->col[k].b == b) break;
        s16 sc = W(6) ? V(6) : 0;
        if (!sc) {                                   /* no script: remove the pair */
            if (k < e->ncol) { e->col[k] = e->col[--e->ncol]; }
            return 1;
        }
        if (k == e->ncol) { if (e->ncol >= 64) return 1; e->ncol++; }
        e->col[k].a = a; e->col[k].b = b;
        e->col[k].script = (u16)(sc - IMM_BIAS);
        e->col[k].arg1 = W(8) ? (u16)(V(8) - IMM_BIAS) : 0;
        e->col[k].arg2 = W(10) ? (u16)(V(10) - IMM_BIAS) : 0;
        e->col[k].repeat = r[0x0C]; e->col[k].hid_a = fa; e->col[k].hid_b = fb; e->col[k].samez = r[0x10];
        return 1;
    }
    case 34: {                                       /* step through a type 9 list, FUN_1008_9f30 */
        int li = as_index(V(2));
        if (res_type(e, li) != 9) return 0;
        const u8 *lb = resource_bytes(&e->c, &e->c.dir[li]);
        size_t ln = e->c.dir[li].size, q = 0;
        int count = 0, tg[8], nt;
        while (q < ln) { int L = record_len(lb, q, ln, tg, &nt); if (L <= 0) break; q += (size_t)L; count++; }
        u16 ia = addr_of(e, W(6));
        int idx = (u16)rdw(e, ia), ret = 1;
        if (idx < count) {                           /* FUN_1008_d056: run item idx as one statement */
            q = 0;
            for (int k = 0; k < idx; k++) q += (size_t)record_len(lb, q, ln, tg, &nt);
            int adv2;
            ret = exec_record(e, lb, q, ln, &adv2);
        } else if (r[8]) ret = run_script(e, W(4));
        if (r[9]) { idx++; if (idx >= count) idx = 0; wrw(e, ia, (s16)idx); }
        if (!ret) { *adv = 0; return 0; }
        return 1;
    }
    case 56: {                                       /* copy or format a string, FUN_1008_9b20 */
        if (W(2) & 0x8000) return 1;                 /* a constant cannot be written */
        char *dst = str_at(e, W(2)), *src = str_at(e, W(4));
        if (!dst || !src) return 1;
        int n = W(6);
        if (n == 0) { char tmp[128]; snprintf(tmp, 128, "%s", src); memcpy(dst, tmp, 128); return 1; }
        s16 num[8]; char *strs[8]; u8 kinds[8]; if (n > 6) n = 6;
        for (int k = 0; k < n; k++) {
            u16 v = W(8 + k * 4); kinds[k] = r[8 + k * 4 + 2];
            if (!kinds[k]) { strs[k] = str_at(e, v); num[k] = 0;   /* a string handle, raw (FUN_1008_e9c2) */ if (!strs[k]) strs[k] = ""; }
            else { strs[k] = NULL; num[k] = vdec(e, v); }
        }
        char tmp[256]; wsprintf16(tmp, sizeof tmp, src, num, strs, n, kinds);
        if (e->trace) fprintf(stderr, "sprintf %04x \"%s\" n=%d -> \"%s\"\n", W(2), src, n, tmp);
        memcpy(dst, tmp, 127); dst[127] = 0;          /* string variables hold 128 bytes */
        return 1;
    }
    case 59: case 60: {                              /* INI read / write, FUN_1008_aa68 */
        const char *sec = W(6) ? str_at(e, W(6)) : "Jungle Games";
        const char *key = str_at(e, W(4));
        if (!sec || !key || !key[0]) return 1;
        int is_str = r[9];
        if (op == 60) {
            char val[128];
            if (!is_str) snprintf(val, sizeof val, "%d", vdec(e, W(2)));
            else { char *sv = str_at(e, W(2)); snprintf(val, sizeof val, "%s", sv ? sv : ""); }
            ini_put(e, sec, key, val);
        } else {
            const char *val = ini_get(e, sec, key);
            if (e->trace) printf("ini read [%s] %s -> %s\n", sec, key, val ? val : "(none)");
            if (!val) return 1;                      /* missing: the variable keeps its default */
            if (!is_str) { if (W(2) < IMM_BASE) wrw(e, addr_of(e, W(2)), (s16)atoi(val)); }
            else if (!(W(2) & 0x8000)) { char *dv = str_at(e, W(2)); if (dv) snprintf(dv, 128, "%s", val); }
        }
        return 1;
    }
    case 83: {                                       /* bind a device to a player, FUN_1008_454c */
        int pl = V(2) & 3, dev = V(4), kind = r[6];
        if (e->trace) printf("%*s     op83 player %d dev %d kind %d flags %d %d\n", e->depth * 2, "", pl, dev, kind, r[7], r[8]);
        if (r[7] || r[8]) return 1;                  /* enable/disable of an existing binding */
        if (kind == 2) { keyboard_enable(e, 1, dev); e->devplayer[(dev + 1) & 7] = (u8)pl; }
        else if (kind == 1) e->devplayer[(dev + 3) & 7] = (u8)pl;   /* joystick: none here */
        else if (kind == 5) e->devplayer[5] = (u8)pl;              /* mouse as joystick */
        return 1;
    }
    case 84: {                                       /* a player's input script, FUN_1008_47f6 */
        int pl = V(2) & 3;
        e->player[pl].script = W(4) ? (u16)(V(4) - IMM_BIAS) : 0;
        return 1;
    }
    case 46: {                                       /* random into a variable, avoiding a repeat */
        int lo = V(4), hi = V(6);
        if (hi < lo) { int t = lo; lo = hi; hi = t; }
        int span = hi - lo + 1, v = msc_rand() % span;
        if (v == e->last_rand) v = (v + 1) % span;
        e->last_rand = v;
        wrw(e, addr_of(e, W(2)), (s16)(lo + v));
        return 1;
    }
    case 12: {                                       /* key binding, FUN_1008_29c4 */
        int vk = W(2);
        u16 sc = W(4);
        for (int k = (vk ? vk : 0); k < (vk ? vk + 1 : 256); k++) {
            if (k > 255) break;
            if (r[8]) { e->keys[k].off = 0; continue; }          /* enable only */
            if (r[9]) { e->keys[k].off = 1; continue; }          /* disable */
            if (r[6]) e->keys[k].ctrl = sc;
            else if (r[7]) e->keys[k].shift = sc;
            else if (r[10]) e->keys[k].up = sc;
            else e->keys[k].down = sc;
            e->keys[k].off = 0;
        }
        if (vk && e->nbind < ENG_MAX_BINDS) { e->bind_vk[e->nbind] = vk; e->bind_to[e->nbind] = sc; e->nbind++; }
        return 1;
    }
    case 18: {                                       /* scene transition, FUN_1008_8c00 */
        int k;
        for (k = 0; k < 14 && r[2 + k]; k++) e->pending[k] = (char)r[2 + k];
        e->pending[k] = 0;
        return 1;
    }
    case 21: e->post_script = W(2); return 1;
    case 29: e->focus_script = W(2); return 1;
    case 37: *adv = (s16)W(2); return *adv != 0;
    case 77: case 79: {
        s16 v = eval(e, r + 6, n - pc - 6);
        *adv = v ? targets[1] : targets[0];
        return 1;
    }
    case 78: case 43: {                              /* switch: FUN_1008_a098 / 9fee */
        s16 v = (op == 78) ? eval(e, r + 10, n - pc - 10) : V(2);
        int cnt = r[8];
        size_t tbl = pc + W(4);
        *adv = (s16)W(6);
        for (int k = 0; k < cnt; k++) {
            size_t q = tbl + (size_t)k * 6;
            if (q + 6 <= n && (s16)rd16(b + q + 2) == v) { *adv = (s16)rd16(b + q + 4); break; }
        }
        return 1;
    }
    case 76: eval(e, r + 4, (size_t)len - 4); return 1;
    case 89: e->result = eval(e, r + 4, n - pc - 4); *adv = 0; return 0;
    default:
        e->unimpl_rec[op & 0xFF]++;
        return 1;
    }
#undef W
#undef V
}

/* FUN_1008_cfe2 + FUN_1008_c418 */
static int run_script(Engine *e, u16 raw)
{
    int idx = as_index(vdec(e, raw));
    if (res_type(e, idx) != 14 || e->depth >= 30) return 0;
    const u8 *b = resource_bytes(&e->c, &e->c.dir[idx]);
    long n = (long)e->c.dir[idx].size, pc = 0;
    if (e->trace) printf("%*s-> script %d\n", e->depth * 2, "", idx);
    e->depth++;
    for (int steps = 0; steps < 100000; steps++) {
        int adv;
        if (!exec_record(e, b, (size_t)pc, (size_t)n, &adv) || adv == 0) break;
        pc += adv;
        if (pc < 0 || pc >= n) break;
        if (e->pending[0]) break;
    }
    e->depth--;
    return 1;
}

/* ---- game input: keyboards as player devices ------------------------------ */

static const u8 DIRTAB[16] = { 0x00, 0x03, 0x05, 0x88, 0x07, 0x0A, 0x0C, 0x88,
                               0x06, 0x09, 0x0B, 0x88, 0x88, 0x88, 0x88, 0x88 };   /* DS 0x54 */

/* FUN_1008_3fe4: deliver an input code from a device to its player's script. */
static void input_event(Engine *e, u8 code, int device)
{
    int pl = e->devplayer[device & 7] & 3;
    EngPlayer *P = &e->player[pl];
    if (code == 0xF7 || code == 0xFB || code == 0xFD || code == 0xFE) P->buttons &= code;
    else if (code == 1 || code == 2 || code == 4 || code == 8) P->buttons |= code;
    else P->code = code;
    if (P->script) enqueue(e, P->script, (u16)(pl - IMM_BIAS), (u16)(code - IMM_BIAS));
    if (e->trace) printf("input code 0x%02x device %d -> player %d script %d\n", code, device, pl, P->script ? (u16)(P->script + IMM_BIAS) : 0);
}

/* FUN_1008_4fde: install (or remove) keyboard layout kbd in the key table. */
static void keyboard_enable(Engine *e, int on, int kbd)
{
    kbd &= 1;
    for (int v = 0; v < 256; v++) if (e->keymap[v].kbd == kbd) { e->keymap[v].armed = 0; e->keymap[v].code = 0; }
    if (!on) return;
    e->dirmask[kbd] = 0;
    for (int k = 0; k < 6; k++) {
        u8 vk = e->layout[kbd][k][0];
        e->keymap[vk].kbd = (u8)kbd; e->keymap[vk].armed = 1; e->keymap[vk].code = e->layout[kbd][k][1];
    }
}

static int is_dir(u8 c) { return c == 0x10 || c == 0x20 || c == 0x40 || c == 0x80; }

static void game_key(Engine *e, int vk, int down)
{
    EngKey *K = &e->keymap[vk & 0xFF];
    if (!K->code) return;
    int kbd = K->kbd & 1;
    if (down) {                                      /* FUN_1008_4e6e */
        if (!K->armed) return;
        K->armed = 0;
        u8 c = K->code;
        if (is_dir(c)) { e->dirmask[kbd] |= c >> 4; c = DIRTAB[e->dirmask[kbd] & 15]; if (c == 0x88) return; }
        input_event(e, c, kbd + 1);
    } else {                                         /* FUN_1008_4efc */
        K->armed = 1;
        u8 c = K->code;
        if (is_dir(c)) {
            e->dirmask[kbd] &= (u8)~(c >> 4);
            c = DIRTAB[e->dirmask[kbd] & 15];        /* the original re-sends after 200 ms */
            if (c == 0x88) return;
        } else c = (u8)~c;
        input_event(e, c, kbd + 1);
    }
}

/* ---- input --------------------------------------------------------------- */

/* FUN_1008_ea92: queue a call, run on the next tick. */
static void enqueue(Engine *e, u16 script, u16 a1, u16 a2)
{
    if (!script || e->nq >= 40) return;
    e->q[e->nq].script = script; e->q[e->nq].a1 = a1; e->q[e->nq].a2 = a2;
    e->q[e->nq].argc = (a1 != 0) + (a2 != 0);
    e->nq++;
}

/* A call with arguments, as FUN_1008_c63e builds it for a zero-size frame. */
static s16 call_with(Engine *e, u16 script, int argc, const u16 *args)
{
    u16 a = e->fp;
    for (int k = 0; k < argc; k++) { wrw(e, a, vdec(e, args[k])); a -= 2; }
    s16 saved = e->result;
    e->result = 0;
    run_script(e, script);
    s16 r = e->result;
    e->result = saved;
    return r;
}

/* FUN_1008_272a: the mouse filter script, if any, may veto the event. */
static int mouse_filter(Engine *e, int code)
{
    if (!e->mouse_script) return 1;
    u16 arg = (u16)(code - IMM_BIAS);
    return call_with(e, e->mouse_script, 1, &arg) != 0;
}

/* The union of the rects of the cels on show (FUN_1000_0b30 keeps +4..+0xA). */
static void sprite_rect(Engine *e, Sprite *s, int *l, int *t, int *r, int *b)
{
    int cels[4], n = 0;
    if (s->ncomp) for (int k = 0; k < s->ncomp; k++) cels[n++] = s->comp[k]; else cels[n++] = s->cur;
    *l = *t = 0x7FFF; *r = *b = -0x7FFF;
    for (int k = 0; k < n; k++) {
        if (cels[k] < 0 || cels[k] >= s->ncel) continue;
        int ox, oy, bi = frame_resolve(&e->c, s->cel[cels[k]] + HANDLE_BASE, &ox, &oy);
        if (bi < 0) continue;
        const u8 *h = resource_bytes(&e->c, &e->c.dir[bi]);
        int w = rd16(h + 2), hh = rd16(h + 6);
        if (s->flipx) ox = -ox - 2 * ((w - 1) / 2);
        int L = s->x + ox, T = s->y + oy;
        if (L < *l) *l = L;
        if (T < *t) *t = T;
        if (L + w > *r) *r = L + w;
        if (T + hh > *b) *b = T + hh;
    }
    if (*l > *r) { *l = *r = s->x; *t = *b = s->y; }
}

/* JUNGS01 FUN_1000_304e on an RLE bitmap: the pixel at (lx, ly), read by walking
 * the row's runs in the compressed data. The walk has no end-of-row test: a 0x00
 * (end of row) reads as a run of length 0, so a point past the last encoded pixel
 * of a row reads on into the next row's bytes rather than coming back transparent. */
static int rle_pixel(const u8 *hdr, size_t size, int lx, int ly)
{
    if (size < 20 || ly < 0 || ly >= rd16(hdr + 6)) return 0;
    const u8 *base = hdr + 20, *end = hdr + size;
    if (base + 2 * ly + 2 > end) return 0;
    const u8 *p = base + rd16(base + 2 * ly);
    int cx = lx;
    for (int guard = 0; guard < 4096; guard++) {
        if (p >= end) return 0;
        int al = *p++;
        if (al <= 0x7F) {                            /* a run: count, then the value */
            p++; cx -= al;
            if (cx >= 0) continue;
            p--;
        } else {                                     /* a literal: 0xFF - b bytes, or 0xFF n */
            int n = al ^ 0xFF;
            if (!n) { if (p >= end) return 0; n = *p++; }
            p += n; cx -= n;
            if (cx >= 0) continue;
            p += cx;
        }
        return p < end ? *p : 0;
    }
    return 0;
}

static int cel_hit(Engine *e, Sprite *s, int celno, int x, int y)
{
    if (celno < 0 || celno >= s->ncel) return 0;
    int ox, oy, bi = frame_resolve(&e->c, s->cel[celno] + HANDLE_BASE, &ox, &oy);
    if (bi < 0) return 0;
    int w, h; const u8 *px = bitmap(e, bi, &w, &h);
    if (!px) return 0;
    const u8 *hdr = resource_bytes(&e->c, &e->c.dir[bi]);
    if (s->flipx) { int sw = rd16(hdr + 2); ox = -ox - 2 * ((sw - 1) / 2); }
    int lx = x - (s->x + ox), ly = y - (s->y + oy);
    if (lx < 0 || ly < 0 || lx >= w || ly >= h) return 0;
    if (s->flipx) lx = w - 1 - lx;
    if (rd16(hdr + 8) & 0x8000) return rle_pixel(hdr, e->c.dir[bi].size, lx, ly) != 0;
    return px[(size_t)ly * w + lx] != 0;
}

/* S_048: do two sprites overlap? Rects first (FUN_1000_5dd2), then opaque
 * pixels against opaque pixels over the intersection (FUN_1000_2306/081e). */
static int sprite_opaque_at(Engine *e, Sprite *s, int x, int y)
{
    if (s->ncomp) { for (int k = 0; k < s->ncomp; k++) if (cel_hit(e, s, s->comp[k], x, y)) return 1; return 0; }
    return cel_hit(e, s, s->cur, x, y);
}

static int sprites_collide(Engine *e, Sprite *A, Sprite *B, int samez, int hid_a, int hid_b)
{
    if (samez && A->z != B->z) return 0;
    if (!A->visible && !hid_a) return 0;
    if (!B->visible && !hid_b) return 0;
    int al, at, ar, ab, bl, bt, br, bb;
    sprite_rect(e, A, &al, &at, &ar, &ab); sprite_rect(e, B, &bl, &bt, &br, &bb);
    int l = al > bl ? al : bl, t = at > bt ? at : bt, r = ar < br ? ar : br, b = ab < bb ? ab : bb;
    if (l >= r || t >= b) return 0;
    for (int y = t; y < b; y++)
        for (int x = l; x < r; x++)
            if (sprite_opaque_at(e, A, x, y) && sprite_opaque_at(e, B, x, y)) return 1;
    return 0;
}

/* S_029: the topmost visible sprite with an opaque pixel under the point. */
static Sprite *sprite_at(Engine *e, int x, int y)
{
    Sprite *best = NULL;
    for (int i = 0; i < ENG_MAX_SPRITES; i++) {
        Sprite *s = &e->spr[i];
        if (!s->used || !(s->visible || s->hit_hidden)) continue;
        int hit;
        if (s->hit_rect) {                           /* +0x5e: the bounding rect is enough */
            int l, t, r, b; sprite_rect(e, s, &l, &t, &r, &b);
            hit = x >= l && x < r && y >= t && y < b;
        } else hit = sprite_opaque_at(e, s, x, y);
        if (hit && (!best || s->z >= best->z)) best = s;
    }
    return best;
}

void engine_mouse(Engine *e, int x, int y, int button, int down)
{
    if (button) e->mheld = down ? (u8)(e->mheld | (1 << button)) : (u8)(e->mheld & ~(1 << button));
    if (e->fade.dir) return;                         /* input waits out a fade */
    x -= ORG_X; y -= ORG_Y;                                     /* FUN_1008_26e2: client -> logical */
    wrw(e, 0x3C38, (s16)x); wrw(e, 0x3C3A, (s16)y);           /* globals 5005/5006 */
    if (!button) {                                              /* FUN_1008_30ae */
        if (!mouse_filter(e, 4)) return;
        if (e->pressed) {                                       /* drag the held object */
            Sprite *sp = sprite_find(e, e->pressed);
            if (sp) { sp->x += x - e->drag_x; sp->y += y - e->drag_y; }
            e->drag_x = x; e->drag_y = y;
            return;
        }
        int handled = 0;
        if (e->hover_script) {                                  /* FUN_1008_23c8 */
            Sprite *sp = sprite_at(e, x, y);
            int now = sp ? sp->res : 0;
            if (now != e->hovered) {
                if (e->hovered) enqueue(e, e->hover_script, (u16)(e->hovered - IMM_BIAS), (u16)(0 - IMM_BIAS));
                if (now) enqueue(e, e->hover_script, (u16)(now - IMM_BIAS), (u16)(1 - IMM_BIAS));
                e->hovered = now;
            }
            handled = sp != NULL;
        }
        if (!handled && e->nreg) {                              /* FUN_1008_2310: hover regions */
            int k;
            for (k = e->nreg - 1; k >= 0; k--)
                if (x >= e->reg[k].l && x < e->reg[k].r && y >= e->reg[k].t && y < e->reg[k].b) break;
            if (k + 1 != e->inreg) {
                if (e->inreg > 0 && e->reg[e->inreg - 1].leave) enqueue(e, e->reg[e->inreg - 1].leave, (u16)(0 - IMM_BIAS), 0);
                if (k >= 0 && e->reg[k].enter) enqueue(e, e->reg[k].enter, (u16)(1 - IMM_BIAS), 0);
                e->inreg = k + 1;
            }
        }
        return;
    }
    if (down) {                                                 /* FUN_1008_2b04 */
        if (e->pressed || e->cursor == 3 || e->cursor == 4) {        /* FUN_1008_2b04: no clicks while busy */
            if (e->trace) printf("click %d,%d ignored: pressed %d cursor %d\n", x, y, e->pressed, e->cursor);
            return;
        }
        wrw(e, 0x3C30, (s16)button);
        wrw(e, 0x3C3C, (s16)x); wrw(e, 0x3C3E, (s16)y);
        if (!mouse_filter(e, 1)) { if (e->trace) printf("click %d,%d vetoed by the mouse filter\n", x, y); return; }
        u16 script = 0, tag = 0;
        Sprite *s = sprite_at(e, x, y);
        if (s) {
            const u8 *rec = resource_bytes(&e->c, &e->c.dir[s->res]);
            tag = (u16)(s->res - IMM_BIAS);
            if (rec[0x11]) { script = rd16(rec + 6); e->pressed = s->res; e->drag_x = x; e->drag_y = y; }
            else if (!e->click_off[s->res]) script = e->click[s->res];
        }
        if (!script)
            for (int k = e->nhot - 1; k >= 0 && !script; k--)
                if (!e->hot[k].off && x >= e->hot[k].l && x < e->hot[k].r && y >= e->hot[k].t && y < e->hot[k].b)
                    script = e->hot[k].script;
        if (e->trace) printf("click %d,%d -> sprite %d script 0x%04x\n", x, y, s ? s->res : -1, script);
        if (script) enqueue(e, script, tag, 0);
    } else {                                                    /* FUN_1008_2d2a */
        int pass = mouse_filter(e, 2);               /* FUN_1008_272a(2): the filter always sees the release */
        if (pass && e->pressed) {
            wrw(e, 0x3C40, (s16)x); wrw(e, 0x3C42, (s16)y);
            const u8 *rec = resource_bytes(&e->c, &e->c.dir[e->pressed]);
            enqueue(e, rd16(rec + 8), (u16)(e->pressed - IMM_BIAS), 0);
        }
        e->pressed = 0;
    }
}

/* RESCOPYRESOURCE: a new directory entry pointing at the same bytes. The
 * per-resource tables grow with it. */
static int clone_resource(Engine *e, int src)
{
    int n = e->c.ndir;
    Entry *d = realloc(e->c.dir, sizeof *d * (size_t)(n + 1));
    if (!d) return -1;
    e->c.dir = d; d[n] = d[src]; e->c.ndir = n + 1;
    void *bm = realloc(g_bm, sizeof *g_bm * (size_t)(n + 1));
    u16 *ck = realloc(e->click, sizeof *e->click * (size_t)(n + 1));
    u8 *co = realloc(e->click_off, (size_t)(n + 1));
    void *tx = realloc(g_txt, sizeof *g_txt * (size_t)(n + 1));
    if (tx) { g_txt = tx; memset(&g_txt[n], 0, sizeof *g_txt); }
    void *sn = realloc(g_snd, sizeof *g_snd * (size_t)(n + 1));
    if (sn) { g_snd = sn; memset(&g_snd[n], 0, sizeof *g_snd); }
    if (bm) { g_bm = bm; memset(&g_bm[n], 0, sizeof *g_bm); g_nbm = n + 1; }
    if (ck) { e->click = ck; e->click[n] = e->click[src]; }
    if (co) { e->click_off = co; e->click_off[n] = e->click_off[src]; }
    return n;
}

/* ---- lifecycle ------------------------------------------------------------ */

int engine_init(Engine *e, const char *dir)
{
    memset(e, 0, sizeof *e);
    e->dir = dir;
    e->mem = calloc(0x8000, sizeof(u16));
    synth_init(&e->synth, 22050);
    e->fade.faded = 1; e->fade.level = 1;            /* DAT_1020_5a46 starts set: the first op 10 fades in */
    char dll[600]; snprintf(dll, sizeof dll, "%s/JUNGA01.DLL", dir);
    adpcm_load_tables(dll);                          /* the ADPCM step tables live in JUNGA01 */
    load_sine(dir);
    char fpath[600]; snprintf(fpath, sizeof fpath, "%s/HYENA.TTF", dir);   /* the disc's own font */
    FILE *ff = fopen(fpath, "rb");
    if (ff) {
        fseek(ff, 0, SEEK_END); long fl = ftell(ff); fseek(ff, 0, SEEK_SET);
        g_font = malloc((size_t)fl);
        if (g_font && fread(g_font, 1, (size_t)fl, ff) == (size_t)fl && stbtt_InitFont(&g_fi, g_font, 0)) g_font_ok = 1;
        fclose(ff);
    }
    if (g_sin[900] != 10000 || g_sin[0] != 0) fprintf(stderr, "engine: JUNGU01 sine table not found in %s\n", dir);
    e->bg = -1; e->bgfill = 0;
    if (!e->mem) return 0;
    /* FUN_1008_b82e publishes system facts into script globals 5002..5014
     * (DS 0x3C32..0x3C4E). Scripts pick hi- or lo-res art from these. */
    wrw(e, 0x3C32, 0);          /* low-res preference from the .INI: off */
    wrw(e, 0x3C34, ENG_W);      /* GetDeviceCaps HORZRES */
    wrw(e, 0x3C36, ENG_H);      /* GetDeviceCaps VERTRES */
    wrw(e, 0x3C38, 0);          /* mouse x, client coordinates */
    wrw(e, 0x3C3A, 0);          /* mouse y */
    wrw(e, 0x3C48, 5);          /* GetProcessorType: Pentium */
    static const u8 lay[2][6][2] = {                 /* DS 0x3C, from the EXE's data segment */
        { {'F', 0x10}, {'V', 0x20}, {'C', 0x80}, {'B', 0x40}, {'1', 1}, {'2', 2} },
        { {0x26, 0x10}, {0x28, 0x20}, {0x25, 0x80}, {0x27, 0x40}, {'9', 1}, {'0', 2} } };
    memcpy(e->layout, lay, sizeof lay);
    return 1;
}

static void unload(Engine *e)
{
    for (int i = 0; i < ENG_MAX_SPRITES; i++) { free(e->spr[i].prog); }
    memset(e->spr, 0, sizeof e->spr);
    for (int i = 0; i < g_nbm; i++) free(g_bm[i].px);
    for (int i = 0; i < g_nbm && g_snd; i++) free(g_snd[i].pcm);
    free(g_snd); g_snd = NULL;
    for (int i = 0; i < g_nbm && g_txt; i++) free(g_txt[i].px);
    free(g_txt); g_txt = NULL;
    memset(e->voice, 0, sizeof e->voice);
    music_stop(e);                                   /* the track goes with its container */
    e->fade.level = 1; e->fade.dir = 0;              /* the new scene's palette is realised at full strength */
    e->edit.on = 0;
    free(g_bm); g_bm = NULL; g_nbm = 0;
    if (e->open) { free(e->c.data); free(e->c.dir); e->open = 0; }
    e->nbind = 0; e->bg = -1; e->bgfill = 0; memset(e->keys, 0, sizeof e->keys);
    e->post_script = e->focus_script = 0;
    free(e->click); free(e->click_off); e->click = NULL; e->click_off = NULL;
    e->nhot = 0; e->mouse_script = 0; e->pressed = 0; e->hover_script = 0; e->hovered = 0; e->key_filter = 0; e->nreg = 0; e->inreg = 0; e->nq = 0; e->ntm = 0; e->nsnd = 0; e->ncol = 0;
    memset(e->keymap, 0, sizeof e->keymap); memset(e->player, 0, sizeof e->player); memset(e->devplayer, 0, sizeof e->devplayer);
}

/* Globals 0..5000 (DS 0x151E, 0x2712 bytes) belong to a container: FUN_1008_beca
 * saves them when leaving, FUN_1008_bb7e restores them when coming back, and a
 * first visit starts from zero plus the container's variable table (key
 * resource 3, {index, value} pairs, via RESENUMVARIABLES). Higher globals are
 * shared by every scene. */
#define IMG_WORDS 5001
static u16 *saved_image(Engine *e, const char *name, int create)
{
    for (int i = 0; i < e->nsaved; i++) if (!strcmp(e->saved[i].name, name)) return e->saved[i].img;
    if (!create || e->nsaved >= 16) return NULL;
    snprintf(e->saved[e->nsaved].name, 16, "%s", name);
    e->saved[e->nsaved].img = calloc(IMG_WORDS, sizeof(u16));
    e->saved[e->nsaved].str = NULL; e->saved[e->nsaved].nstr = 0;
    return e->saved[e->nsaved++].img;
}

/* Corrections to the disc's own data, applied to a container's initial globals.
 * Each is a defect in the 1995 data, not in the engine; JUNGLE_ORIGINAL=1 keeps
 * the disc exactly as it is. See docs/FIDELITY.md, "Table fixes". */
static void table_fixes(Engine *e)
{
    if (getenv("JUNGLE_ORIGINAL")) return;
    s16 *g = (s16 *)e->mem + (VAR_BASE >> 1);
    if (!strcmp(e->name, "JUNGPINB.BIN") && g[2496] == 146 && g[2501] == -193) {
        /* Pinball, the rightmost GRUB lane post: its collision segment starts at the
         * stake's top-left corner, outside the stake's pixel mask, where the other four
         * start inside theirs. A ball dropping onto the stake's top-right shoulder is
         * then stopped by the mask but judged by the segment to be moving away, so it
         * neither moves nor bounces, for good. Start the segment inside the stake. */
        g[2496] = 154; g[2501] = -185;
    }
}

int engine_load(Engine *e, const char *name)
{
    char path[512], up[16]; int k;
    for (k = 0; k < 15 && name[k]; k++) up[k] = (char)(name[k] >= 'a' && name[k] <= 'z' ? name[k] - 32 : name[k]);
    up[k] = 0;
    if (e->open) {                                    /* save the outgoing scene's globals */
        u16 *img = saved_image(e, e->name, 1);
        if (img) memcpy(img, e->mem + (VAR_BASE >> 1), IMG_WORDS * sizeof(u16));
        for (int i = 0; i < e->nsaved; i++) if (!strcmp(e->saved[i].name, e->name)) {
            free(e->saved[i].str); e->saved[i].str = e->str; e->saved[i].nstr = e->nstr;
            e->str = NULL; e->nstr = 0;
        }
    }
    unload(e);
    snprintf(path, sizeof path, "%s/%s", e->dir, up);
    if (!container_open(&e->c, path)) { fprintf(stderr, "engine: cannot open %s\n", path); return 0; }
    e->open = 1;
    snprintf(e->name, sizeof e->name, "%s", up);
    free(e->str); e->str = NULL; e->nstr = 0;
    u16 *img = saved_image(e, up, 0);
    for (int i = 0; i < e->nsaved; i++) if (!strcmp(e->saved[i].name, up) && e->saved[i].str) {
        e->str = e->saved[i].str; e->nstr = e->saved[i].nstr; e->saved[i].str = NULL;
    }
    if (!e->str) { e->nstr = rd16(e->c.data + 0xA8) + 1; e->str = calloc((size_t)e->nstr, 128); }
    if (img) memcpy(e->mem + (VAR_BASE >> 1), img, IMG_WORDS * sizeof(u16));
    else {
        memset(e->mem + (VAR_BASE >> 1), 0, IMG_WORDS * sizeof(u16));
        u32 voff = rd32(e->c.data + OFF_TABLES + 3 * 8), vlen = rd32(e->c.data + OFF_TABLES + 3 * 8 + 4);
        for (u32 i = 0; i + 4 <= vlen && voff + i + 4 <= e->c.len; i += 4) {
            u16 gi = rd16(e->c.data + voff + i), gv = rd16(e->c.data + voff + i + 2);
            if (gi < IMG_WORDS) e->mem[(VAR_BASE >> 1) + gi] = gv;
        }
        table_fixes(e);
    }
    g_nbm = e->c.ndir;
    g_bm = calloc((size_t)g_nbm, sizeof *g_bm);
    g_snd = calloc((size_t)g_nbm, sizeof *g_snd);
    g_txt = calloc((size_t)g_nbm, sizeof *g_txt);
    e->click = calloc((size_t)e->c.ndir, sizeof *e->click);
    e->click_off = calloc((size_t)e->c.ndir, 1);
    for (int i = 0; i < e->c.ndir; i++)
        if (e->c.dir[i].type == 15 && e->c.dir[i].size >= 0x14) {
            const u8 *r = resource_bytes(&e->c, &e->c.dir[i]);
            e->click[i] = rd16(r); e->click_off[i] = r[0x0E];
        }
    memset(e->mem + (0x4064 >> 1), 0, 0x44);          /* bd72 clears these, not the globals */
    e->fp = FP_INIT;
    e->pending[0] = 0;
    run_script(e, (u16)(0 - IMM_BIAS));               /* cfe2(DAT_1020_14ea - 0x7531), 14ea = 0 */
    return 1;
}

/* Pinball: a ball the table's own physics can no longer move. The movement
 * scripts stop a ball wherever a pixel mask blocks its next step and rely on the
 * collision scripts to bounce it; where a collision line judges the ball to be
 * moving away (a mask and its line disagreeing, or a corner between two), the
 * ball neither moves nor bounces and stays there for good. After two seconds
 * perfectly still, away from the plunger and with no flipper held (a cradled
 * ball is the player's choice), send it gently upward, off the vertical to one
 * side and then the other: a ball at rest under gravity is always held from
 * below, so up is the way out of any wedge, and the table's gravity brings it
 * back down clear of it. The balls: x g1900.., y g1903.., speed g1894.., heading g1897..,
 * 1 in play g1917... JUNGLE_ORIGINAL=1 turns this off with the table fixes. */
static void pinball_unstick(Engine *e)
{
    static int original = -1;
    if (original < 0) original = getenv("JUNGLE_ORIGINAL") != NULL;
    if (original || e->timers_paused || strcmp(e->name, "JUNGPINB.BIN")) return;
    s16 *g = (s16 *)e->mem + (VAR_BASE >> 1);
    int holding = e->keydown['Z'] || e->keydown[0xBF] || e->keydown[0x26] || e->mheld;
    for (int i = 0; i < 3; i++) {
        s16 x = g[1900 + i], y = g[1903 + i];
        int moved = x != e->unstick[i].x || y != e->unstick[i].y;
        if (moved) e->unstick[i].tries = 0;
        e->unstick[i].x = x; e->unstick[i].y = y;
        if (moved || g[1917 + i] != 1 || holding || (x > 300 && y > 100)) { e->unstick[i].since = e->now; continue; }
        if (e->now - e->unstick[i].since < 2000) continue;
        int k = ++e->unstick[i].tries, off = 300 + 150 * ((k - 1) / 2 % 3);   /* 30, 45, 60 degrees off vertical */
        int up = (k & 1) ? 1800 - off : 1800 + off;  /* up and to the right, then up and to the left */
        g[1897 + i] = (s16)up;
        if (g[1894 + i] < 12) g[1894 + i] = 12;
        e->unstick[i].since = e->now;
        if (e->trace || getenv("ENGINE_UNSTICK_LOG")) fprintf(stderr, "pinball: ball %d still at %d,%d; sent off at %d\n", i, x, y, up);
    }
}

void engine_tick(Engine *e, u32 now)
{
    e->now = now;
    mix_to(e, now);
    {                                                 /* ENGINE_WATCH=N: log changes to global N */
        static int watch = -2; static s16 last;
        if (watch == -2) watch = getenv("ENGINE_WATCH") ? atoi(getenv("ENGINE_WATCH")) : -1;
        if (watch >= 0) { s16 v = (s16)e->mem[(0x151E + watch * 2) / 2]; if (v != last) { fprintf(stderr, "watch g%d = %d at %u in %s\n", watch, v, now, e->name); last = v; } }
    }
    fade_update(e);
    if (e->fade.dir) return;
    pinball_unstick(e);
    if (e->warp.pending) { e->warp.pending = 0; engine_mouse(e, e->warp.x, e->warp.y, 0, 0); }   /* the WM_MOUSEMOVE it causes */                          /* a fade holds the engine, as its loop did */
    if (e->pending[0]) {                              /* WM_USER+0xC9 -> FUN_1008_beca */
        char nm[16]; snprintf(nm, sizeof nm, "%s", e->pending);
        e->pending[0] = 0;
        engine_load(e, nm);
        return;
    }
    if (e->post_script) {                             /* WM_USER+0xC8 -> FUN_1008_ef84 */
        u16 s = e->post_script; e->post_script = 0;
        run_script(e, s);
    }
    for (int k = 0; k < e->ntm && !e->pending[0] && !e->timers_paused; k++) {   /* FUN_1008_df36 */
        if ((int)(now - e->tm[k].due) < 0) continue;
        u16 sc = e->tm[k].script;
        if (e->trace) fprintf(stderr, "timer %u -> script %u at %u (due %u)\n", e->tm[k].id, sc, now, e->tm[k].due);
        if (e->tm[k].repeat) e->tm[k].due = now + e->tm[k].every;
        else { memmove(&e->tm[k], &e->tm[k + 1], sizeof e->tm[0] * (size_t)(e->ntm - k - 1)); e->ntm--; k--; }
        run_script(e, sc);
    }
    for (int k = 0; k < e->ncol && !e->pending[0]; k++) {      /* FUN_1008_347e */
        Sprite *A = sprite_find(e, e->col[k].a), *B = sprite_find(e, e->col[k].b);
        if (!A || !B || !sprites_collide(e, B, A, e->col[k].samez, e->col[k].hid_b, e->col[k].hid_a)) continue;
        u16 sc = e->col[k].script, a1 = e->col[k].arg1, a2 = e->col[k].arg2;
        if (!e->col[k].repeat) { e->col[k] = e->col[--e->ncol]; k--; }
        enqueue(e, sc, a1, a2);
    }
    for (int k = 0; k < e->nsnd; k++)                           /* sound finished */
        if ((int)(now - e->snd[k].due) >= 0) {
            enqueue(e, e->snd[k].script, e->snd[k].tag, 0);
            e->snd[k--] = e->snd[--e->nsnd];
        }
    while (e->nq && !e->pending[0]) {               /* deferred calls, oldest first */
        u16 args[2]; int n = 0;
        u16 script = e->q[0].script;
        if (e->q[0].a1) args[n++] = e->q[0].a1;
        if (e->q[0].a2) args[n++] = e->q[0].a2;
        memmove(&e->q[0], &e->q[1], sizeof e->q[0] * (size_t)(--e->nq));
        call_with(e, script, n, args);
    }
    for (int i = 0; i < ENG_MAX_SPRITES; i++) if (e->spr[i].used) sprite_tick(e, &e->spr[i]);
}

void engine_keystate(Engine *e, int vk, int down)
{
    e->keydown[vk & 0xFF] = (u8)(down != 0);
    if (down) return;
    game_key(e, vk, 0);
    if (vk < 256 && !e->keys[vk].off && e->keys[vk].up) run_script(e, e->keys[vk].up);   /* WM_KEYUP */
}

/* Text entry, FUN_1008_da12 / da98 / dc3c: the field edits its type 16
 * object's text in place with a '_' cursor on the end. Enter keeps the text
 * and Escape restores what it held before; either copies it to the string
 * variable and queues the field's done script. */
static void edit_finish(Engine *e, int cancel)
{
    if (!e->edit.on || !g_txt) return;
    char *t = g_txt[e->edit.ri].text;
    if (cancel) snprintf(t, sizeof g_txt[0].text, "%s", e->edit.saved);
    else { size_t n = strlen(t); if (n) t[n - 1] = 0; }            /* drop the cursor */
    if (!(e->edit.strvar & 0x8000)) {
        char *dst = str_at(e, e->edit.strvar);
        if (dst) { strncpy(dst, t, 127); dst[127] = 0; }
    }
    if (e->edit.done) enqueue(e, e->edit.done, 0, 0);
    e->edit.on = 0;
    text_render(e, e->edit.ri);
}

s16 engine_builtin(Engine *e, int id, s16 *a, int argc) { return builtin(e, id, a, argc); }

int engine_sprite_rect(Engine *e, int res, int *l, int *t, int *r, int *b)
{
    Sprite *sp = sprite_find(e, res);
    if (!sp) return 0;
    sprite_rect(e, sp, l, t, r, b);
    *l += ORG_X; *r += ORG_X; *t += ORG_Y; *b += ORG_Y;   /* canvas pixels */
    return 1;
}

void engine_char(Engine *e, int c)
{
    if (e->trace) fprintf(stderr, "char 0x%02x: edit %d ri %d max %d digits %d fade %d\n", c, e->edit.on, e->edit.ri, e->edit.maxlen, e->edit.digits, e->fade.dir);
    if (!e->edit.on || !g_txt || e->fade.dir) return;
    char *t = g_txt[e->edit.ri].text;
    size_t n = strlen(t);                            /* includes the cursor */
    if (c == 0x1B || c == 0x0D) { edit_finish(e, c == 0x1B); return; }
    if (c == 8) {
        if (n < 2) return;
        t[n - 2] = '_'; t[n - 1] = 0;
    } else if ((c == 10 || c > 0x1F) && c != 9 && c != '@' && c < 256) {
        if (e->edit.digits && !(c >= '0' && c <= '9')) return;
        if (n - 1 >= e->edit.maxlen || n + 1 >= sizeof g_txt[0].text) return;
        t[n - 1] = (char)c; t[n] = '_'; t[n + 1] = 0;
    } else return;
    text_render(e, e->edit.ri);
}

int engine_edit_len(Engine *e)
{
    if (!e->edit.on || !g_txt) return -1;
    size_t n = strlen(g_txt[e->edit.ri].text);       /* includes the cursor */
    return n ? (int)n - 1 : 0;
}

void engine_key(Engine *e, int vk)
{
    if (e->fade.dir) return;                         /* input waits out a fade */
    e->keydown[vk & 0xFF] = 1;
    game_key(e, vk, 1);
    if (e->key_filter) {                                        /* FUN_1008_2696 */
        u16 arg = (u16)(vk - IMM_BIAS);
        if (!call_with(e, e->key_filter, 1, &arg)) return;
    }
    if (e->edit.on) return;                          /* bindings wait while a field takes text */
    if (vk < 0 || vk > 255 || e->keys[vk].off) return;     /* FUN_1008_2f72 */
    u16 sc = e->keydown[0x11] && e->keys[vk].ctrl ? e->keys[vk].ctrl
           : e->keydown[0x10] && e->keys[vk].shift ? e->keys[vk].shift : e->keys[vk].down;
    if (sc) run_script(e, sc);
}

static void draw_cel(Engine *e, u8 *fb, Sprite *s, int celno)
{
    if (celno < 0 || celno >= s->ncel) return;
    if (res_type(e, s->cel[celno]) == 16) {
        int w, h, ox, oy; const u8 *px = text_cel(e, s->cel[celno], &w, &h, &ox, &oy);
        if (px) blit8(fb, ENG_W, ENG_W, ENG_H, ORG_X + s->x + ox, ORG_Y + s->y + oy, px, w, w, h, 0);
        return;
    }
    int ox, oy, bi = frame_resolve(&e->c, s->cel[celno] + HANDLE_BASE, &ox, &oy);
    if (bi < 0) return;
    int w, h; const u8 *px = bitmap(e, bi, &w, &h);
    if (!px) return;
    unsigned fl = 0;
    if (s->flipx) {
        int sw = rd16(resource_bytes(&e->c, &e->c.dir[bi]) + 2);
        ox = -ox - 2 * ((sw - 1) / 2);
        fl = BLIT_FLIP_X;
    }
    blit8(fb, ENG_W, ENG_W, ENG_H, ORG_X + s->x + ox, ORG_Y + s->y + oy, px, w, w, h, fl);
    const char *pr = getenv("ENGINE_PROBE");                   /* debug: who covers x,y */
    if (pr) {
        int qx = atoi(pr), qy = strchr(pr, ',') ? atoi(strchr(pr, ',') + 1) : 0;
        int L = ORG_X + s->x + ox, T = ORG_Y + s->y + oy;
        if (qx >= L && qx < L + w && qy >= T && qy < T + h)
            printf("probe %d,%d: sprite %d z %d cel %d bmp %d rect %d,%d %dx%d pixel %d\n", qx, qy,
                   s->res, s->z, celno, bi, L, T, w, h, px[(qy - T) * w + (s->flipx ? w - 1 - (qx - L) : qx - L)]);
    }
}

/* Fade progress: one step per 60 Hz frame. Out shows (n-k)/n from k = 2,
 * in shows k/n from k = 1, as FUN_1008_397a counts. */
#define FADE_STEP_MS 17
static void fade_update(Engine *e)
{
    if (!e->fade.dir) return;
    int n = e->fade.n, k = (int)((e->now - e->fade.t0) / FADE_STEP_MS);
    if (e->fade.dir < 0) {
        k += 2;
        if (k >= n) { e->fade.level = 0; e->fade.dir = 0; }
        else e->fade.level = (float)(n - k) / n;
    } else {
        k += 1;
        if (k >= n) { e->fade.level = 1; e->fade.dir = 0; }
        else e->fade.level = (float)k / n;
    }
}

void engine_palette(Engine *e, u8 pal[256][3])
{
    fade_update(e);
    memcpy(pal, e->c.pal, sizeof e->c.pal);
    if (e->fade.level >= 1) return;
    for (int i = 10; i < 246; i++)
        for (int c = 0; c < 3; c++) pal[i][c] = (u8)(pal[i][c] * e->fade.level);
}

void engine_render(Engine *e, u8 *fb)
{
    if (e->fade.dir < 0 && e->fade.snap) { memcpy(fb, e->fade.snap, (size_t)ENG_W * ENG_H); return; }
    engine_render_scene(e, fb);
}

static void engine_render_scene(Engine *e, u8 *fb)
{
    memset(fb, e->bgfill < 0 ? 0 : e->bgfill, (size_t)ENG_W * ENG_H);
    int w, h; const u8 *px = bitmap(e, e->bg, &w, &h);
    if (px) blit8(fb, ENG_W, ENG_W, ENG_H, 0, 0, px, w, w, h, 0);
    int order[ENG_MAX_SPRITES], n = 0;
    if (getenv("ENGINE_BGONLY")) return;
    int skip = getenv("ENGINE_SKIP") ? atoi(getenv("ENGINE_SKIP")) : -1;
    for (int i = 0; i < ENG_MAX_SPRITES; i++) if (e->spr[i].used && e->spr[i].visible && e->spr[i].res != skip) order[n++] = i;
    for (int i = 1; i < n; i++)                        /* stable insertion sort by z */
        for (int j = i; j > 0 && e->spr[order[j - 1]].z > e->spr[order[j]].z; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    for (int i = 0; i < n; i++) {
        Sprite *s = &e->spr[order[i]];
        if (s->ncomp) for (int k = 0; k < s->ncomp; k++) draw_cel(e, fb, s, s->comp[k]);
        else draw_cel(e, fb, s, s->cur);
    }
}

void engine_free(Engine *e) { unload(e); free(e->mem); e->mem = NULL; }

static int draw_item(Engine *e, Sprite *s, int celno, EngDraw *d)
{
    if (celno < 0 || celno >= s->ncel) return 0;
    memset(d, 0, sizeof *d);
    if (res_type(e, s->cel[celno]) == 16) {
        int w, h, ox, oy; const u8 *px = text_cel(e, s->cel[celno], &w, &h, &ox, &oy);
        if (!px) return 0;
        d->kind = 2; d->res = s->cel[celno]; d->px = px; d->w = w; d->h = h;
        d->x = ORG_X + s->x + ox; d->y = ORG_Y + s->y + oy; d->version = g_txt[d->res].version;
        return 1;
    }
    int ox, oy, bi = frame_resolve(&e->c, s->cel[celno] + HANDLE_BASE, &ox, &oy);
    if (bi < 0) return 0;
    int w, h; const u8 *px = bitmap(e, bi, &w, &h);
    if (!px) return 0;
    if (s->flipx) {
        int sw = rd16(resource_bytes(&e->c, &e->c.dir[bi]) + 2);
        ox = -ox - 2 * ((sw - 1) / 2);
        d->flip = 1;
    }
    d->kind = 1; d->res = bi; d->px = px; d->w = w; d->h = h;
    d->x = ORG_X + s->x + ox; d->y = ORG_Y + s->y + oy;
    return 1;
}

/* The same painting order as engine_render_scene, as a list. */
int engine_drawlist(Engine *e, EngDraw *out, int max)
{
    int n = 0;
    fade_update(e);
    if (e->fade.dir < 0 && e->fade.snap) {          /* fading out: the frozen 8-bit frame */
        if (max > 0) { memset(out, 0, sizeof *out); out->kind = 3; n = 1; }
        return n;
    }
    if (n < max) { memset(&out[n], 0, sizeof *out); out[n].kind = 0; out[n].fill = (u8)(e->bgfill < 0 ? 0 : e->bgfill); n++; }
    int w, h; const u8 *px = bitmap(e, e->bg, &w, &h);
    if (px && n < max) { memset(&out[n], 0, sizeof *out); out[n].kind = 1; out[n].res = e->bg; out[n].px = px; out[n].w = w; out[n].h = h; n++; }
    int order[ENG_MAX_SPRITES], ns = 0;
    for (int i = 0; i < ENG_MAX_SPRITES; i++) if (e->spr[i].used && e->spr[i].visible) order[ns++] = i;
    for (int i = 1; i < ns; i++)
        for (int j = i; j > 0 && e->spr[order[j - 1]].z > e->spr[order[j]].z; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    for (int i = 0; i < ns && n < max; i++) {
        Sprite *s = &e->spr[order[i]];
        if (s->ncomp) { for (int k = 0; k < s->ncomp && n < max; k++) n += draw_item(e, s, s->comp[k], &out[n]); }
        else n += draw_item(e, s, s->cur, &out[n]);
    }
    return n;
}
