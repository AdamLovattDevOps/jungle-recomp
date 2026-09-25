/* jungle-recomp — native viewer for 7th Level "7L" containers.
 *
 * Loads a .BIN, decodes its bitmaps with both original codecs, and presents
 * them through SDL2. This is the port's foundation: the container layer, the
 * codecs and the palette pipeline, running natively.
 *
 * Build:  cc -O2 -o jungle src/jungle.c $(sdl2-config --cflags --libs)
 * Run:    ./jungle orig/cd/JUNGLE/JUNGOPTS.BIN
 *         arrows / space browse bitmaps, 'a' plays audio, q quits
 */
#include <SDL.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
static void *g_web_engine;                        /* the running game (Engine *), for the page's touch buttons */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "blit.h"
#include "gif.h"
#include "timing.h"

#include "core.h"
#include "engine.h"
#include "hires.h"
#include <sys/stat.h>

/* ---- container ------------------------------------------------------- */

int container_open(Container *c, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); c->len = (size_t)ftell(f); rewind(f);
    c->data = malloc(c->len);
    if (fread(c->data, 1, c->len, f) != c->len) { fclose(f); return 0; }
    fclose(f);

    if (rd16(c->data) != MAGIC) { fprintf(stderr, "bad magic\n"); return 0; }
    if (rd16(c->data + OFF_VERSION) != 2) { fprintf(stderr, "bad version\n"); return 0; }

    /* the engine's own validity test: 8-bit sum of the header must be zero */
    u16 hdr = rd16(c->data + 2);
    u8 sum = 0;
    for (u16 i = 0; i < hdr; i++) sum = (u8)(sum + c->data[i]);
    if (sum != 0) { fprintf(stderr, "header checksum failed\n"); return 0; }

    u32 dir_off = rd32(c->data + OFF_TABLES);
    u32 dir_len = rd32(c->data + OFF_TABLES + 4);
    c->ndir = (int)(dir_len / ENTRY_SIZE);
    c->dir = malloc(sizeof(Entry) * (size_t)c->ndir);
    for (int i = 0; i < c->ndir; i++) {
        const u8 *e = c->data + dir_off + (size_t)i * ENTRY_SIZE;
        c->dir[i].type   = rd16(e);
        c->dir[i].offset = rd32(e + 2);
        c->dir[i].size   = rd32(e + 6);
    }

    u32 blob_off = rd32(c->data + OFF_TABLES + 5 * 8);
    c->blob_len  = rd32(c->data + OFF_TABLES + 5 * 8 + 4);
    c->blob      = c->data + blob_off;

    /* palette: 236 entries mapping to indices 10..245 (Windows reserves 20) */
    u32 pal_off = rd32(c->data + OFF_PALETTE);
    u32 pal_len = rd32(c->data + OFF_PALETTE + 4);
    memset(c->pal, 0, sizeof c->pal);
    u32 n = pal_len / 4;
    u32 base = 10;               /* always after the 10 low static colours */
    for (u32 i = 0; i < n && base + i < 256; i++) {
        const u8 *q = c->data + pal_off + i * 4;
        c->pal[base + i][0] = q[0];   /* PALETTEENTRY is R,G,B,flags */
        c->pal[base + i][1] = q[1];
        c->pal[base + i][2] = q[2];
    }
    for (int i = base + n; i < 256; i++)
        c->pal[i][0] = c->pal[i][1] = c->pal[i][2] = 255;
    return 1;
}

const u8 *resource_bytes(const Container *c, const Entry *e)
{
    if (e->type >= RESIDENT_LO && e->type <= RESIDENT_HI)
        return c->blob + e->offset;
    return c->data + e->offset;
}

/* ---- RLE codec (FUN_1000_0230) ---------------------------------------- */

static void unrle(const u8 *src, size_t n, u8 *dst, int w, int h)
{
    size_t p = 0;
    int row = 0, x = 0;
    memset(dst, 0, (size_t)w * h);
    while (p < n && row < h) {
        u8 b = src[p++];
        if (b == 0) { row++; x = 0; if (p < n && src[p] == 0) break; continue; }
        if (b < 0x80) {
            if (p >= n) break;
            u8 v = src[p++];
            for (int i = 0; i < b && x < w; i++) dst[(size_t)row * w + x++] = v;
        } else {
            int len = 0xFF - b;
            if (b == 0xFF) { if (p >= n) break; len = src[p++]; }
            for (int i = 0; i < len && p < n; i++)
                { u8 v = src[p++]; if (x < w) dst[(size_t)row * w + x++] = v; }
        }
    }
}

/* ---- LZW codec (FUN_1000_218c / 263a / 2470) -------------------------- */

typedef struct { const u8 *d; size_t n, pos; u32 acc; int bits; } Bits;

static int bits_read(Bits *b, int width)
{
    while (b->bits < width) {
        if (b->pos >= b->n) return -1;
        b->acc = (b->acc << 8) | b->d[b->pos++];
        b->bits += 8;
    }
    b->bits -= width;
    int v = (int)((b->acc >> b->bits) & ((1u << width) - 1));
    b->acc &= (1u << b->bits) - 1;
    return v;
}

static size_t lzw_block(const u8 *in, size_t n, int width, u8 *out, size_t cap)
{
    int end = (1 << width) - 1, maxc = (1 << width) - 2, next = 0x100;
    static int prefix[4096]; static u8 suffix[4096];
    u8 stack[4096];
    Bits b = { in, n, 0, 0, 0 };
    size_t o = 0;
    int code = bits_read(&b, width);
    if (code < 0 || code == end) return 0;
    if (o < cap) out[o++] = (u8)code;
    int prev = code, first = code;
    for (;;) {
        code = bits_read(&b, width);
        if (code < 0 || code == end) break;
        int c = code, sp = 0;
        if (code >= next) { c = prev; }
        while (c >= 0x100 && sp < 4095) { stack[sp++] = suffix[c]; c = prefix[c]; }
        stack[sp++] = (u8)c;
        first = c;
        while (sp > 0 && o < cap) out[o++] = stack[--sp];
        if (code >= next && o < cap) out[o++] = (u8)first;   /* KwKwK */
        if (next <= maxc) { prefix[next] = prev; suffix[next] = (u8)first; next++; }
        prev = code;
    }
    return o;
}

static size_t lzw_expand(const u8 *in, size_t n, u8 *out, size_t cap)
{
    size_t p = 0, o = 0;
    while (p + 2 <= n) {
        u16 tag = rd16(in + p); p += 2;
        u16 len = tag & 0x1FFF, method = tag & 0xE000;
        if (method == 0xE000) break;
        if (method == 0x0000) {
            for (u16 i = 0; i < len && o < cap; i++) out[o++] = in[p + i];
        } else if (method == 0x4000 || method == 0x6000 || method == 0x8000) {
            int width = method == 0x4000 ? 10 : method == 0x6000 ? 11 : 12;
            size_t q = 0;
            while (q + 2 <= len) {          /* sub-blocks, dictionary per block */
                u16 bl = rd16(in + p + q); q += 2;
                if (!bl || q + bl > len) break;
                o += lzw_block(in + p + q, bl, width, out + o, cap - o);
                q += bl;
            }
        } else break;
        p += len;
    }
    return o;
}

/* ---- ADPCM codec (FUN_1000_3f10 in JUNGA01) ---------------------------
 *
 * 4-bit ADPCM, two samples per byte, low nibble first. IMA-derived but not
 * IMA: the adaptation index runs at 8x resolution, so the index table is
 * IMA's times eight, the clamp is 88*8, and the step table is interpolated
 * to ~712 entries. A stock IMA decoder produces wrong output.
 *
 * The tables are loaded from JUNGA01.DLL's data segment rather than
 * hardcoded, so the decoder uses exactly what shipped.
 */
#define ADPCM_STEP_OFF   0x1E0
#define ADPCM_INDEX_OFF  0x770
#define ADPCM_INDEX_MAX  0x2C0

static u16 *g_step = NULL; static int g_nstep = 0;
static s16  g_index_adj[16];

int adpcm_load_tables(const char *dll_path)
{
    FILE *f = fopen(dll_path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    u8 *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return 0; }
    fclose(f);
    /* The tables are at these offsets within the DLL's DATA SEGMENT, not
     * within the file. Segment 2 of JUNGA01 starts at file offset 0x4B50, so
     * reading them at the raw file offset yields unrelated bytes — which
     * decodes to silence, because the step table then begins with zeros.
     * Locate the segment through the NE header rather than hardcoding it. */
    if (n < 0x40) { free(d); return 0; }
    u32 ne = rd32(d + 0x3C);
    if (ne + 0x34 > (u32)n || d[ne] != 'N' || d[ne+1] != 'E') { free(d); return 0; }
    u16 seg_tab   = (u16)(ne + rd16(d + ne + 0x22));
    u16 align     = rd16(d + ne + 0x32); if (!align) align = 9;
    u16 nsegs     = rd16(d + ne + 0x1C);
    if (nsegs < 2) { free(d); return 0; }
    u32 seg2_off  = (u32)rd16(d + seg_tab + 8) << align;      /* segment 2 */
    if (seg2_off + ADPCM_INDEX_OFF + 32 > (u32)n) { free(d); return 0; }
    const u8 *seg = d + seg2_off;

    g_nstep = (ADPCM_INDEX_OFF - ADPCM_STEP_OFF) / 2;
    g_step = malloc(sizeof(u16) * (size_t)g_nstep);
    for (int i = 0; i < g_nstep; i++) g_step[i] = rd16(seg + ADPCM_STEP_OFF + i * 2);
    for (int i = 0; i < 16; i++) g_index_adj[i] = (s16)rd16(seg + ADPCM_INDEX_OFF + i * 2);
    free(d);
    return 1;
}

size_t adpcm_decode(const u8 *in, size_t n, s16 *out)
{
    int pred = 0, index = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int nib[2] = { in[i] & 0x0F, in[i] >> 4 };
        for (int k = 0; k < 2; k++) {
            int v = nib[k];
            int si = (v & 7) + index;
            if (si >= g_nstep) si = g_nstep - 1;
            int delta = g_step[si];
            if (v & 8) delta = -delta;
            pred += delta;
            if (pred > 32767) pred = 32767;
            if (pred < -32768) pred = -32768;
            index += g_index_adj[v];
            if (index < 0) index = 0;
            if (index > ADPCM_INDEX_MAX) index = ADPCM_INDEX_MAX;
            out[o++] = (s16)pred;
        }
    }
    return o;
}

/* ---- bitmap ----------------------------------------------------------- */

int bitmap_decode(const u8 *src, size_t size, int *w_out, int *h_out, u8 **px)
{
    if (size < BMP_HDR || rd16(src) != BMP_HDR) return 0;
    int w = rd16(src + 4), h = rd16(src + 6);
    u16 flags = rd16(src + 8);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    u8 *buf = calloc((size_t)w * h, 1);
    if (flags & FLAG_RLE) {
        size_t skip = BMP_HDR + (size_t)h * 2;   /* per-row offset table */
        if (skip < size) unrle(src + skip, size - skip, buf, w, h);
    } else {
        lzw_expand(src + BMP_HDR, size - BMP_HDR, buf, (size_t)w * h);
    }
    *w_out = w; *h_out = h; *px = buf;
    return 1;
}

/* ---- script VM (FUN_1008_1bf2) ----------------------------------------
 *
 * One stack, not two: the decompiled source indexes 0x70C and 0x70E both by
 * sp*2, so the slot at 0x70E+sp*2 IS 0x70C+(sp+1)*2. Top of stack is S[sp];
 * push writes S[sp+1] then increments.
 *
 * Variables are addressed, not named:
 *   index <  0x13FE   addr = 0x151E + index*2      script global
 *   index <  0x159F   addr = FRAME  - index*2      call-frame local
 *   otherwise         immediate, value index + 0x7531
 */
#define VM_VAR_BASE   0x151E
#define VM_VAR_SPLIT  0x13FE
#define VM_IMM_BASE   0x159F
#define VM_IMM_BIAS   0x7531
#define VM_FRAME_BASE 0x27FC
#define VM_STACK      128
#define VM_MEM        0x10000

typedef struct { s16 *mem; int err; } VM;

static int vm_wide(int op)  { return op==1||op==2||op==4||op==37||op==38; }
static int vm_wide4(int op) { return op==12||op==13; }

static int vm_addr(int index)
{
    if (index < VM_VAR_SPLIT) return (VM_VAR_BASE + index * 2) & 0xFFFF;
    return (VM_FRAME_BASE - index * 2) & 0xFFFF;
}

static s16 vm_run(VM *vm, const u8 *code, size_t n)
{
    s16 S[VM_STACK]; int sp = 0; size_t p = 0;
    memset(S, 0, sizeof S);
    vm->err = 0;
    while (p < n) {
        int op = code[p];
        if (op == 0) return S[sp];
        long arg = -1;
        if (vm_wide4(op))     { if (p+5 > n) { vm->err = 1; return 0; } arg = (long)rd32(code+p+1); p += 5; }
        else if (vm_wide(op)) { if (p+3 > n) { vm->err = 1; return 0; } arg = rd16(code+p+1);       p += 3; }
        else p += 1;
        if (sp < 0 || sp >= VM_STACK-1) { vm->err = 2; return 0; }

        switch (op) {
        case 1:  S[++sp] = (s16)arg; break;
        case 2:  S[++sp] = (arg >= VM_IMM_BASE) ? (s16)(arg + VM_IMM_BIAS)
                                                : vm->mem[vm_addr((int)arg)]; break;
        case 4:  S[++sp] = (arg >= VM_IMM_BASE) ? (s16)(arg + VM_IMM_BIAS)
                                                : (s16)vm_addr((int)arg); break;
        case 3:  sp--; S[sp] = vm->mem[(u16)(S[sp+1]*2 + S[sp])]; break;
        case 6:  sp--; S[sp] = (s16)(S[sp+1]*2 + S[sp]); break;
        case 8:  sp--; vm->mem[(u16)S[sp]] = S[sp+1]; S[sp] = S[sp+1]; break;
        case 39: sp++; S[sp] = vm->mem[(u16)S[sp-1]]; break;
        case 34: { u16 a = (u16)S[sp]; S[sp] = vm->mem[a]; vm->mem[a]++; } break;
        case 5: case 12: case 13: case 37: case 38: break;
        case 14: S[sp] = (s16)(-S[sp]); break;
        case 15: S[sp] = (s16)(S[sp] == 0); break;
        case 16: S[sp] = (s16)(~S[sp]); break;
        case 33: S[sp]++; break;
        case 35: case 36: S[sp]--; break;
        default:
            if (op >= 17 && op <= 32) {
                sp--; int x = S[sp], y = S[sp+1], r = 0;
                switch (op) {
                case 17: r = x + y; break;  case 18: r = x - y; break;
                case 19: r = x * y; break;
                case 20: if (!y) { vm->err = 3; return 0; } r = x / y; break;
                case 21: if (!y) { vm->err = 3; return 0; } r = x % y; break;
                case 22: r = x == y; break; case 23: r = x >  y; break;
                case 24: r = x >= y; break; case 25: r = x <  y; break;
                case 26: r = x <= y; break; case 27: r = x != y; break;
                case 28: r = x && y; break; case 29: r = x || y; break;
                case 30: r = x & y;  break; case 31: r = x | y;  break;
                case 32: r = x ^ y;  break;
                }
                S[sp] = (s16)r;
            } else { vm->err = 4; return 0; }
        }
    }
    return S[sp];
}

