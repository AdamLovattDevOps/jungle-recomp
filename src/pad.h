#ifndef JUNGLE_PAD_H
#define JUNGLE_PAD_H

/* Game controllers, mapped onto the keys and the mouse the 1995 games read.
 *
 * The games never saw a modern pad: they read the keyboard (bound keys, held
 * keys, and the two redefinable keyboard layouts Bug Drop's players use) and
 * the mouse. Each frame pad_update works out which keys and mouse buttons the
 * pads are holding for the scene on screen and sends the engine only the
 * changes, so a stick, a scene change or a pad unplugged mid-press all end in
 * clean key-ups. The layout per game is in docs/CONTROLLERS.md.
 */

#include <SDL.h>
#include "engine.h"

typedef struct {
    void (*warp)(void *ctx, int x, int y);   /* move the real pointer to canvas x,y */
    void *ctx;
} PadHost;

void pad_init(void);                           /* opens the pads already connected */
void pad_event(const SDL_Event *ev);           /* hot-plug */
void pad_pointer(int x, int y);                /* the real mouse moved: canvas pixels */
void pad_update(Engine *e, unsigned dt_ms, const PadHost *host);

#endif
