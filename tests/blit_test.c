/* blit_test.c — compositor behaviour, no assets required. */
#include "../src/blit.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    unsigned char dst[8 * 8], src[4 * 4];
    int n, i;

    /* 1. index 0 is transparent, everything else is written */
    memset(dst, 0xAA, sizeof dst);
    for (i = 0; i < 16; i++) src[i] = (i % 2) ? 0 : 0x11;
    n = blit8(dst, 8, 8, 8, 0, 0, src, 4, 4, 4, 0);
    CHECK(n == 8);                       /* half the pixels are index 0 */
    CHECK(dst[0] == 0x11);               /* opaque written */
    CHECK(dst[1] == 0xAA);               /* transparent left alone */

    /* 2. horizontal flip reflects the row */
    memset(dst, 0, sizeof dst);
    for (i = 0; i < 16; i++) src[i] = (unsigned char)(i + 1);
    blit8(dst, 8, 8, 8, 0, 0, src, 4, 4, 4, BLIT_FLIP_X);
    CHECK(dst[0] == 4 && dst[1] == 3 && dst[2] == 2 && dst[3] == 1);
    blit8(dst, 8, 8, 8, 0, 0, src, 4, 4, 4, 0);
    CHECK(dst[0] == 1 && dst[1] == 2 && dst[2] == 3 && dst[3] == 4);

    /* 3. clipped off each edge, nothing written, no overrun */
    memset(dst, 0x55, sizeof dst);
    CHECK(blit8(dst, 8, 8, 8, -4, 0, src, 4, 4, 4, 0) == 0);
    CHECK(blit8(dst, 8, 8, 8,  8, 0, src, 4, 4, 4, 0) == 0);
    CHECK(blit8(dst, 8, 8, 8, 0, -4, src, 4, 4, 4, 0) == 0);
    CHECK(blit8(dst, 8, 8, 8, 0,  8, src, 4, 4, 4, 0) == 0);
    for (i = 0; i < 64; i++) CHECK(dst[i] == 0x55);

    /* 4. partial clip top-left: only the bottom-right quadrant lands */
    memset(dst, 0, sizeof dst);
    n = blit8(dst, 8, 8, 8, -2, -2, src, 4, 4, 4, 0);
    CHECK(n == 4);
    CHECK(dst[0] == 11);                 /* src row 2, col 2 -> value 11 */

    /* 5. partial clip bottom-right against the destination edge */
    memset(dst, 0, sizeof dst);
    n = blit8(dst, 8, 8, 8, 6, 6, src, 4, 4, 4, 0);
    CHECK(n == 4);
    CHECK(dst[6 * 8 + 6] == 1);

    /* 6. flip combined with a left clip reflects before clipping */
    memset(dst, 0, sizeof dst);
    blit8(dst, 8, 8, 8, -2, 0, src, 4, 4, 4, BLIT_FLIP_X);
    CHECK(dst[0] == 2 && dst[1] == 1);

    /* 7. degenerate extents are refused, matching the original's guard */
    CHECK(blit8(dst, 8, 8, 8, 0, 0, src, 4, 0, 4, 0) == 0);
    CHECK(blit8(dst, 8, 8, 8, 0, 0, src, 4, 4, 0, 0) == 0);

    printf(fails ? "blit: %d FAILED\n" : "blit: all checks passed\n", fails);
    return fails != 0;
}