/* ---- scene record walker (FUN_1008_c724) ------------------------------
 *
 * Record length is a property of the opcode, not a stored field, and three
 * opcodes are control flow, so the stream is a graph rather than a list:
 *   37       jump; the advance is SIGNED, so negative is a backward jump (loops)
 *   77, 79   conditional branch: u16[1] if false, u16[2] if true
 *   78, 43   switch: count at +8, table at p+u16[+4], default at +6, 6-byte entries
 *   0, 44    terminators (zero advance, no handler)
 * A linear walk desynchronises immediately; both arms must be followed.
 */
static const short OPCODE_LEN[256] = {
 /*  0*/  -1, -1,  4,  4, 18,  8, 14, 10, 28,  4, -1,  4, 12,  2,  4,  8,
 /* 16*/  24, -1, 18, 12,  6,  4, 18, 18, 12,  6,  6,  6,  6,  4,  8,  4,
 /* 32*/   6, 18, 10,  4,  8, -1, -1,  8,  8,  8, 10, -1,  0,  2,  8,  6,
 /* 48*/  10, 12, 20, -1,  8,  4, 12, 12, 32, -1,  8, -1, 10, 16, 22,  8,
 /* 64*/   4, -1,  6,  4,  6, -1, 10,  6,  6,  6, -1, 12, -1, -1, -1, -1,
 /* 80*/  10,  4, 16, 10,  6,  8, 16,  6, 10,  0, -1, -1, 14,  4,  6, -1,
 /* 96*/   4, 12, -1, -1, -1, -1, -1, -1, -1, -1,  4, -1, -1, -1, -1, -1,
 /*112*/  -1, -1, -1, -1, -1, -1, -1, 16, -1, -1, -1, -1, -1, -1, -1, -1,
 /*128*/  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
 /*144*/  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
 /*160*/  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
 /*176*/  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
 /*192*/  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
 /*208*/  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
 /*224*/  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
 /*240*/  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static size_t expr_len(const u8 *b, size_t p, size_t n)
{
    size_t start = p;
    while (p < n) {
        int op = b[p];
        if (op == 0) return p + 1 - start;
        p += vm_wide4(op) ? 5 : vm_wide(op) ? 3 : 1;
    }
    return n - start;
}

/* Returns length, or 0 for a terminator, or -1 if unknown. Fills up to 8
 * branch targets (offsets relative to the record start). */
int record_len(const u8 *b, size_t p, size_t n, int *targets, int *ntarget)
{
    *ntarget = 0;
    if (p + 2 > n) return -1;
    int op = rd16(b + p);
    if (op == 0 || op == 44) return 0;
    if (op == 37) {
        /* JUMP. The field is a distance, not a length: the record is four
         * bytes and the advance says where execution continues. Counting the
         * distance as a span marks every skipped byte as walked, which
         * inflates coverage without decoding anything in between. */
        if (p + 4 > n) return -1;
        s16 adv = (s16)rd16(b + p + 2);
        targets[(*ntarget)++] = adv;
        return -2;
    }
    if (op == 76) {
        if (p + 4 > n) return -1;
        return (s16)rd16(b + p + 2);
    }
    if (op == 77 || op == 79) {
        if (p + 6 > n) return -1;
        targets[(*ntarget)++] = rd16(b + p + 2);
        targets[(*ntarget)++] = rd16(b + p + 4);
        return (int)(6 + expr_len(b, p + 6, n));
    }
    if (op == 78 || op == 43) {
        if (p + 10 > n) return -1;
        int cnt = b[p + 8];
        size_t tbl = p + (op == 78 ? rd16(b + p + 4) : (size_t)(s16)rd16(b + p + 4));
        targets[(*ntarget)++] = rd16(b + p + 6);
        for (int i = 0; i < cnt && *ntarget < 8; i++) {
            size_t q = tbl + (size_t)i * 6;
            if (q + 6 <= n) targets[(*ntarget)++] = rd16(b + q + 4);
        }
        return op == 78 ? (int)(10 + expr_len(b, p + 10, n)) : 10;
    }
    if (op == 38) {                      /* compare two variables, two arms */
        if (p + 4 > n) return -1;
        targets[(*ntarget)++] = rd16(b + p + 2);
        targets[(*ntarget)++] = 10;
        return 10;
    }
    if (op == 89) return (int)(4 + expr_len(b, p + 4, n));
    if (op == 1)  return p + 6 <= n ? (b[p + 5] + 3) * 2 : -1;
    if (op == 10 || op == 11) return 4;
    if (op == 59 || op == 60) return 10;
    if (op == 17) return 14;
    if (op < 256 && OPCODE_LEN[op] > 0) return OPCODE_LEN[op];
    return -1;
}

/* ---- PNG writer -------------------------------------------------------
 * Indexed-colour PNG, so the output keeps the original 8-bit indices and the
 * game's own palette rather than being flattened to RGB. That makes a dumped
 * file directly comparable with the Python extractor's output.
 */
/* PNG needs only CRC-32 and a zlib stream; stored (uncompressed) deflate
 * blocks keep this dependency-free on every platform. */
static u32 crc_update(u32 c, const u8 *p, size_t n)
{
    static u32 t[256]; static int init;
    if (!init) {
        for (u32 i = 0; i < 256; i++) { u32 k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; t[i] = k; }
        init = 1;
    }
    c = ~c;
    while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static u8 *zlib_stored(const u8 *raw, size_t n, size_t *out_len)
{
    size_t blocks = n / 65535 + 1, cap = 2 + n + blocks * 5 + 4;
    u8 *z = malloc(cap), *o = z;
    if (!z) return NULL;
    *o++ = 0x78; *o++ = 0x01;
    size_t p = 0;
    do {
        size_t len = n - p > 65535 ? 65535 : n - p;
        *o++ = (u8)(p + len >= n);
        *o++ = (u8)len; *o++ = (u8)(len >> 8); *o++ = (u8)~len; *o++ = (u8)(~len >> 8);
        memcpy(o, raw + p, len); o += len; p += len;
    } while (p < n);
    u32 a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    u32 ad = (b << 16) | a;
    *o++ = (u8)(ad >> 24); *o++ = (u8)(ad >> 16); *o++ = (u8)(ad >> 8); *o++ = (u8)ad;
    *out_len = (size_t)(o - z);
    return z;
}

static void png_chunk(FILE *f, const char *tag, const u8 *data, u32 len)
{
    u8 hdr[4] = { (u8)(len>>24), (u8)(len>>16), (u8)(len>>8), (u8)len };
    fwrite(hdr, 1, 4, f);
    fwrite(tag, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    u32 crc = crc_update(0, (const u8 *)tag, 4);
    if (len) crc = crc_update(crc, data, len);
    u8 c[4] = { (u8)(crc>>24), (u8)(crc>>16), (u8)(crc>>8), (u8)crc };
    fwrite(c, 1, 4, f);
}

static int g_png_trns;                                /* --export: palette index 0 is transparent */
static int png_write(const char *path, int w, int h, const u8 *px, u8 pal[256][3])
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);

    u8 ihdr[13] = { (u8)(w>>24),(u8)(w>>16),(u8)(w>>8),(u8)w,
                    (u8)(h>>24),(u8)(h>>16),(u8)(h>>8),(u8)h, 8, 3, 0, 0, 0 };
    png_chunk(f, "IHDR", ihdr, 13);

    u8 plte[768];
    for (int i = 0; i < 256; i++)
        { plte[i*3] = pal[i][0]; plte[i*3+1] = pal[i][1]; plte[i*3+2] = pal[i][2]; }
    png_chunk(f, "PLTE", plte, 768);
    if (g_png_trns) { u8 trns[1] = { 0 }; png_chunk(f, "tRNS", trns, 1); }

    /* rows are stored bottom-up, emit top-down, each with a zero filter byte */
    size_t raw_len = (size_t)(w + 1) * h;
    u8 *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (w + 1)] = 0;
        memcpy(raw + (size_t)y * (w + 1) + 1, px + (size_t)(h - 1 - y) * w, (size_t)w);
    }
    size_t zlen = 0;
    u8 *z = zlib_stored(raw, raw_len, &zlen);
    png_chunk(f, "IDAT", z, (u32)zlen);
    png_chunk(f, "IEND", NULL, 0);
    free(raw); free(z); fclose(f);
    return 1;
}

/* ---- main ------------------------------------------------------------- */

/* Headless self-test: decode every bitmap and report, so the codecs can be
 * verified without a display and checked against the Python reference. */
static int verify(Container *c, const char *name)
{
    int ok = 0, fail = 0, total = 0;
    long pixels = 0;
    for (int i = 0; i < c->ndir; i++) {
        if (c->dir[i].type != 1) continue;
        total++;
        int w, h; u8 *px;
        if (bitmap_decode(resource_bytes(c, &c->dir[i]), c->dir[i].size, &w, &h, &px)) {
            ok++; pixels += (long)w * h; free(px);
        } else fail++;
    }
    printf("%-22s %6d bitmaps  %6d decoded  %4d failed  %9ld pixels\n",
           name, total, ok, fail, pixels);
    return fail;
}


/* Resolve one entry of a sprite frame list to a drawable bitmap plus its
 * origin.
 *
 * A frame list entry is a resource handle which may be:
 *   type 1  a bitmap, drawn at the origin in its own 20-byte header
 *   type 10 a 6-byte record { u16 bitmap handle, i16 x, i16 y } -- the same
 *           bitmap repositioned. FUN_1008_5a66 copies the bitmap's control
 *           block and then overwrites the two origin fields, so the record's
 *           coordinates replace the bitmap's own rather than offsetting them.
 *           Verified: 4,755 of 4,755 type 10 records across all 13 containers
 *           are 6 bytes whose first handle is a type 1.
 * Anything else (types 7, 13, 16, ...) is not a bitmap and is skipped here.
 *
 * Returns the directory index of the bitmap, or -1.
 */
int frame_resolve(const Container *c, int handle, int *ox, int *oy)
{
    /* Returns the bitmap index and, in ox and oy, its top-left relative to the
     * sprite anchor. JUNGS01 FUN_1000_0b30 centres each cel on the anchor and
     * shifts it by the header offsets, +0x0A for x and +0x0C for y:
     *   left = x - ((w-1)/2 - hdr[+0x0A])     w = hdr[+2]
     *   top  = y - ((h-1)/2 - hdr[+0x0C])     h = hdr[+6]
     * A type 10 copies the header and overrides +0x0A/+0x0C with its x, y. */
    const int HANDLE_BASE = 0x10000 - 0x7531;
    int idx = handle - HANDLE_BASE, offx, offy;
    if (idx < 0 || idx >= c->ndir) return -1;

    if (c->dir[idx].type == 10) {
        if (c->dir[idx].size < 6) return -1;
        const u8 *r = resource_bytes(c, &c->dir[idx]);
        int inner = rd16(r) - HANDLE_BASE;
        if (inner < 0 || inner >= c->ndir || c->dir[inner].type != 1) return -1;
        offx = (short)rd16(r + 2);
        offy = (short)rd16(r + 4);
        idx = inner;
    } else if (c->dir[idx].type == 1) {
        const u8 *hdr = resource_bytes(c, &c->dir[idx]);
        offx = (short)rd16(hdr + 0x0A);
        offy = (short)rd16(hdr + 0x0C);
    } else return -1;

    const u8 *hdr = resource_bytes(c, &c->dir[idx]);
    int w = rd16(hdr + 0x02), h = rd16(hdr + 0x06);
    *ox = offx - (w - 1) / 2;
    *oy = offy - (h - 1) / 2;
    return idx;
}


/* ---- scene execution, headless core -----------------------------------
 *
 * Runs one type 14 script and reports what it did: key bindings installed,
 * sprites activated, and any scene transition requested. The same executor
 * --run uses, factored out so the interactive game loop can call it.
 */
typedef struct {
    int  vk[64], target[64], n;      /* key -> script bindings */
    int  act[256], nact;             /* sprites activated */
    char scene[40];                  /* container to switch to, or empty */
} SceneOut;

