/* gif.c — GIF89a writer for the engine's indexed frames.
 *
 * GIF is used here because it is the only common format whose colour model is
 * the same as the engine's: 8-bit indices plus a 256-entry palette. Frames are
 * written with no colour conversion, so what comes out is what the compositor
 * produced, and palette animation stays visible as palette changes rather than
 * being baked into pixels.
 */
#include "gif.h"
#include <string.h>

static void put16(FILE *f, int v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }

int gif_open(Gif *g, const char *path, int w, int h, unsigned char pal[256][3])
{
    int i;
    g->f = fopen(path, "wb");
    if (!g->f) return 0;
    g->w = w; g->h = h;

    fwrite("GIF89a", 1, 6, g->f);
    put16(g->f, w); put16(g->f, h);
    fputc(0xF7, g->f);              /* global table, 256 entries, 8 bits */
    fputc(0, g->f); fputc(0, g->f);
    for (i = 0; i < 256; i++) fwrite(pal[i], 1, 3, g->f);

    /* NETSCAPE2.0 loop-forever block */
    fputc(0x21, g->f); fputc(0xFF, g->f); fputc(11, g->f);
    fwrite("NETSCAPE2.0", 1, 11, g->f);
    fputc(3, g->f); fputc(1, g->f); put16(g->f, 0); fputc(0, g->f);
    return 1;
}

/* LZW as GIF specifies it: variable code width, clear and end codes. */
typedef struct {
    FILE *f;
    unsigned char buf[255];
    int  n;
    unsigned long acc;
    int  bits;
} BitOut;

static void flush_block(BitOut *b)
{
    if (b->n) { fputc(b->n, b->f); fwrite(b->buf, 1, b->n, b->f); b->n = 0; }
}

static void put_code(BitOut *b, int code, int width)
{
    b->acc |= (unsigned long)code << b->bits;
    b->bits += width;
    while (b->bits >= 8) {
        b->buf[b->n++] = (unsigned char)(b->acc & 0xFF);
        b->acc >>= 8; b->bits -= 8;
        if (b->n == 255) flush_block(b);
    }
}

int gif_frame(Gif *g, const unsigned char *px, int delay_cs, int transparent_index)
{
    /* Literal-only LZW with periodic clear codes.
     *
     * GIF requires LZW framing but does not require that anything actually be
     * compressed: a stream of literal codes separated by clear codes before the
     * decoder's table can outgrow the code width is valid and universally
     * decodable. Emitting a clear every 250 codes keeps the width pinned at 9
     * bits, which removes the dictionary-growth and table-full paths entirely.
     *
     * A full LZW encoder was written first and was subtly wrong: it round
     * tripped on small synthetic images but corrupted real frames, because only
     * the real frames -- with their large uniform transparent regions -- ever
     * filled the 4096-entry table and exercised the reset path. The frames here
     * are a few hundred KB of highly repetitive indexed art that the GIF is
     * only a viewing convenience for, so paying about 12% in size to delete an
     * entire class of bug is the right trade. Correctness of the pixels is the
     * point; this is not a shipping asset path.
     */
    const int minc = 8, clear = 1 << minc, endc = clear + 1;
    const int width = minc + 1;
    long i, total = (long)g->w * g->h;
    int since_clear = 0;
    BitOut b;

    fputc(0x21, g->f); fputc(0xF9, g->f); fputc(4, g->f);
    fputc(transparent_index >= 0 ? 0x09 : 0x08, g->f);   /* restore to bg, transparency */
    put16(g->f, delay_cs);
    fputc(transparent_index >= 0 ? transparent_index : 0, g->f);
    fputc(0, g->f);

    fputc(0x2C, g->f);
    put16(g->f, 0); put16(g->f, 0);
    put16(g->f, g->w); put16(g->f, g->h);
    fputc(0, g->f);
    fputc(minc, g->f);

    memset(&b, 0, sizeof b);
    b.f = g->f;
    put_code(&b, clear, width);

    for (i = 0; i < total; i++) {
        put_code(&b, px[i], width);
        if (++since_clear >= 250) {      /* before the table can reach 512 */
            put_code(&b, clear, width);
            since_clear = 0;
        }
    }
    put_code(&b, endc, width);

    if (b.bits) {
        b.buf[b.n++] = (unsigned char)(b.acc & 0xFF);
        if (b.n == 255) flush_block(&b);
    }
    flush_block(&b);
    fputc(0, g->f);
    return 1;
}

void gif_close(Gif *g)
{
    if (!g->f) return;
    fputc(0x3B, g->f);
    fclose(g->f);
    g->f = NULL;
}
