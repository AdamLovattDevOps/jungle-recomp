#ifndef JUNGLE_BLIT_H
#define JUNGLE_BLIT_H

/* Sprite compositor, transcribed from FUN_1000_2848 in JUNGS01.DLL.
 * See docs/FINDINGS.md, "The compositor blits from compressed source".
 *
 * Operates on 8-bit palette-indexed surfaces. Index 0 is transparent.
 */

#define BLIT_FLIP_X 0x01        /* bit 0 of the original's param_1 */

/* Composite an 8-bit indexed sprite onto an 8-bit indexed destination.
 *
 * dst/dst_stride/dst_w/dst_h  destination surface
 * dx, dy                      top-left placement, may be negative
 * src/src_stride/sw/sh        source sprite
 * flags                       BLIT_FLIP_X
 *
 * Returns the number of pixels written (transparent pixels are not counted).
 */
int blit8(unsigned char *dst, int dst_stride, int dst_w, int dst_h,
          int dx, int dy,
          const unsigned char *src, int src_stride, int sw, int sh,
          unsigned flags);

#endif