static void scene_exec(const Container *c, int res, SceneOut *o, int depth)
{
    if (res < 0 || res >= c->ndir || c->dir[res].type != 14 || depth > 8) return;
    const u8 *b = resource_bytes(c, &c->dir[res]);
    size_t n = c->dir[res].size, pc = 0;
    VM vm; vm.mem = calloc(VM_MEM, sizeof(s16)); vm.err = 0;
    int steps = 0;

    while (pc < n && steps++ < 20000) {
        int op = rd16(b + pc), targets[8], nt = 0;
        int len = record_len(b, pc, n, targets, &nt);
        if (op == 0 || op == 44) break;

        if (op == 76) { if (len < 4) break; vm_run(&vm, b + pc + 4, (size_t)len - 4); pc += (size_t)len; }
        else if (op == 89) { vm_run(&vm, b + pc + 4, n - (pc + 4)); pc += (size_t)len; }
        else if (op == 77 || op == 79) {
            s16 v = vm_run(&vm, b + pc + 6, n - (pc + 6));
            size_t t = pc + (size_t)(v ? targets[1] : targets[0]);
            if (t >= n) break;
            pc = t;
        } else if (op == 37) {
            s16 adv = (s16)rd16(b + pc + 2);
            if (!adv || (long)pc + adv < 0 || (size_t)((long)pc + adv) >= n) break;
            pc = (size_t)((long)pc + adv);
        } else if (op == 78 || op == 43) {
            s16 v = (op == 78) ? vm_run(&vm, b + pc + 10, n - (pc + 10)) : 0;
            size_t t = pc + (size_t)targets[0];
            int cnt = b[pc + 8], k;
            size_t tbl = pc + (op == 78 ? rd16(b + pc + 4) : (size_t)(s16)rd16(b + pc + 4));
            for (k = 0; k < cnt; k++) {
                size_t q = tbl + (size_t)k * 6;
                if (q + 6 <= n && (s16)rd16(b + q) == v) { t = pc + rd16(b + q + 4); break; }
            }
            if (t >= n) break;
            pc = t;
        } else if (op == 12) {                       /* bind key -> script */
            if (o->n < 64) {
                o->vk[o->n] = rd16(b + pc + 2);
                o->target[o->n] = rd16(b + pc + 4) - (0x10000 - 0x7531);
                o->n++;
            }
            pc += (size_t)len;
        } else if (op == 28) {                       /* activate sprite */
            int ri = rd16(b + pc + 2) - (0x10000 - 0x7531);
            if (ri >= 0 && ri < c->ndir && o->nact < 256) o->act[o->nact++] = ri;
            pc += (size_t)len;
        } else if (op == 18) {                       /* scene transition, FUN_1008_8c00 */
            int k;                                   /* 14-byte name at +2, NUL padded */
            for (k = 0; k < 14 && k < 39 && pc + 2 + k < n && b[pc + 2 + k]; k++)
                o->scene[k] = (char)b[pc + 2 + k];
            o->scene[k] = 0;
            pc += (size_t)len;
        } else if (op == 1) {                        /* in-scene call */
            int t2 = rd16(b + pc + 6) - (0x10000 - 0x7531);
            scene_exec(c, t2, o, depth + 1);
            pc += (size_t)len;
        } else {
            if (len <= 0) break;
            pc += (size_t)len;
        }
    }
    free(vm.mem);
}

/* --drive: the engine under a program's control, for bots and end-to-end tests.
 * One command per stdin line; each reply ends with a line "." so the reader
 * knows it is complete. Time only moves on "run".
 *   run MS              advance MS milliseconds in 16 ms ticks
 *   key VK / up VK      press (with its WM_CHAR) / release a virtual key, hex
 *   click X Y [r]       move, press, hold ~100 ms, release
 *   move X Y            move the pointer
 *   char C              type a character code (decimal)
 *   scene | t           current container | engine time
 *   g N [M]             globals N..M
 *   spr RES             one sprite: x y visible (or none)
 *   sprites             visible sprites: res x y z cel w h l t r b (w, h of the current cel;
 *                       l t r b its bounds in canvas pixels)
 *   shot PATH           write the frame as a PNG
 *   quit */
static int drive_mode(Engine *e, FILE *wav, u32 *wav_n)
{
    char line[512]; u32 t = 0;
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    while (fgets(line, sizeof line, stdin)) {
        char cmd[32] = ""; int a = 0, b = 0; char path[400] = "";
        int n = sscanf(line, "%31s", cmd);
        if (n < 1) continue;
        #define TICK() do { engine_tick(e, t); if (wav) { s16 buf[4096]; u32 got; \
            while ((got = engine_audio(e, buf, 4096)) > 0) { fwrite(buf, 2, got, wav); *wav_n += got; } } } while (0)
        if (!strcmp(cmd, "run")) {
            sscanf(line, "%*s %d", &a);
            for (u32 end = t + (u32)a; t < end; ) { t += 16; TICK(); }
        } else if (!strcmp(cmd, "key")) {
            sscanf(line, "%*s %x", &a);
            engine_key(e, a);
            int ch = 0;
            if (a >= 0x41 && a <= 0x5A) ch = e->keydown[0x10] ? a : a + 0x20;
            else if ((a >= 0x30 && a <= 0x39) || a == 0x20 || a == 0x08 || a == 0x0D || a == 0x1B) ch = a;
            if (ch) engine_char(e, ch);
        } else if (!strcmp(cmd, "up")) {
            sscanf(line, "%*s %x", &a); engine_keystate(e, a, 0);
        } else if (!strcmp(cmd, "char")) {
            sscanf(line, "%*s %d", &a); engine_char(e, a);
        } else if (!strcmp(cmd, "move")) {
            sscanf(line, "%*s %d %d", &a, &b); engine_mouse(e, a, b, 0, 0);
        } else if (!strcmp(cmd, "click")) {
            char r[8] = ""; sscanf(line, "%*s %d %d %7s", &a, &b, r);
            int btn = r[0] == 'r' ? 2 : 1;
            engine_mouse(e, a, b, 0, 0); t += 16; TICK();
            engine_mouse(e, a, b, btn, 1);
            for (int k = 0; k < 6; k++) { t += 16; TICK(); }
            engine_mouse(e, a, b, btn, 0);
        } else if (!strcmp(cmd, "scene")) {
            printf("%s\n", e->name);
        } else if (!strcmp(cmd, "t")) {
            printf("%u\n", t);
        } else if (!strcmp(cmd, "g")) {
            int m = sscanf(line, "%*s %d %d", &a, &b); if (m < 2) b = a;
            for (int i = a; i <= b && i < 0x13FE; i++) printf("%d%c", (s16)e->mem[(0x151E + i * 2) / 2], i == b ? '\n' : ' ');
        } else if (!strcmp(cmd, "spr")) {             /* one sprite: x y visible, or "none" */
            sscanf(line, "%*s %d", &a);
            int k; for (k = 0; k < ENG_MAX_SPRITES; k++) if (e->spr[k].used && e->spr[k].res == a) break;
            if (k < ENG_MAX_SPRITES) printf("%d %d %d\n", e->spr[k].x, e->spr[k].y, e->spr[k].visible); else printf("none\n");
        } else if (!strcmp(cmd, "sprites")) {
            for (int i = 0; i < ENG_MAX_SPRITES; i++) {
                Sprite *sp = &e->spr[i];
                if (!sp->used || !sp->visible) continue;
                int w = 0, h = 0, ox = 0, oy = 0;
                int cel = sp->ncel ? (sp->ncomp ? sp->cel[sp->comp[0]] : sp->cel[sp->cur]) : -1;
                int bi = cel >= 0 ? frame_resolve(&e->c, cel + (0x10000 - 0x7531), &ox, &oy) : -1;
                if (bi >= 0) { const u8 *r = resource_bytes(&e->c, &e->c.dir[bi]); w = rd16(r + 2); h = rd16(r + 6); }
                int l = 0, tp = 0, rr = 0, bb = 0; engine_sprite_rect(e, sp->res, &l, &tp, &rr, &bb);
                printf("%d %d %d %d %d %d %d %d %d %d %d\n", sp->res, sp->x, sp->y, sp->z, sp->cur, w, h, l, tp, rr, bb);
            }
        } else if (!strcmp(cmd, "shot")) {
            sscanf(line, "%*s %399s", path);
            u8 *fb = malloc(ENG_W * ENG_H), *up = malloc(ENG_W * ENG_H);
            engine_render(e, fb);
            for (int y = 0; y < ENG_H; y++) memcpy(up + (size_t)y * ENG_W, fb + (size_t)(ENG_H - 1 - y) * ENG_W, ENG_W);
            u8 dpal[256][3]; engine_palette(e, dpal);
            png_write(path, ENG_W, ENG_H, up, dpal);
            free(fb); free(up);
        } else if (!strcmp(cmd, "call")) {            /* call ID ARGS...: a script builtin, hex id */
            s16 args[8]; int na = 0, id = 0, off = 0, v;
            char *p = line + 4; if (sscanf(p, "%x%n", &id, &off) == 1) p += off;
            while (na < 8 && sscanf(p, "%d%n", &v, &off) == 1) { args[na++] = (s16)v; p += off; }
            printf("%d\n", engine_builtin(e, id, args, na));
        } else if (!strcmp(cmd, "state")) {           /* what drives the scene right now */
            int ns = 0, nv = 0, nr = 0, nk = 0;
            for (int i = 0; i < ENG_MAX_SPRITES; i++) if (e->spr[i].used) { ns++; nv += e->spr[i].visible; nr += e->spr[i].running; }
            for (int v = 0; v < 256; v++) if (e->keys[v].down || e->keys[v].up || e->keys[v].shift || e->keys[v].ctrl) nk++;
            printf("scene %s sprites %d visible %d running %d timers %d keys %d hotspots %d regions %d collisions %d queue %d music %d voices",
                   e->name, ns, nv, nr, e->ntm, nk, e->nhot, e->nreg, e->ncol, e->nq, e->mus.on);
            int nvo = 0; for (int k = 0; k < 16; k++) nvo += e->voice[k].pcm != NULL; printf(" %d\n", nvo);
            for (int i = 0; i < e->ntm; i++) printf("timer %u every %u repeat %d\n", e->tm[i].id, e->tm[i].every, e->tm[i].repeat);
            for (int v = 0; v < 256; v++) if (e->keys[v].down || e->keys[v].up) printf("key 0x%02x%s%s\n", v, e->keys[v].down ? " down" : "", e->keys[v].up ? " up" : "");
            for (int i = 0; i < 4; i++) if (e->player[i].script) printf("player %d input-script\n", i);
        } else if (!strcmp(cmd, "trace")) {           /* trace 1 / trace 0: the script trace, to stdout */
            sscanf(line, "%*s %d", &a); e->trace = a;
        } else if (!strcmp(cmd, "quit")) {
            printf(".\n"); fflush(stdout); return 0;
        } else printf("? %s\n", cmd);
        #undef TICK
        printf(".\n"); fflush(stdout);
    }
    return 0;
}

/* Where the 800x600 canvas sits in the window's pixels: centred, as large as fits. */
static SDL_FRect canvas_rect(SDL_Renderer *ren)
{
    int ow, oh; SDL_GetRendererOutputSize(ren, &ow, &oh);
    float sc = (float)ow / ENG_W < (float)oh / ENG_H ? (float)ow / ENG_W : (float)oh / ENG_H;
    SDL_FRect r = { (ow - ENG_W * sc) / 2, (oh - ENG_H * sc) / 2, ENG_W * sc, ENG_H * sc };
    return r;
}

/* Window points (mouse events) to canvas pixels, and back (pointer warps).
 * Points differ from pixels on Retina displays. */
static void to_canvas(SDL_Window *win, SDL_Renderer *ren, int *x, int *y)
{
    int ww, wh, ow, oh; SDL_GetWindowSize(win, &ww, &wh); SDL_GetRendererOutputSize(ren, &ow, &oh);
    SDL_FRect r = canvas_rect(ren);
    float px = *x * (float)ow / ww, py = *y * (float)oh / wh;
    *x = (int)((px - r.x) * ENG_W / r.w); *y = (int)((py - r.y) * ENG_H / r.h);
}

static void from_canvas(SDL_Window *win, SDL_Renderer *ren, int *x, int *y)
{
    int ww, wh, ow, oh; SDL_GetWindowSize(win, &ww, &wh); SDL_GetRendererOutputSize(ren, &ow, &oh);
    SDL_FRect r = canvas_rect(ren);
    *x = (int)((r.x + *x * r.w / ENG_W) * ww / ow); *y = (int)((r.y + *y * r.h / ENG_H) * wh / oh);
}

/* The high-resolution art cache: $JUNGLE_HIRES, else hires/ beside the
 * executable, else build/hires in a checkout (tools/upscale.py writes it). */
static int find_hires(char *out, size_t cap)
{
    struct stat st;
    const char *env = getenv("JUNGLE_HIRES");
    if (env) { snprintf(out, cap, "%s", env); return stat(out, &st) == 0; }
    char *base = SDL_GetBasePath();
    if (base) { snprintf(out, cap, "%shires", base); SDL_free(base); if (stat(out, &st) == 0) return 1; }
    snprintf(out, cap, "build/hires");
    return stat(out, &st) == 0;
}

#ifdef __EMSCRIPTEN__
/* The page's on-screen buttons (phones have no keyboard): press or release a
 * virtual key, with the WM_CHAR a keyboard would add. */
EMSCRIPTEN_KEEPALIVE void web_key(int vk, int down)
{
    Engine *e = (Engine *)g_web_engine;
    if (!e) return;
    if (!down) { engine_keystate(e, vk, 0); return; }
    engine_key(e, vk);
    if (vk == 0x08 || vk == 0x0D || vk == 0x1B || vk == 0x20) engine_char(e, vk);
}
#endif

#define AUDIO_MAX_LAG (22050 / 10)            /* samples: at most 100 ms of sound waits to play */

