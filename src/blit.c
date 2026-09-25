/* blit.c — sprite compositor.
 *
 * Transcribed from FUN_1000_2848 in JUNGS01.DLL (1000:2848, 2033 bytes), the
 * single per-sprite blit the engine routes every sprite through. Evidence and
 * call sites: notes/decomp/JUNGS01.DLL.c lines 2992-4039.
 *
 * Semantics recovered from the uncompressed path (the branch taken when
 * bit 7 of the source descriptor's byte 9 is clear):
 *
 *   - Palette index 0 is transparent and is not written. The original tests
 *     this per byte: it loads a 16-bit word (two pixels, `int` being 2 bytes
 *     on 16-bit x86), skips the pair outright when the word is zero, then
 *     tests the low and high bytes separately before storing each. That is
 *     per-pixel index-0 rejection, so a plain per-pixel loop is output
 *     identical -- the word load and the 4-pixel unroll are speed, not
 *     meaning.
 *
 *   - Bit 0 of param_1 selects a horizontally mirrored blit. The original
 *     keeps a second copy of the whole loop that walks the source backwards
 *     while the destination still advances forwards, reversing each group of
 *     four as it goes. Net effect is a horizontal mirror, expressed here as a
 *     source-column reflection.
 *
 *   - Rows advance by a caller-supplied stride delta on both sides
 *     (in_EDX for the destination, the stack argument at 0x14 for the source),
 *     which is stride-minus-width. Expressed here as explicit strides.
 *
 * NOT transcribed: the compressed path. The original decodes its RLE during
 * the blit, seeking into the row-offset table so it can skip clipped-away
 * pixels without expanding them, which is how it composited hundreds of
 * sprites on a 1995 machine. This file composites already-decoded surfaces;
 * container_decode_bitmap does the expansion. Output is the same, the work is
 * not. The decode-during-blit path is still open -- see docs/PORT_PLAN.md.
 *
 * Left-clipping in the original is done by walking the RLE opcode stream
 * forward and discarding param_3 pixels before drawing. On a decoded surface
 * that is a rectangle intersection, done below.
 */
#include "blit.h"
#include <stddef.h>

int blit8(unsigned char *dst, int dst_stride, int dst_w, int dst_h,
          int dx, int dy,
          const unsigned char *src, int src_stride, int sw, int sh,
          unsigned flags)
{
    int x0, y0, x1, y1, x, y, n = 0;

    if (!dst || !src || sw <= 0 || sh <= 0) return 0;

    /* The original returns immediately when either extent is zero:
     *   if ((param_7 != 0) && (param_6 != 0)) { ... } */
    x0 = dx < 0 ? 0 : dx;
    y0 = dy < 0 ? 0 : dy;
    x1 = dx + sw; if (x1 > dst_w) x1 = dst_w;
    y1 = dy + sh; if (y1 > dst_h) y1 = dst_h;
    if (x0 >= x1 || y0 >= y1) return 0;

    for (y = y0; y < y1; y++) {
        const unsigned char *srow = src + (size_t)(y - dy) * src_stride;
        unsigned char       *drow = dst + (size_t)y * dst_stride;

        for (x = x0; x < x1; x++) {
            int sx = x - dx;
            unsigned char v;

            if (flags & BLIT_FLIP_X) sx = sw - 1 - sx;
            v = srow[sx];
            if (v) { drow[x] = v; n++; }   /* index 0 transparent */
        }
    }
    return n;
}
