/* hires.h — GPU drawing from the high-resolution art cache (see hires.c). */
#ifndef HIRES_H
#define HIRES_H
#include <SDL.h>
#include "engine.h"

typedef struct HiRes HiRes;
HiRes *hires_open(SDL_Renderer *ren, const char *base, int level);   /* base holds x3/ and x4/ <CONTAINER>/<NNNNN>.png */
int    hires_has_level(const char *base, int level);        /* art levels 1, 2, 3, 4 */
int    hires_draw(HiRes *h, Engine *e, SDL_FRect dst);    /* 0: draw the 8-bit frame instead */
void   hires_stats(HiRes *h, int *art, int *fallback);
void   hires_close(HiRes *h);

#endif