int main(int argc, char **argv)
{
    /* No arguments: play. The disc's JUNGLE directory is looked for where
     * each platform keeps app data (the user copies it there from their disc). */
    static char def_bin[1024]; static char *def_argv[3];
    if (argc < 2) {
#if defined(__vita__)
        snprintf(def_bin, sizeof def_bin, "ux0:data/jungle/JUNGLE.BIN");
#else
        char *base = SDL_GetBasePath();
        snprintf(def_bin, sizeof def_bin, "%sJUNGLE/JUNGLE.BIN", base ? base : "./");
        if (base) SDL_free(base);
        FILE *probe = fopen(def_bin, "rb");
        if (!probe) snprintf(def_bin, sizeof def_bin, "orig/cd/JUNGLE/JUNGLE.BIN");
        else fclose(probe);
#endif
        def_argv[0] = argv[0]; def_argv[1] = def_bin; def_argv[2] = "--game";
        argv = def_argv; argc = 3;
    }
    Container c;
    if (!container_open(&c, argv[1])) return 1;

    adpcm_load_tables("orig/cd/JUNGLE/JUNGA01.DLL");

    if (argc > 2 && strcmp(argv[2], "--audio") == 0) {
        if (!g_step) { fprintf(stderr, "no ADPCM tables (run from repo root)\n"); return 1; }
        int n = 0; long frames = 0;
        for (int i = 0; i < c.ndir; i++) {
            if (c.dir[i].type != 7) continue;
            const u8 *r = resource_bytes(&c, &c.dir[i]);
            u32 ds = rd32(r);
            u16 rate = rd16(r + 0x10), ch = rd16(r + 0x0E), bits = rd16(r + 0x1A);
            if (ds + 28 > c.dir[i].size) continue;
            s16 *pcm = malloc((size_t)ds * 2 * sizeof(s16));
            size_t got = adpcm_decode(r + 28, ds, pcm);
            if (n == 0) printf("  format: %u Hz, %u ch, %u bit\n", rate, ch, bits);
            frames += (long)got; free(pcm); n++;
        }
        printf("  %d clips decoded, %ld frames, %.1f seconds\n", n, frames, frames / 22050.0);
        return 0;
    }

    if (argc > 2 && strcmp(argv[2], "--script") == 0) {
        /* Walk every type-14 record; run the expression bytecode carried by
         * opcodes 76/89. Length is a property of the opcode, so only the
         * fixed and field-stored forms are handled here — enough to exercise
         * the VM against real data. */
        VM vm; vm.mem = calloc(VM_MEM, sizeof(s16));
        int ran = 0, failed = 0, records = 0, complete = 0, partial = 0;
        int blockers_buf[256]; memset(blockers_buf, 0, sizeof blockers_buf);
        int *blockers = blockers_buf;
        int list_partial = (argc > 3 && strcmp(argv[3], "--list") == 0);
        for (int i = 0; i < c.ndir; i++) {
            if (c.dir[i].type != 14) continue;
            const u8 *b = resource_bytes(&c, &c.dir[i]);
            size_t n = c.dir[i].size;
            /* `visited` marks record STARTS; `seen` marks covered BYTES. They
             * must be separate: a branch target can legitimately point inside
             * the span of an already-walked record, and conflating the two
             * makes the walker refuse to follow it. */
            u8 *seen = calloc(n + 1, 1);
            u8 *visited = calloc(n + 1, 1);
            size_t *work = malloc(sizeof(size_t) * 4096); int nw = 0; work[nw++] = 0;
            int stopped = 0;
            while (nw > 0) {
                size_t p2 = work[--nw];
                while (p2 + 2 <= n && !visited[p2]) {
                    visited[p2] = 1; seen[p2] = 1;
                    int op = rd16(b + p2);
                    int tg[8], nt = 0;
                    int ln = record_len(b, p2, n, tg, &nt);
                    if (ln == 0) {                            /* terminator */
                        /* The terminator occupies its own opcode word. Not
                         * marking it leaves the resource one word short of
                         * fully covered, which reads as an incomplete walk. */
                        records++;
                        for (size_t k = p2; k < p2 + 2 && k < n; k++) seen[k] = 1;
                        break;
                    }
                    if (ln == -2) {                           /* jump: 4-byte record + edge */
                        for (int t = 0; t < nt; t++) {
                            long q = (long)p2 + tg[t];
                            if (q >= 0 && (size_t)q < n && !visited[q] && nw < 4096) work[nw++] = (size_t)q;
                        }
                        for (size_t k = p2; k < p2 + 4 && k < n; k++) seen[k] = 1;
                        records++;
                        break;
                    }
                    if (ln < 2) { stopped = 1; if (op < 256) blockers[op]++; break; }
                    /* A final record may declare a length that overruns the
                     * resource by a byte or two. The reference walker clamps
                     * and still counts it; rejecting it loses the last record
                     * and reports the resource as incomplete. */
                    size_t span = (size_t)ln;
                    if (p2 + span > n) {
                        if (p2 + 2 > n) { stopped = 1; break; }
                        span = n - p2;
                    }
                    records++;
                    if (op == 76 || op == 89) {
                        vm_run(&vm, b + p2 + 4, (size_t)ln - 4);
                        if (vm.err) failed++; else ran++;
                    }
                    for (size_t k = p2; k < p2 + span; k++) seen[k] = 1;
                    /* Only seed a target if a plausible opcode sits there. A bad
                     * target lands mid-record and poisons everything after it. */
                    for (int t = 0; t < nt; t++) {
                        long q = (long)p2 + tg[t];
                        if (q < 2 || (size_t)q + 2 > n || visited[q] || nw >= 4096) continue;
                        int nxt = rd16(b + q);
                        int dummy[8], nd;
                        if (record_len(b, (size_t)q, n, dummy, &nd) != -1 || nxt == 0 || nxt == 44)
                            work[nw++] = (size_t)q;
                    }
                    p2 += (size_t)ln;
                }
            }
            free(work); free(visited);
            size_t cov = 0; for (size_t k = 0; k < n; k++) cov += seen[k];
            if (!stopped && cov == n) complete++; else {
                partial++;
                if (list_partial) printf("  partial res %d  size %zu  covered %zu\n",
                                         i, n, cov);
            }
            free(seen);
        }
        int written = 0;
        for (int i = 0; i < VM_MEM; i++) if (vm.mem[i]) written++;
        printf("%-22s %6d records  %5d expr  %3d fail  %4d complete / %4d partial  %5d cells\n",
               strrchr(argv[1],'/') ? strrchr(argv[1],'/')+1 : argv[1],
               records, ran, failed, complete, partial, written);
        if (argc > 3 && strcmp(argv[3], "--blockers") == 0) {
            for (int i = 0; i < 256; i++)
                if (blockers[i]) printf("    blocked by opcode %-4d x%d\n", i, blockers[i]);
        }
        free(vm.mem);
        return 0;
    }

    if (argc > 2 && strcmp(argv[2], "--opcensus") == 0) {
        /* Which outer opcodes does a scene actually execute? Implementing all
         * ~94 handlers blind is the wrong order; this says which ones matter. */
        int freq[256]; memset(freq, 0, sizeof freq);
        long total = 0;
        for (int i = 0; i < c.ndir; i++) {
            if (c.dir[i].type != 14) continue;
            const u8 *b = resource_bytes(&c, &c.dir[i]);
            size_t n = c.dir[i].size;
            u8 *visited = calloc(n + 1, 1);
            size_t *work = malloc(sizeof(size_t) * 4096); int nw = 0; work[nw++] = 0;
            while (nw > 0) {
                size_t p2 = work[--nw];
                while (p2 + 2 <= n && !visited[p2]) {
                    visited[p2] = 1;
                    int op = rd16(b + p2), tg[8], nt = 0;
                    int ln = record_len(b, p2, n, tg, &nt);
                    if (op < 256) { freq[op]++; total++; }
                    if (ln == 0) break;
                    if (ln == -2) { for (int t=0;t<nt;t++){ long q=(long)p2+tg[t];
                        if (q>=0 && (size_t)q<n && !visited[q] && nw<4096) work[nw++]=(size_t)q; } break; }
                    if (ln < 2) break;
                    size_t span = (size_t)ln;
                    if (p2 + span > n) { if (p2 + 2 > n) break; span = n - p2; }
                    for (int t=0;t<nt;t++){ long q=(long)p2+tg[t];
                        if (q>=2 && (size_t)q+2<=n && !visited[q] && nw<4096) work[nw++]=(size_t)q; }
                    p2 += span;
                }
            }
            free(visited); free(work);
        }
        /* rank by frequency, print cumulative share */
        int order[256]; for (int i=0;i<256;i++) order[i]=i;
        for (int a=0;a<256;a++) for (int b2=a+1;b2<256;b2++)
            if (freq[order[b2]] > freq[order[a]]) { int t=order[a]; order[a]=order[b2]; order[b2]=t; }
        long cum = 0;
        printf("%-6s %8s %8s  %s\n", "opcode", "count", "cum%", "");
        for (int k = 0; k < 256 && freq[order[k]]; k++) {
            cum += freq[order[k]];
            printf("%-6d %8d %7.1f%%\n", order[k], freq[order[k]], 100.0*cum/total);
            if (100.0*cum/total > 95.0) break;
        }
        printf("total records %ld\n", total);
        return 0;
    }

    if (argc > 2 && strcmp(argv[2], "--vmtrace") == 0) {
        /* Emit one line per executed expression: resource, offset, result.
         * Directly diffable against the Python VM over the same input. */
        int trace_records = (argc > 3 && strcmp(argv[3], "--records") == 0);
        VM vm; vm.mem = calloc(VM_MEM, sizeof(s16));
        for (int i = 0; i < c.ndir; i++) {
            if (c.dir[i].type != 14) continue;
            /* Memory resets per resource. Sharing it across resources makes
             * results depend on directory order, which is exactly the kind of
             * difference that shows up as a phantom semantic disagreement. */
            memset(vm.mem, 0, VM_MEM * sizeof(s16));
            const u8 *b = resource_bytes(&c, &c.dir[i]);
            size_t n = c.dir[i].size;
            u8 *seen = calloc(n + 1, 1);
            u8 *visited = calloc(n + 1, 1);
            size_t *work = malloc(sizeof(size_t) * 4096); int nw = 0; work[nw++] = 0;
            while (nw > 0) {
                size_t p2 = work[--nw];
                while (p2 + 2 <= n && !visited[p2]) {
                    visited[p2] = 1; seen[p2] = 1;
                    int op = rd16(b + p2), tg[8], nt = 0;
                    int ln = record_len(b, p2, n, tg, &nt);
                    if (ln == 0) {
                        /* Terminators are real records; omitting them from the
                         * trace made the walker look like it had skipped an
                         * offset it had actually visited. */
                        if (trace_records) printf("R %d %zu %d 0\n", i, p2, op);
                        break;
                    }
                    if (ln == -2) {
                        if (trace_records) printf("R %d %zu %d 4\n", i, p2, op);
                        for (size_t k = p2; k < p2 + 4 && k < n; k++) seen[k] = 1;
                        for (int t=0;t<nt;t++){ long q=(long)p2+tg[t];
                        if (q>=0 && (size_t)q<n && !visited[q] && nw<4096) work[nw++]=(size_t)q; } break; }
                    if (ln < 2) break;
                    size_t span = (size_t)ln;
                    if (p2 + span > n) { if (p2 + 2 > n) break; span = n - p2; }
                    if (trace_records) printf("R %d %zu %d %zu\n", i, p2, op, span);
                    if ((op == 76 || op == 89) && span > 4) {
                        s16 r = vm_run(&vm, b + p2 + 4, span - 4);
                        if (!trace_records) printf("%d %zu %d %d\n", i, p2, (int)r, vm.err);
                    }
                    for (size_t k = p2; k < p2 + span; k++) seen[k] = 1;
                    for (int t=0;t<nt;t++){ long q=(long)p2+tg[t];
                        if (q>=2 && (size_t)q+2<=n && !visited[q] && nw<4096) work[nw++]=(size_t)q; }
                    p2 += span;
                }
            }
            free(seen); free(work); free(visited);
        }
        free(vm.mem);
        return 0;
    }

    if (argc > 2 && strcmp(argv[2], "--info") == 0) {
        u16 hdr = rd16(c.data + 2);
        printf("header      %u bytes, version %u\n", hdr, rd16(c.data + OFF_VERSION));
        printf("strings     %u declared\n", rd16(c.data + 0xA8));
        printf("variables   %u declared\n", rd16(c.data + 0xAA));
        printf("blob        %u bytes in %u segment(s)\n", c.blob_len, rd16(c.data + 0xA0));
        printf("tables:\n");
        /* Slot 6 is the palette: OFF_PALETTE 0xEE == OFF_TABLES 0xBE + 6*8. */
        static const char *tname[8] = { "directory", "entries", "(unused)",
                                        "variables", "strings", "script buffer",
                                        "palette", "(unused)" };
        for (int i = 0; i < 8; i++) {
            u32 off = rd32(c.data + OFF_TABLES + i * 8);
            u32 len = rd32(c.data + OFF_TABLES + i * 8 + 4);
            if (len) printf("  %-14s offset %9u  length %8u\n", tname[i], off, len);
        }
        int counts[32]; memset(counts, 0, sizeof counts);
        for (int i = 0; i < c.ndir; i++)
            if (c.dir[i].type < 32) counts[c.dir[i].type]++;
        printf("resources   %d total\n", c.ndir);
        for (int t = 0; t < 32; t++) if (counts[t]) {
            const char *what = t == 1 ? "bitmap" : t == 7 ? "audio" :
                               t == 14 ? "scene script" :
                               (t >= 8 && t <= 16) ? "resident" : "";
            printf("  type %-3d %6d  %s\n", t, counts[t], what);
        }
        u32 soff = rd32(c.data + OFF_TABLES + 4 * 8);
        u32 slen = rd32(c.data + OFF_TABLES + 4 * 8 + 4);
        if (slen) {
            printf("constant strings:\n");
            u32 i = soff; int shown = 0;
            while (i < soff + slen && shown < 12) {
                const char *str = (const char *)(c.data + i);
                size_t l = strnlen(str, soff + slen - i);
                if (l) { printf("  \"%.*s\"\n", (int)l, str); shown++; }
                i += (u32)l + 1;
            }
        }
        return 0;
    }

    if (argc > 2 && strcmp(argv[2], "--dumpwav") == 0) {
        const char *outdir = argc > 3 ? argv[3] : ".";
        if (!g_step) { fprintf(stderr, "no ADPCM tables (run from repo root)\n"); return 1; }
        int n = 0;
        for (int i = 0; i < c.ndir; i++) {
            if (c.dir[i].type != 7) continue;
            const u8 *r = resource_bytes(&c, &c.dir[i]);
            u32 ds = rd32(r);
            u16 id = rd16(r + 0x0A), ch = rd16(r + 0x0E), bits = rd16(r + 0x1A);
            u32 rate = rd32(r + 0x10);
            if (ds + 28 > c.dir[i].size) continue;
            s16 *pcm = malloc((size_t)ds * 2 * sizeof(s16));
            size_t got = adpcm_decode(r + 28, ds, pcm);
            u32 bytes = (u32)(got * sizeof(s16));
            u32 align = ch * bits / 8, body = 4 + 8 + 16 + 8 + bytes;
            char path[512];
            snprintf(path, sizeof path, "%s/%05d_id%d.wav", outdir, i, id);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite("RIFF", 1, 4, f); fwrite(&body, 4, 1, f); fwrite("WAVE", 1, 4, f);
                u32 fl = 16; u16 tag = 1; u32 avg = rate * align;
                fwrite("fmt ", 1, 4, f); fwrite(&fl, 4, 1, f);
                fwrite(&tag, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
                fwrite(&avg, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
                fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
                fwrite(pcm, 1, bytes, f); fclose(f); n++;
            }
            free(pcm);
        }
        printf("wrote %d WAVs to %s\n", n, outdir);
        return 0;
    }

    /* --export DIR: every bitmap as DIR/NNNNN.png, index 0 transparent, for tools/upscale.sh */
    if (argc > 2 && strcmp(argv[2], "--export") == 0) {
        const char *outdir = argc > 3 ? argv[3] : ".";
        int n = 0; long px_total = 0; g_png_trns = 1;
        for (int i = 0; i < c.ndir; i++) {
            if (c.dir[i].type != 1) continue;
            int w, h; u8 *px;
            if (!bitmap_decode(resource_bytes(&c, &c.dir[i]), c.dir[i].size, &w, &h, &px)) continue;
            char path[512];
            snprintf(path, sizeof path, "%s/%05d.png", outdir, i);
            if (png_write(path, w, h, px, c.pal)) { n++; px_total += (long)w * h; }
            free(px);
        }
        printf("exported %d bitmaps (%ld pixels) to %s\n", n, px_total, outdir);
        return 0;
    }
    if (argc > 2 && strcmp(argv[2], "--dump") == 0) {
        const char *outdir = argc > 3 ? argv[3] : ".";
        int n = 0;
        for (int i = 0; i < c.ndir; i++) {
            if (c.dir[i].type != 1) continue;
            int w, h; u8 *px;
            if (!bitmap_decode(resource_bytes(&c, &c.dir[i]), c.dir[i].size, &w, &h, &px)) continue;
            char path[512];
            snprintf(path, sizeof path, "%s/%05d_%dx%d.png", outdir, i, w, h);
            if (png_write(path, w, h, px, c.pal)) n++;
            free(px);
        }
        printf("wrote %d PNGs to %s\n", n, outdir);
        return 0;
    }

    /* --scene [OUT.png]  render what --game would draw, headless.
     *
     * Same scene composition as the interactive loop, written to a PNG so the
     * result can be inspected without a display and without a person.
     */
    /* --engine OUT.png [MS] [KEYS]  run the reimplemented runtime headless.
     *
     * Loads this container the way the game does (resource 0, then the
     * deferred script), simulates MS milliseconds in 16 ms ticks, and writes
     * the frame. KEYS is a list of virtual keys in hex, pressed at even
     * intervals, so navigation can be exercised without a person. */
    if (argc > 2 && strcmp(argv[2], "--drive") == 0) {
        char dir[512]; snprintf(dir, sizeof dir, "%s", argv[1]);
        char *slash = strrchr(dir, '/'); const char *base = argv[1];
        if (slash) { *slash = 0; base = slash + 1; } else snprintf(dir, sizeof dir, ".");
        Engine *e = calloc(1, sizeof *e);
        if (!engine_init(e, dir)) return 1;
        if (getenv("ENGINE_INI")) engine_set_ini(e, getenv("ENGINE_INI"));
        if (getenv("ENGINE_MUSIC")) e->synth.master = (float)atof(getenv("ENGINE_MUSIC"));
        if (!engine_load(e, base)) return 1;
        FILE *wav = getenv("ENGINE_WAV") ? fopen(getenv("ENGINE_WAV"), "wb") : NULL;
        u32 wav_n = 0;
        if (wav) { u8 hdr[44] = {0}; fwrite(hdr, 1, 44, wav); }
        drive_mode(e, wav, &wav_n);
        if (wav) {
            u8 h[44]; u32 db = wav_n * 2, rate = 22050, v; u16 w;
            memcpy(h, "RIFF", 4); v = 36 + db; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
            v = 16; memcpy(h + 16, &v, 4); w = 1; memcpy(h + 20, &w, 2); memcpy(h + 22, &w, 2);
            memcpy(h + 24, &rate, 4); v = rate * 2; memcpy(h + 28, &v, 4); w = 2; memcpy(h + 32, &w, 2);
            w = 16; memcpy(h + 34, &w, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &db, 4);
            fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav); fclose(wav);
        }
        return 0;
    }
    if (argc > 2 && strcmp(argv[2], "--engine") == 0) {
        const char *out = argc > 3 ? argv[3] : "engine.png";
        int ms = argc > 4 ? atoi(argv[4]) : 3000;
        const char *keys = argc > 5 ? argv[5] : "";
        char dir[512]; snprintf(dir, sizeof dir, "%s", argv[1]);
        char *slash = strrchr(dir, '/'); const char *base = argv[1];
        if (slash) { *slash = 0; base = slash + 1; } else snprintf(dir, sizeof dir, ".");
        Engine *e = calloc(1, sizeof *e);
        if (!engine_init(e, dir)) return 1;
        e->trace = getenv("ENGINE_TRACE") != NULL;
        if (getenv("ENGINE_INI")) engine_set_ini(e, getenv("ENGINE_INI"));
        if (!engine_load(e, base)) return 1;
        /* events: hex virtual keys, or @x:y for a left click, comma separated */
        enum { MAXEV = 4096 }; static int kv[MAXEV], kx[MAXEV], ky[MAXEV], kt[MAXEV], kh[MAXEV]; int nkeys = 0; const char *q = keys;
        while (*q && nkeys < MAXEV) {
            kt[nkeys] = -1;                          /* TIME=EVENT pins an event to a time */
            const char *eq = strchr(q, '='), *cm = strchr(q, ',');
            if (eq && (!cm || eq < cm)) { kt[nkeys] = atoi(q); q = eq + 1; }
            if (*q == '@') {
                kx[nkeys] = (int)strtol(q + 1, (char **)&q, 10); ky[nkeys] = (int)strtol(q + 1, (char **)&q, 10);
                kv[nkeys] = -1;
                if (*q == ':' && q[1] == 'r') { kv[nkeys] = -2; q += 2; }
                nkeys++;
            }
            else {
                int ctrl = *q == '^'; if (ctrl) q++;  /* ^KEY: with Ctrl held */
                kh[nkeys] = 150;                     /* KEY~MS holds the key for MS */
                kv[nkeys] = (int)strtol(q, (char **)&q, 16);
                if (*q == '~') kh[nkeys] = (int)strtol(q + 1, (char **)&q, 10);
                if (ctrl) kv[nkeys] |= 0x1000;
                nkeys++;
            }
            while (*q == ',') q++;
        }
        FILE *wav = getenv("ENGINE_WAV") ? fopen(getenv("ENGINE_WAV"), "wb") : NULL;
        if (getenv("ENGINE_MUSIC")) e->synth.master = (float)atof(getenv("ENGINE_MUSIC"));   /* music level, 0 mutes */
        u32 wav_n = 0;
        if (wav) { u8 hdr[44] = {0}; fwrite(hdr, 1, 44, wav); }
        int ki = 0, release_vk = -1, release_t = 0;
        for (int t = 0; t <= ms; t += 16) {
            if (release_vk >= 0 && t >= release_t) { engine_keystate(e, release_vk, 0); release_vk = -1; }
            if (ki < nkeys && t >= (kt[ki] >= 0 ? kt[ki] : (ki + 1) * ms / (nkeys + 1))) {
                if (kv[ki] < 0) {
                    int btn = kv[ki] == -2 ? 2 : 1;
                    printf("t=%d click%s %d,%d in %s\n", t, btn == 2 ? " (right)" : "", kx[ki], ky[ki], e->name);
                    engine_mouse(e, kx[ki], ky[ki], 0, 0);            /* move there first */
                    engine_tick(e, (u32)t);
                    engine_mouse(e, kx[ki], ky[ki], btn, 1);
                    for (int k = 0; k < 6; k++) engine_tick(e, (u32)(t + k * 16));   /* ~100 ms held */
                    engine_mouse(e, kx[ki], ky[ki], btn, 0);
                } else {
                    printf("t=%d key 0x%x in %s\n", t, kv[ki], e->name);
                    if (release_vk >= 0) engine_keystate(e, release_vk, 0);
                    if (kv[ki] & 0x1000) engine_keystate(e, 0x11, 1);
                    engine_key(e, kv[ki] & 0xFFF);
                    {                                /* the WM_CHAR TranslateMessage would add */
                        int v = kv[ki] & 0xFFF, ch = 0;
                        if (v >= 0x41 && v <= 0x5A) ch = e->keydown[0x10] ? v : v + 0x20;
                        else if ((v >= 0x30 && v <= 0x39) || v == 0x20 || v == 0x08 || v == 0x0D || v == 0x1B) ch = v;
                        if (ch && !(kv[ki] & 0x1000)) engine_char(e, ch);
                    }
                    if (kv[ki] & 0x1000) engine_keystate(e, 0x11, 0);
                    kv[ki] &= 0xFFF;
                    release_vk = kv[ki]; release_t = t + kh[ki];
                }
                ki++;
            }
            engine_tick(e, (u32)t);
            if (wav) { s16 buf[4096]; u32 got; while ((got = engine_audio(e, buf, 4096)) > 0) { fwrite(buf, 2, got, wav); wav_n += got; } }
        }
        if (wav) {                                           /* 22050 Hz mono 16-bit PCM */
            u8 h[44]; u32 db = wav_n * 2, rate = 22050;
            memcpy(h, "RIFF", 4); u32 v = 36 + db; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
            v = 16; memcpy(h + 16, &v, 4); u16 w = 1; memcpy(h + 20, &w, 2); memcpy(h + 22, &w, 2);
            memcpy(h + 24, &rate, 4); v = rate * 2; memcpy(h + 28, &v, 4); w = 2; memcpy(h + 32, &w, 2);
            w = 16; memcpy(h + 34, &w, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &db, 4);
            fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav); fclose(wav);
            printf("audio: %u samples (%.1f s) -> %s\n", wav_n, wav_n / 22050.0, getenv("ENGINE_WAV"));
        }
        u8 *fb = malloc(ENG_W * ENG_H);
        engine_render(e, fb);
        if (getenv("ENGINE_DRAWLIST")) {                /* the draw list paints the same frame */
            static EngDraw dl[2048]; int nd = engine_drawlist(e, dl, 2048), diff = 0;
            u8 *fb2 = malloc(ENG_W * ENG_H);
            for (int k = 0; k < nd; k++) {
                if (dl[k].kind == 0) memset(fb2, dl[k].fill, ENG_W * ENG_H);
                else if (dl[k].kind == 3) memcpy(fb2, fb, ENG_W * ENG_H);
                else blit8(fb2, ENG_W, ENG_W, ENG_H, dl[k].x, dl[k].y, dl[k].px, dl[k].w, dl[k].w, dl[k].h, dl[k].flip ? BLIT_FLIP_X : 0);
            }
            for (int k = 0; k < ENG_W * ENG_H; k++) diff += fb[k] != fb2[k];
            printf("drawlist: %d items, %d pixels differ\n", nd, diff);
            free(fb2);
        }
        if (getenv("ENGINE_HIRES_SHOT")) {              /* the high-resolution frame, drawn by hires.c into memory */
            char hdir[512]; find_hires(hdir, sizeof hdir);   /* an explicit JUNGLE_HIRES is used as given */
            int sc = getenv("ENGINE_HIRES_SCALE") ? atoi(getenv("ENGINE_HIRES_SCALE")) : 3;
            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ENG_W * sc, ENG_H * sc, 32, SDL_PIXELFORMAT_ARGB8888);
            SDL_Renderer *sr = surf ? SDL_CreateSoftwareRenderer(surf) : NULL;
            HiRes *hh = sr ? hires_open(sr, hdir, getenv("ENGINE_HIRES_LEVEL") ? atoi(getenv("ENGINE_HIRES_LEVEL")) : 3) : NULL;
            SDL_FRect r = { 0, 0, (float)ENG_W * sc, (float)ENG_H * sc };
            if (hh && hires_draw(hh, e, r)) {
                SDL_RenderPresent(sr);
                int art, fb_n; hires_stats(hh, &art, &fb_n);
                int ok2 = SDL_SaveBMP(surf, getenv("ENGINE_HIRES_SHOT")) == 0;
                printf("hires: %d bitmaps from the art cache, %d from the original pixels -> %s%s\n", art, fb_n, getenv("ENGINE_HIRES_SHOT"), ok2 ? "" : " FAILED");
            } else printf("hires: not drawn (%s)\n", SDL_GetError());
            hires_close(hh); if (sr) SDL_DestroyRenderer(sr); if (surf) SDL_FreeSurface(surf);
        }
        u8 *up = malloc(ENG_W * ENG_H);                 /* png_write expects bottom-up rows */
        for (int y = 0; y < ENG_H; y++) memcpy(up + (size_t)y * ENG_W, fb + (size_t)(ENG_H - 1 - y) * ENG_W, ENG_W);
        u8 dpal[256][3]; engine_palette(e, dpal);
        int ok = png_write(out, ENG_W, ENG_H, up, dpal);
        free(up);
        int ns = 0, nv = 0, nr = 0;
        for (int i = 0; i < ENG_MAX_SPRITES; i++) if (e->spr[i].used) { ns++; nv += e->spr[i].visible; nr += e->spr[i].running; }
        printf("%s: bg %d, %d sprites (%d visible, %d running), %d key bindings -> %s\n",
               e->name, e->bg, ns, nv, nr, e->nbind, ok ? out : "FAILED");
        if (getenv("ENGINE_SPRITES"))
            for (int i = 0; i < ENG_MAX_SPRITES; i++) {
                Sprite *sp = &e->spr[i];
                if (!sp->used) continue;
                int ox = 0, oy = 0, bi = sp->ncel ? frame_resolve(&e->c, (sp->ncomp ? sp->cel[sp->comp[0]] : sp->cel[sp->cur]) + (0x10000 - 0x7531), &ox, &oy) : -1;
                int bw = bi >= 0 ? rd16(resource_bytes(&e->c, &e->c.dir[bi]) + 2) : 0, bh = bi >= 0 ? rd16(resource_bytes(&e->c, &e->c.dir[bi]) + 6) : 0;
                printf("  spr res %4d brk %d z %6d at %4d,%4d vis %d run %d pc %4d/%4d cels %2d cur %2d ncomp %d bmp %4d %3dx%3d off %d,%d\n",
                       sp->res, sp->brk, sp->z, sp->x, sp->y, sp->visible, sp->running, sp->pc, sp->plen, sp->ncel, sp->cur, sp->ncomp, bi, bw, bh, ox, oy);
            }
        if (getenv("ENGINE_STATE")) {
            for (int i = 0; i < e->ntm; i++) printf("  timer id %d script %d every %u repeat %d\n", e->tm[i].id, (u16)(e->tm[i].script + 0x7531), e->tm[i].every, e->tm[i].repeat);
            for (int v = 0; v < 256; v++) if (e->keys[v].down || e->keys[v].up || e->keys[v].shift || e->keys[v].ctrl || e->keys[v].off)
                printf("  key 0x%02x down %d up %d shift %d ctrl %d off %d\n", v, e->keys[v].down ? (u16)(e->keys[v].down + 0x7531) : 0,
                       e->keys[v].up ? (u16)(e->keys[v].up + 0x7531) : 0, e->keys[v].shift ? (u16)(e->keys[v].shift + 0x7531) : 0,
                       e->keys[v].ctrl ? (u16)(e->keys[v].ctrl + 0x7531) : 0, e->keys[v].off);
            for (int i = 0; i < e->nhot; i++) printf("  hotspot %d,%d-%d,%d script %d off %d\n", e->hot[i].l, e->hot[i].t, e->hot[i].r, e->hot[i].b, (u16)(e->hot[i].script + 0x7531), e->hot[i].off);
            for (int v = 0; v < 256; v++) if (e->keymap[v].code) printf("  keymap vk 0x%02x kbd %d armed %d code 0x%02x\n", v, e->keymap[v].kbd, e->keymap[v].armed, e->keymap[v].code);
            for (int i = 0; i < 4; i++) printf("  player %d script %d code %d buttons %d\n", i, e->player[i].script ? (u16)(e->player[i].script + 0x7531) : 0, e->player[i].code, e->player[i].buttons);
            printf("  devplayer %d %d %d %d %d %d\n", e->devplayer[0], e->devplayer[1], e->devplayer[2], e->devplayer[3], e->devplayer[4], e->devplayer[5]);
            for (int i = 0; i < e->nreg; i++) printf("  region %d,%d-%d,%d enter %d leave %d\n", e->reg[i].l, e->reg[i].t, e->reg[i].r, e->reg[i].b, (u16)(e->reg[i].enter + 0x7531), e->reg[i].leave ? (u16)(e->reg[i].leave + 0x7531) : 0);
            printf("  hover script %d, in region %d\n", e->hover_script ? (u16)(e->hover_script + 0x7531) : 0, e->inreg);
            printf("  mouse filter %d, queue %d\n", e->mouse_script ? (u16)(e->mouse_script + 0x7531) : 0, e->nq);
        }
        if (getenv("ENGINE_GLOBALS")) {                  /* e.g. ENGINE_GLOBALS=5030-5045 */
            int lo = atoi(getenv("ENGINE_GLOBALS")), hi = lo;
            const char *dash = strchr(getenv("ENGINE_GLOBALS"), '-'); if (dash) hi = atoi(dash + 1);
            for (int g = lo; g <= hi; g++) printf("  g%d = %d\n", g, (s16)e->mem[(0x151E + g * 2) >> 1]);
        }
        printf("unimplemented records:");
        for (int i = 0; i < 256; i++) if (e->unimpl_rec[i]) printf(" %d x%u", i, e->unimpl_rec[i]);
        printf("\nbuiltin calls:");
        for (int i = 0; i < 256; i++) if (e->unimpl_builtin[i]) printf(" %d x%u", i, e->unimpl_builtin[i]);
        printf("\n");
        return ok ? 0 : 1;
    }

    if (argc > 2 && strcmp(argv[2], "--scene") == 0) {
        const int W = 800, H = 600;   /* engine canvas; StretchDIBits shrinks it on small screens */
        const char *out = argc > 3 ? argv[3] : "scene.png";
        SceneOut so; int i;
        memset(&so, 0, sizeof so);
        for (i = 0; i < c.ndir; i++)
            if (c.dir[i].type == 14 && c.dir[i].size <= 4096)
                scene_exec(&c, i, &so, 0);

        int bg = -1;
        for (i = 0; i < c.ndir; i++)
            if (c.dir[i].type == 1 && (bg < 0 || c.dir[i].size > c.dir[bg].size)) bg = i;

        u8 *fb = calloc((size_t)W * H, 1);
        int drawn = 0;
        if (bg >= 0) {
            int w, h; u8 *px = NULL;
            if (bitmap_decode(resource_bytes(&c, &c.dir[bg]), c.dir[bg].size, &w, &h, &px)) {
                blit8(fb, W, W, H, (W - w) / 2, (H - h) / 2, px, w, w, h, 0);
                free(px); drawn++;
                printf("background: res %d  %dx%d\n", bg, w, h);
            }
        }
        for (i = 0; i < so.nact; i++) {
            int fx, fy, ri = so.act[i];
            if (ri < 0 || ri >= c.ndir || c.dir[ri].type != 15) continue;
            const u8 *rec = resource_bytes(&c, &c.dir[ri]);
            if (c.dir[ri].size < 0x16) continue;
            int cnt = rd16(rec + 4);
            if (cnt < 1 || 0x14 + cnt * 2 > (int)c.dir[ri].size) continue;
            int idx2 = frame_resolve(&c, rd16(rec + 0x14), &fx, &fy);
            if (idx2 < 0) continue;
            int w, h; u8 *px = NULL;
            if (bitmap_decode(resource_bytes(&c, &c.dir[idx2]), c.dir[idx2].size, &w, &h, &px)) {
                blit8(fb, W, W, H, fx, fy, px, w, w, h, 0);
                free(px); drawn++;
            }
        }
        int ok = png_write(out, W, H, fb, c.pal);
        printf("%s: %d bindings, %d activated, %d objects drawn -> %s (%s)\n",
               argv[1], so.n, so.nact, drawn, out, ok ? "written" : "FAILED");
        free(fb);
        return ok ? 0 : 1;
    }

    /* --game  scenes, input and transitions, wired together.
     *
     * Opens a container, runs its scripts to collect key bindings, draws the
     * scene, and on a bound key runs that key's script. If the script asks for
     * a scene transition the container is swapped and the process repeats, so
     * navigation is the game's own, not a menu written here.
     */
    if (argc > 2 && strcmp(argv[2], "--game") == 0) {
        /* The game, played: the engine drives everything, SDL only shows the
         * 800x600 canvas and passes the mouse and keyboard through. */
        char dir[512]; snprintf(dir, sizeof dir, "%s", argv[1]);
        char *slash = strrchr(dir, '/'); const char *base = argv[1];
        if (slash) { *slash = 0; base = slash + 1; } else snprintf(dir, sizeof dir, ".");
        Engine *e = calloc(1, sizeof *e);
        if (!engine_init(e, dir)) return 1;
        g_web_engine = e;
        e->trace = getenv("ENGINE_TRACE") != NULL;
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
        for (int j = 0; j < SDL_NumJoysticks(); j++) if (SDL_IsGameController(j)) SDL_GameControllerOpen(j);
        SDL_AudioSpec want; SDL_memset(&want, 0, sizeof want);
        want.freq = 22050; want.format = AUDIO_S16LSB; want.channels = 1; want.samples = 512;   /* 23 ms */
        const char *mute = getenv("JUNGLE_MUTE");
        SDL_AudioDeviceID adev = (mute && *mute && strcmp(mute, "0")) ? 0 : SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);   /* JUNGLE_MUTE=1: silent */
        if (adev) SDL_PauseAudioDevice(adev, 0);
