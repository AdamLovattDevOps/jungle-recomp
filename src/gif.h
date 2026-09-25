#ifndef JUNGLE_GIF_H
#define JUNGLE_GIF_H
#include <stdio.h>

/* Minimal GIF89a writer for palette-indexed frames.
 *
 * The engine's framebuffer is 8-bit indexed with a 256-entry palette, which is
 * exactly GIF's native model -- so frames go out with no colour conversion and
 * no loss, and index 0 stays transparent as the compositor intends.
 */
typedef struct {
    FILE *f;
    int   w, h;
} Gif;

int  gif_open(Gif *g, const char *path, int w, int h, unsigned char pal[256][3]);
/* delay_cs is in centiseconds, as the GIF format requires. */
int  gif_frame(Gif *g, const unsigned char *px, int delay_cs, int transparent_index);
void gif_close(Gif *g);

#endif