#if defined(__IPHONEOS__) || defined(__ANDROID__) || defined(__vita__)
        Uint32 wflags = SDL_WINDOW_FULLSCREEN;       /* handhelds: the whole screen, letterboxed, landscape */
#else
        Uint32 wflags = SDL_WINDOW_RESIZABLE;
#endif
        char hdir[512]; int have_hires = find_hires(hdir, sizeof hdir) && !getenv("JUNGLE_CLASSIC");
        int win_w = have_hires ? ENG_W * 3 / 2 : ENG_W, win_h = have_hires ? ENG_H * 3 / 2 : ENG_H;   /* 3x pixels on Retina */
        SDL_Window *win = SDL_CreateWindow("Timon & Pumbaa's Jungle Games",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, wflags | SDL_WINDOW_ALLOW_HIGHDPI);
        SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        /* Art levels: 0 the original pixels, 1-4 the upscaled art at that scale.
         * F9 cycles through the ones present; JUNGLE_ART picks the first. */
        int levels[5] = { 0 }, nlev = 1;
        for (int l = 1; l <= 4; l++) if (have_hires && hires_has_level(hdir, l)) levels[nlev++] = l;
        int art = levels[nlev - 1];
        if (getenv("JUNGLE_ART")) { int want = strcmp(getenv("JUNGLE_ART"), "original") ? atoi(getenv("JUNGLE_ART")) : 0;
            for (int k = 0; k < nlev; k++) if (levels[k] == want) art = want; }
        HiRes *hr = art ? hires_open(ren, hdir, art) : NULL;
        int use_hires = hr != NULL;
        SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, ENG_W, ENG_H);
        u8 *fb = calloc(ENG_W * ENG_H, 1);
        u32 *rgba = malloc(ENG_W * ENG_H * 4);
        char *pref = SDL_GetPrefPath("7th Level", "Jungle Games");   /* settings and high scores */
        if (pref) { char ini[600]; snprintf(ini, sizeof ini, "%s7THLEVEL.INI", pref); engine_set_ini(e, ini); SDL_free(pref); }
        u32 t0 = SDL_GetTicks();
        engine_tick(e, 0);
        if (!engine_load(e, base)) return 1;
        int running = 1;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) running = 0;
                else if (ev.type == SDL_CONTROLLERDEVICEADDED) SDL_GameControllerOpen(ev.cdevice.which);
                else if (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP) {
                    /* Pads map onto the keys the games already use: arrows, X and Z
                     * for Hippo Hop and Burper, Z and / for Pinball's flippers. */
                    int vk = 0;
                    switch (ev.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_DPAD_UP: vk = 0x26; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: vk = 0x28; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: vk = 0x25; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: vk = 0x27; break;
                    case SDL_CONTROLLER_BUTTON_A: vk = 'X'; break;
                    case SDL_CONTROLLER_BUTTON_B: vk = 'Z'; break;
                    case SDL_CONTROLLER_BUTTON_X: vk = 0x20; break;
                    case SDL_CONTROLLER_BUTTON_Y: case SDL_CONTROLLER_BUTTON_START: vk = 0x0D; break;
                    case SDL_CONTROLLER_BUTTON_BACK: vk = 0x1B; break;
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: vk = 'Z'; break;
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: vk = 0xBF; break;
                    }
                    if (vk) { if (ev.type == SDL_CONTROLLERBUTTONDOWN) engine_key(e, vk); else engine_keystate(e, vk, 0); }
                }
                else if (ev.type == SDL_MOUSEMOTION) { int mx = ev.motion.x, my = ev.motion.y; to_canvas(win, ren, &mx, &my); engine_mouse(e, mx, my, 0, 0); }
                else if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
                    int b = ev.button.button == SDL_BUTTON_RIGHT ? 2 : 1;
                    int mx = ev.button.x, my = ev.button.y; to_canvas(win, ren, &mx, &my);
                    engine_mouse(e, mx, my, b, ev.type == SDL_MOUSEBUTTONDOWN);
                } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                    SDL_Keycode k = ev.key.keysym.sym;
                    int vk = 0;                      /* SDL keycode -> Windows virtual key */
                    if (k == SDLK_ESCAPE) vk = 0x1B; else if (k == SDLK_SPACE) vk = 0x20;
                    else if (k == SDLK_BACKSPACE) vk = 0x08;
                    /* iPad keyboards have no Esc: ` (where Esc sits) and Cmd+. stand in for it */
                    else if (k == SDLK_BACKQUOTE) vk = 0x1B;
                    else if (k == SDLK_PERIOD && (ev.key.keysym.mod & KMOD_GUI)) vk = 0x1B;
                    else if (k == SDLK_RETURN) vk = 0x0D;
                    else if (k == SDLK_LEFT) vk = 0x25; else if (k == SDLK_UP) vk = 0x26;
                    else if (k == SDLK_RIGHT) vk = 0x27; else if (k == SDLK_DOWN) vk = 0x28;
                    else if (k == SDLK_KP_PLUS) vk = 0x6B; else if (k == SDLK_KP_MINUS) vk = 0x6D;
                    else if (k == SDLK_KP_MULTIPLY) vk = 0x6A;
                    else if (k == SDLK_SLASH) vk = 0xBF;
                    else if (k == SDLK_LCTRL || k == SDLK_RCTRL) vk = 0x11;
                    else if (k == SDLK_LSHIFT || k == SDLK_RSHIFT) vk = 0x10;
                    else if (k >= SDLK_F1 && k <= SDLK_F12) vk = 0x70 + (k - SDLK_F1);
                    else if (k >= SDLK_a && k <= SDLK_z) vk = 0x41 + (k - SDLK_a);
                    else if (k >= SDLK_0 && k <= SDLK_9) vk = 0x30 + (k - SDLK_0);
                    if (k == SDLK_F9 && ev.type == SDL_KEYDOWN) {    /* next art level */
                        int k2 = 0; while (k2 < nlev && levels[k2] != art) k2++;
                        art = levels[(k2 + 1) % nlev];
                        hires_close(hr); hr = art ? hires_open(ren, hdir, art) : NULL;
                        use_hires = hr != NULL;
                        continue;
                    }
                    if (!vk) continue;
                    if (ev.type == SDL_KEYUP) engine_keystate(e, vk, 0);
                    else {
                        if (!ev.key.repeat) engine_key(e, vk);
                        if (vk == 0x08 || vk == 0x0D || vk == 0x1B) engine_char(e, vk);   /* WM_CHAR for control keys */
                    }
                } else if (ev.type == SDL_TEXTINPUT) {  /* typed characters: high-score names */
                    for (const char *c = ev.text.text; *c; c++) if ((unsigned char)*c < 0x80) engine_char(e, *c);
                }
            }
            engine_tick(e, SDL_GetTicks() - t0);
            if (e->warp.host) {                      /* op 73: put the real pointer where the game moved it */
                e->warp.host = 0;
                int wx = e->warp.x, wy = e->warp.y; from_canvas(win, ren, &wx, &wy);
                SDL_WarpMouseInWindow(win, wx, wy);
            }
#if defined(__IPHONEOS__) || defined(__ANDROID__) || defined(__vita__)
            if (e->edit.on != (SDL_IsTextInputActive() == SDL_TRUE))   /* the on-screen keyboard follows text entry */
                { if (e->edit.on) SDL_StartTextInput(); else SDL_StopTextInput(); }
#endif
            {   /* Keep the device queue short so sound stays with the picture: after a
                 * stall (a scene load) the engine's backlog is dropped, not queued. */
                s16 buf[4096]; u32 got;
                while ((got = engine_audio(e, buf, 4096)) > 0) {
                    if (!adev) continue;
                    u32 queued = SDL_GetQueuedAudioSize(adev) / 2;
                    if (queued > AUDIO_MAX_LAG) continue;
                    SDL_QueueAudio(adev, buf, got * 2);
                }
            }
            SDL_FRect dst = canvas_rect(ren);
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            if (!(use_hires && hires_draw(hr, e, dst))) {
                engine_render(e, fb);
                u8 dpal[256][3]; engine_palette(e, dpal);
                for (int i = 0; i < ENG_W * ENG_H; i++) {
                    const u8 *c = dpal[fb[i]];
                    rgba[i] = 0xFF000000u | ((u32)c[0] << 16) | ((u32)c[1] << 8) | c[2];
                }
                SDL_UpdateTexture(tex, NULL, rgba, ENG_W * 4);
                SDL_RenderCopyF(ren, tex, NULL, &dst);
            }
            char title[128];
            if (use_hires) snprintf(title, sizeof title, "Jungle Games - %s - upscaled %dx (F9)", e->name, art);
            else snprintf(title, sizeof title, "Jungle Games - %s - original art%s", e->name, nlev > 1 ? " (F9)" : "");
            SDL_SetWindowTitle(win, title);
            SDL_RenderPresent(ren);
#ifdef __EMSCRIPTEN__
            {   /* the browser owns the frame: give it back until the next ~60 Hz slot (ASYNCIFY) */
                static u32 last; u32 now = SDL_GetTicks(), spent = now - last;
                emscripten_sleep(spent < 16 ? 16 - spent : 0);
                last = SDL_GetTicks();
            }
#endif
        }
        hires_close(hr);
        SDL_Quit();
        return 0;
    }

    /* --run RES [--trace]  execute a scene script.
     *
     * The walker enumerates records; this follows them. Control flow is real:
     * op 37 jumps, op 77/79 branch on an expression, op 78 switches. Statements
     * (op 76, 52.8% of all execution on the disc) are expression bytecode that
     * vm_run already evaluates against a variable memory.
     *
     * Opcodes with no implementation yet are counted and reported rather than
     * silently skipped, so the output is an honest map of what a scene needs.
     */
    if (argc > 2 && strcmp(argv[2], "--run") == 0) {
        int want = argc > 3 ? atoi(argv[3]) : -1;
        int trace = (argc > 4 && strcmp(argv[4], "--trace") == 0);
        int i;

        if (want < 0) {                 /* default: the largest type 14 script */
            int best = -1;
            for (i = 0; i < c.ndir; i++)
                if (c.dir[i].type == 14 && (best < 0 || c.dir[i].size > c.dir[best].size))
                    best = i;
            want = best;
        }
        if (want < 0 || want >= c.ndir || c.dir[want].type != 14) {
            fprintf(stderr, "resource %d is not a type 14 script\n", want); return 1;
        }

        const u8 *b = resource_bytes(&c, &c.dir[want]);
        size_t n = c.dir[want].size;
        VM vm; vm.mem = calloc(VM_MEM, sizeof(s16)); vm.err = 0;

        unsigned opcount[256]; memset(opcount, 0, sizeof opcount);
        s16 g_result = 0; int g_mode_a = 0, g_mode_b = 0, g_mode_c = 0, scene_loads = 0;
        int act[256], nact = 0, keyreads = 0;
        int bindvk[64], bindres[64], nbind = 0;
        unsigned unimpl[256];  memset(unimpl, 0, sizeof unimpl);
        size_t pc = 0;
        int steps = 0, halted = 0;
        const int LIMIT = 200000;

        printf("running script res %d (%zu bytes)\n", want, n);
        while (pc < n && steps++ < LIMIT) {
            int op = rd16(b + pc);
            int targets[8], ntarget = 0;
            int len = record_len(b, pc, n, targets, &ntarget);
            if (op < 256) opcount[op]++;

            if (op == 0 || op == 44) { halted = 1; break; }

            if (op == 76) {                              /* statement */
                if (len < 4) { halted = 2; break; }
                vm_run(&vm, b + pc + 4, (size_t)len - 4);
                if (trace) printf("  %04zx  op76 statement (%d bytes)\n", pc, len - 4);
                pc += (size_t)len;
            } else if (op == 77 || op == 79) {           /* branch on expression */
                s16 v = vm_run(&vm, b + pc + 6, n - (pc + 6));
                /* Branch targets are relative to the record start, not absolute
                 * offsets into the script. Record 0 hides this, because there
                 * the two are the same number. */
                size_t t = pc + (size_t)(v ? targets[1] : targets[0]);
                if (trace) printf("  %04zx  op%-2d branch -> %04zx (expr=%d)\n", pc, op, t, v);
                if (t >= n) { halted = 3; break; }
                pc = t;
            } else if (op == 37) {                       /* jump */
                s16 adv = (s16)rd16(b + pc + 2);
                if (trace) printf("  %04zx  op37 jump %+d\n", pc, adv);
                if (adv == 0) { halted = 4; break; }
                if ((long)pc + adv < 0 || (size_t)((long)pc + adv) >= n) { halted = 5; break; }
                pc = (size_t)((long)pc + adv);
            } else if (op == 78 || op == 43) {           /* switch */
                s16 v = (op == 78) ? vm_run(&vm, b + pc + 10, n - (pc + 10)) : 0;
                size_t t = pc + (size_t)targets[0];      /* default arm */
                int cnt = b[pc + 8];
                size_t tbl = pc + (op == 78 ? rd16(b + pc + 4) : (size_t)(s16)rd16(b + pc + 4));
                for (i = 0; i < cnt; i++) {
                    size_t q = tbl + (size_t)i * 6;
                    if (q + 6 <= n && (s16)rd16(b + q) == v) { t = pc + rd16(b + q + 4); break; }
                }
                if (trace) printf("  %04zx  op%-2d switch(%d) -> %04zx\n", pc, op, v, t);
                if (t >= n) { halted = 3; break; }
                pc = t;
            } else if (op == 89) {
                /* Evaluate an expression and latch it in the engine's result
                 * global: case 0x59 does DAT_1020_40ae = <eval>. */
                g_result = vm_run(&vm, b + pc + 4, n - (pc + 4));
                if (trace) printf("  %04zx  op89 result = %d\n", pc, g_result);
                pc += (size_t)len;
            } else if (op == 21 || op == 29 || op == 31) {
                /* Simple global sets. 0x15 and 0x1d store the record's word 1;
                 * 0x1f passes its low byte to a mode setter. */
                int v = (len >= 4) ? rd16(b + pc + 2) : 0;
                if (op == 21) g_mode_a = v;
                else if (op == 29) g_mode_b = v;
                else g_mode_c = v & 0xFF;
                if (trace) printf("  %04zx  op%-2d set global = %d\n", pc, op, v);
                pc += (size_t)len;
            } else if (op == 18) {
                /* Scene transition, FUN_1008_8c00: the 18-byte record is copied
                 * to the pending-load slot and WM_USER+0xC9 posted. The name is
                 * 14 bytes at +2, NUL padded. (op 56 is not a transition: it is
                 * FUN_1008_9b20, a wvsprintf into a string variable.) */
                char nm[16]; int k;
                for (k = 0; k < 14 && pc + 2 + k < n && b[pc + 2 + k]; k++) nm[k] = (char)b[pc + 2 + k];
                nm[k] = 0;
                printf("  %04zx  op18 LOAD SCENE \"%s\"\n", pc, nm);
                scene_loads++;
                pc += (size_t)len;
            } else if (op == 28) {
                /* FUN_1008_9394 calls the type 15 accessor on the record's
                 * word 1, so this activates a sprite. Recording which sprites a
                 * scene activates is what turns a running script into a screen. */
                int h = rd16(b + pc + 2);
                int ri = h - (0x10000 - 0x7531);
                if (ri >= 0 && ri < c.ndir && nact < 256) act[nact++] = ri;
                if (trace) printf("  %04zx  op28 ACTIVATE SPRITE res %d%s\n", pc, ri,
                                  (ri >= 0 && ri < c.ndir && c.dir[ri].type == 15) ? "" : " (not type 15)");
                pc += (size_t)len;
            } else if (op == 47) {
                /* FUN_1008_934a resolves the record's word 1 as a variable and
                 * stores GetKeyState() of the virtual key in word 2. */
                int vk = (len >= 6) ? rd16(b + pc + 4) : 0;
                if (trace) printf("  %04zx  op47 READ KEY vk=0x%02x -> var %04x\n",
                                  pc, vk, rd16(b + pc + 2));
                keyreads++;
                pc += (size_t)len;
            } else if (op == 12) {
                /* FUN_1008_29c4 selects a slot from the flag bytes at +6..+10
                 * and stores the word at +4 into it. The word at +2 is a
                 * Windows virtual key code -- 0x1b escape, 0x20 space, 0x6a/6b/6d
                 * the numpad -- so this binds a key to a target handle. */
                int vk = rd16(b + pc + 2), tgt = rd16(b + pc + 4);
                int ri = tgt - (0x10000 - 0x7531);
                if (nbind < 64) { bindvk[nbind] = vk; bindres[nbind] = ri; nbind++; }
                if (trace) printf("  %04zx  op12 BIND key 0x%02x -> res %d%s\n", pc, vk, ri,
                                  (ri >= 0 && ri < c.ndir) ? "" : " (out of range)");
                pc += (size_t)len;
            } else {                                     /* action, not yet implemented */
                if (op < 256) unimpl[op]++;
                if (trace) {
                    int k;
                    printf("  %04zx  op%-3d len %-4d |", pc, op, len);
                    for (k = 2; k < len && k < 34; k++) printf(" %02x", b[pc + k]);
                    /* any printable run is usually a container filename */
                    printf("  \"");
                    for (k = 2; k < len && k < 34; k++)
                        putchar((b[pc+k] >= 32 && b[pc+k] < 127) ? b[pc+k] : '.');
                    printf("\"");
                    for (k = 2; k + 1 < len && k < 12; k += 2) {
                        int ri = rd16(b + pc + k) - (0x10000 - 0x7531);
                        if (ri >= 0 && ri < c.ndir)
                            printf("  [+%d->res%d t%d]", k, ri, c.dir[ri].type);
                    }
                    printf("\n");
                }
                if (len <= 0) { halted = 6; break; }
                pc += (size_t)len;
            }
        }

        static const char *why[] = {"ran off the end", "terminator", "short op76",
                                    "target out of range", "zero jump", "jump out of range",
                                    "unknown record length"};
        printf("halted: %s after %d steps  (result=%d scenes=%d modes=%d/%d/%d)\n",
               why[halted], steps, g_result, scene_loads, g_mode_a, g_mode_b, g_mode_c);
        if (nact) {
            printf("sprites activated (%d):", nact);
            for (i = 0; i < nact && i < 24; i++) printf(" %d", act[i]);
            printf("%s\n", nact > 24 ? " ..." : "");
        }
        if (keyreads) printf("key reads: %d\n", keyreads);
        if (nbind) {
            printf("key bindings (%d):\n", nbind);
            for (i = 0; i < nbind && i < 20; i++) {
                const char *nmv = "";
                switch (bindvk[i]) {
                case 0x1b: nmv = "ESC"; break;      case 0x20: nmv = "SPACE"; break;
                case 0x0d: nmv = "ENTER"; break;    case 0x25: nmv = "LEFT"; break;
                case 0x26: nmv = "UP"; break;       case 0x27: nmv = "RIGHT"; break;
                case 0x28: nmv = "DOWN"; break;     case 0x6a: nmv = "NUM*"; break;
                case 0x6b: nmv = "NUM+"; break;     case 0x6d: nmv = "NUM-"; break;
                }
                printf("   vk 0x%02x %-6s -> res %d%s\n", bindvk[i], nmv, bindres[i],
                       (bindres[i] >= 0 && bindres[i] < c.ndir)
                           ? (c.dir[bindres[i]].type == 14 ? " (script)" : "") : " (oor)");
            }
        }
        printf("opcodes executed:\n");
        for (i = 0; i < 256; i++)
            if (opcount[i])
                printf("   op %-3d x%-6u %s\n", i, opcount[i],
                       unimpl[i] ? "NOT IMPLEMENTED" : "");
        free(vm.mem);
        return 0;
    }

    /* --play  the port's actual render path, interactive.
     *
     * This is not a bitmap browser. It runs the pieces the port is built from,
     * on real disc data:
     *   - type 15 sprite timelines supply the frame lists
     *   - frame_resolve() handles type 10 repositioned bitmaps
     *   - each frame is drawn at its own origin by blit8, index 0 transparent
     *   - the framebuffer stays 8-bit palette-indexed and the palette is
     *     applied at present time, so palette effects stay possible
     *   - frame advance is driven by timing.c, with the engine's accumulating
     *     deadline rather than a rearm from now
     *
     * Keys: left/right sprite, up/down speed, space pause, tab container view,
     *       s sound, q quit.
     */
    if (argc > 2 && strcmp(argv[2], "--play") == 0) {
        const int W = 800, H = 600;   /* engine canvas; StretchDIBits shrinks it on small screens */
        int *spr = malloc(sizeof(int) * (size_t)c.ndir);
        int nspr = 0, i;
        for (i = 0; i < c.ndir; i++) {
            if (c.dir[i].type != 15 || c.dir[i].size < 0x16) continue;
            const u8 *r = resource_bytes(&c, &c.dir[i]);
            int n = rd16(r + 4);
            if (n >= 2 && 0x14 + n * 2 <= (int)c.dir[i].size) spr[nspr++] = i;
        }
        if (!nspr) { fprintf(stderr, "no sprite timelines in %s\n", argv[1]); return 1; }
        printf("%s: %d sprite timelines\n", argv[1], nspr);

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1;
        }
        SDL_Window *win = SDL_CreateWindow("jungle-recomp — player",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W * 2, H * 2, SDL_WINDOW_RESIZABLE);
        SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        SDL_RenderSetLogicalSize(ren, W, H);
        SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, W, H);
        SDL_AudioSpec want; SDL_memset(&want, 0, sizeof want);
        want.freq = 22050; want.format = AUDIO_S16LSB; want.channels = 1; want.samples = 512;   /* 23 ms */
        SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
        if (dev) SDL_PauseAudioDevice(dev, 0);

        u8  *fb   = calloc((size_t)W * H, 1);
        u32 *rgba = malloc((size_t)W * H * 4);

        TimerTable timers;
        timer_reset(&timers);
        int cur = 0, frame = 0, paused = 0, sound = 1, ms = 66, running = 1;
        unsigned long now = SDL_GetTicks();
        timer_set(&timers, 1, 0, (unsigned long)ms, 1, now);

        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) running = 0;
                if (ev.type == SDL_KEYDOWN) {
                    SDL_Keycode k = ev.key.keysym.sym;
                    if (k == SDLK_q || k == SDLK_ESCAPE) running = 0;
                    else if (k == SDLK_RIGHT) { cur = (cur + 1) % nspr; frame = 0; }
                    else if (k == SDLK_LEFT)  { cur = (cur + nspr - 1) % nspr; frame = 0; }
                    else if (k == SDLK_SPACE) paused = !paused;
                    else if (k == SDLK_s)     sound = !sound;
                    else if (k == SDLK_UP)   { if (ms > 16) ms -= 8; timer_set(&timers, 1, 0, (unsigned long)ms, 1, SDL_GetTicks()); }
                    else if (k == SDLK_DOWN) { if (ms < 400) ms += 8; timer_set(&timers, 1, 0, (unsigned long)ms, 1, SDL_GetTicks()); }
                }
            }

            const Entry *se = &c.dir[spr[cur]];
            const u8 *rec = resource_bytes(&c, se);
            int count = rd16(rec + 4);

            now = SDL_GetTicks();
            int advanced = 0;
            if (!paused) advanced = timer_service(&timers, now, NULL, NULL);
            if (advanced) frame = (frame + 1) % count;

            /* compose: index 0 is transparent, so clear to it */
            memset(fb, 0, (size_t)W * H);
            int handle = rd16(rec + 0x14 + frame * 2);
            int fx, fy, idx2 = frame_resolve(&c, handle, &fx, &fy);
            if (idx2 >= 0) {
                int w, h; u8 *px = NULL;
                if (bitmap_decode(resource_bytes(&c, &c.dir[idx2]), c.dir[idx2].size, &w, &h, &px)) {
                    /* centre the timeline's own coordinate space in the window */
                    blit8(fb, W, W, H, W / 2 + fx, H / 2 + fy, px, w, w, h, 0);
                    free(px);
                }
            } else if (advanced && sound && dev && g_step) {
                int si = handle - (0x10000 - 0x7531);
                if (si >= 0 && si < c.ndir && c.dir[si].type == 7) {
                    const u8 *r = resource_bytes(&c, &c.dir[si]);
                    u32 ds = rd32(r);
                    if (ds + 28 <= c.dir[si].size) {
                        s16 *pcm = malloc((size_t)ds * 2 * sizeof(s16));
                        size_t got = adpcm_decode(r + 28, ds, pcm);
                        SDL_QueueAudio(dev, pcm, (u32)(got * sizeof(s16)));
                        free(pcm);
                    }
                }
            }

            /* present: expand through the live palette, bottom-up like a DIB */
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    u8 v = fb[(size_t)(H - 1 - y) * W + x];
                    rgba[(size_t)y * W + x] = 0xFF000000u |
                        ((u32)c.pal[v][0] << 16) | ((u32)c.pal[v][1] << 8) | c.pal[v][2];
                }
            SDL_UpdateTexture(tex, NULL, rgba, W * 4);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);

            char title[200];
            snprintf(title, sizeof title,
                     "jungle-recomp — sprite %d [%d/%d]  frame %d/%d  %dms%s",
                     spr[cur], cur + 1, nspr, frame + 1, count, ms, paused ? "  PAUSED" : "");
            SDL_SetWindowTitle(win, title);
            SDL_Delay(4);
        }
        SDL_Quit();
        return 0;
    }

    /* --anim [INDEX OUT.GIF]  play a type 15 sprite record as an animation.
     *
     * A type 15 record is a sprite: a u16 count at +0x04 and that many resource
     * handles from +0x14. A handle resolves to a directory index by the
     * engine's own address form -- it computes handle + 0x7531, and resources
     * are identified by 0x10000 + index, so index = handle - 0x8ACF.
     * See docs/FORMAT.md.
     *
     * Frames are emitted as GIF because GIF's colour model is the engine's:
     * 8-bit indices plus a 256-entry palette, so nothing is converted and
     * index 0 stays transparent. */
    if (argc > 2 && strcmp(argv[2], "--anim") == 0) {
        int want = argc > 3 ? atoi(argv[3]) : -1;
        const char *out = argc > 4 ? argv[4] : "anim.gif";
        int delay_ms = argc > 5 ? atoi(argv[5]) : 66;

        int chosen = -1, count = 0, i;
        const u8 *rec = NULL;

        for (i = 0; i < c.ndir; i++) {
            if (c.dir[i].type != 15 || c.dir[i].size < 0x16) continue;
            const u8 *r = resource_bytes(&c, &c.dir[i]);
            int n = rd16(r + 4);
            if (0x14 + n * 2 > (int)c.dir[i].size) continue;
            if (want >= 0 ? (i == want) : (n > count)) { chosen = i; count = n; rec = r; }
            if (want >= 0 && i == want) break;
        }
        if (chosen < 0) { fprintf(stderr, "no usable type 15 record\n"); return 1; }

        /* Frames are cropped individually, so each is placed relative to the
         * sprite anchor by frame_resolve(). The canvas is the union of all
         * placed frames. */
        int minx = 0x7FFF, miny = 0x7FFF, maxx = -0x7FFF, maxy = -0x7FFF, usable = 0;
        for (i = 0; i < count; i++) {
            int fx, fy, idx = frame_resolve(&c, rd16(rec + 0x14 + i * 2), &fx, &fy);
            if (idx < 0) continue;
            const u8 *hdr = resource_bytes(&c, &c.dir[idx]);
            int fw = rd16(hdr + 0x04), fh = rd16(hdr + 0x06);
            if (fx < minx) minx = fx;
            if (fy < miny) miny = fy;
            if (fx + fw > maxx) maxx = fx + fw;
            if (fy + fh > maxy) maxy = fy + fh;
            usable++;
        }
        int W = maxx - minx, H = maxy - miny;
        if (W < 1 || W > 4096) W = 1;
        if (H < 1 || H > 4096) H = 1;
        if (!usable) { fprintf(stderr, "record %d has no decodable bitmap frames\n", chosen); return 1; }

        Gif g;
        if (!gif_open(&g, out, W, H, c.pal)) { fprintf(stderr, "cannot write %s\n", out); return 1; }
        u8 *fb = malloc((size_t)W * H);
        int written = 0;
        for (i = 0; i < count; i++) {
            int fx, fy, idx = frame_resolve(&c, rd16(rec + 0x14 + i * 2), &fx, &fy);
            if (idx < 0) continue;
            int w, h; u8 *px = NULL;
            if (!bitmap_decode(resource_bytes(&c, &c.dir[idx]), c.dir[idx].size, &w, &h, &px)) continue;
            memset(fb, 0, (size_t)W * H);               /* index 0 = transparent */
            blit8(fb, W, W, H, fx - minx, fy - miny, px, w, w, h, 0);
            gif_frame(&g, fb, delay_ms / 10, 0);
            written++; free(px);
        }
        gif_close(&g);
        free(fb);
        printf("%s: record %d, %d frames (%d drawn) %dx%d @ %dms -> %s\n",
               argv[1], chosen, count, written, W, H, delay_ms, out);
        return 0;
    }

    /* --compose [W H OUT.PNG]  composite every bitmap in the container onto one
     * palette-indexed framebuffer through the recovered compositor, then write
     * a PNG. This exercises the whole chain headlessly -- container, bitmap
     * codecs, palette, blit8 -- with no window and no SDL. */
    if (argc > 2 && strcmp(argv[2], "--compose") == 0) {
        int W = argc > 3 ? atoi(argv[3]) : 800;
        int H = argc > 4 ? atoi(argv[4]) : 600;
        const char *out = argc > 5 ? argv[5] : "compose.png";
        u8 *fb = calloc((size_t)W * H, 1);
        if (!fb) { fprintf(stderr, "out of memory\n"); return 1; }
        int placed = 0; long drawn = 0; int x = 0, y = 0, rowh = 0;
        for (int i = 0; i < c.ndir; i++) {
            if (c.dir[i].type != 1) continue;
            int w, h; u8 *px = NULL;
            if (!bitmap_decode(resource_bytes(&c, &c.dir[i]), c.dir[i].size, &w, &h, &px)) continue;
            if (x + w > W) { x = 0; y += rowh; rowh = 0; }
            if (y >= H) { free(px); break; }
            drawn += blit8(fb, W, W, H, x, y, px, w, w, h, 0);
            x += w; if (h > rowh) rowh = h;
            placed++; free(px);
        }
        int ok = png_write(out, W, H, fb, c.pal);
        printf("%s: composited %d bitmaps, %ld opaque pixels -> %s (%s)\n",
               argv[1], placed, drawn, out, ok ? "written" : "FAILED");
        free(fb);
        return ok ? 0 : 1;
    }

    if (argc > 2 && strcmp(argv[2], "--verify") == 0) {
        const char *base = strrchr(argv[1], '/');
        return verify(&c, base ? base + 1 : argv[1]) == 0 ? 0 : 1;
    }

    int *bmp = malloc(sizeof(int) * (size_t)c.ndir);
    int nb = 0;
    for (int i = 0; i < c.ndir; i++) if (c.dir[i].type == 1) bmp[nb++] = i;
    printf("%s: %d resources, %d bitmaps\n", argv[1], c.ndir, nb);
    if (!nb) return 1;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_Window *win = SDL_CreateWindow("jungle-recomp", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);

    /* audio: open the device once, at the format every clip on the disc uses */
    SDL_AudioSpec want; SDL_memset(&want, 0, sizeof want);
    want.freq = 22050; want.format = AUDIO_S16LSB; want.channels = 1; want.samples = 1024;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);

    int *snd = malloc(sizeof(int) * (size_t)c.ndir);
    int ns = 0;
    for (int i = 0; i < c.ndir; i++) if (c.dir[i].type == 7) snd[ns++] = i;

    int idx = 0, running = 1, sidx = 0;
    SDL_Texture *tex = NULL;
    int tw = 0, th = 0;

    while (running) {
        if (!tex) {
            Entry *e = &c.dir[bmp[idx]];
            int w, h; u8 *px;
            if (bitmap_decode(resource_bytes(&c, e), e->size, &w, &h, &px)) {
                u32 *rgba = malloc((size_t)w * h * 4);
                for (int y = 0; y < h; y++)                       /* stored bottom-up */
                    for (int x = 0; x < w; x++) {
                        u8 v = px[(size_t)(h - 1 - y) * w + x];
                        rgba[(size_t)y * w + x] = 0xFF000000u |
                            ((u32)c.pal[v][0] << 16) | ((u32)c.pal[v][1] << 8) | c.pal[v][2];
                    }
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STATIC, w, h);
                SDL_UpdateTexture(tex, NULL, rgba, w * 4);
                tw = w; th = h;
                free(rgba); free(px);
                char title[160];
                snprintf(title, sizeof title, "jungle-recomp — res %d  %dx%d  [%d/%d]",
                         bmp[idx], w, h, idx + 1, nb);
                SDL_SetWindowTitle(win, title);
            } else { idx = (idx + 1) % nb; continue; }
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_q || k == SDLK_ESCAPE) running = 0;
                else if (k == SDLK_RIGHT || k == SDLK_SPACE)
                    { idx = (idx + 1) % nb; SDL_DestroyTexture(tex); tex = NULL; }
                else if (k == SDLK_LEFT)
                    { idx = (idx + nb - 1) % nb; SDL_DestroyTexture(tex); tex = NULL; }
                else if (k == SDLK_a && dev && ns && g_step) {
                    /* play the next audio resource */
                    Entry *se = &c.dir[snd[sidx]];
                    const u8 *r = resource_bytes(&c, se);
                    u32 ds = rd32(r);
                    if (ds + 28 <= se->size) {
                        s16 *pcm = malloc((size_t)ds * 2 * sizeof(s16));
                        size_t got = adpcm_decode(r + 28, ds, pcm);
                        SDL_ClearQueuedAudio(dev);
                        SDL_QueueAudio(dev, pcm, (Uint32)(got * sizeof(s16)));
                        printf("playing clip %d  (%.2fs)\n", snd[sidx], got / 22050.0);
                        free(pcm);
                    }
                    sidx = (sidx + 1) % ns;
                }
            }
        }
        if (!tex) continue;

        int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
        /* integer scale, letterboxed — resolution independence for free */
        int s = ww / tw; int s2 = wh / th; if (s2 < s) s = s2; if (s < 1) s = 1;
        SDL_Rect dst = { (ww - tw * s) / 2, (wh - th * s) / 2, tw * s, th * s };
        SDL_SetRenderDrawColor(ren, 20, 20, 24, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, &dst);
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
