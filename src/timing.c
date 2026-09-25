/* timing.c — the engine's timer model.
 *
 * Transcribed from FUN_1008_debe (install) and FUN_1008_df36 (service) in
 * JUNGLE.EXE. See docs/FIDELITY.md.
 *
 * Two properties are load-bearing for fidelity and are the reason this is not
 * just a call to SDL_AddTimer:
 *
 * 1. The clock is a free-running millisecond counter read by polling
 *    (timeGetTime, 13 call sites). There is no SetTimer and no WM_TIMER
 *    anywhere in any of the five modules, so no part of the engine's pacing
 *    depends on Windows message delivery. SDL_GetTicks is an exact substitute.
 *
 * 2. A repeating timer rearms as `deadline += interval`, NOT `deadline = now +
 *    interval`. The engine accumulates from the previous deadline, so a late
 *    frame is caught up rather than absorbed. Rearming from `now` would make
 *    the game run progressively slower under load instead of dropping frames,
 *    which is exactly the kind of drift that makes a port feel wrong while
 *    every individual number still looks right.
 */
#include "timing.h"
#include <string.h>

void timer_reset(TimerTable *t)
{
    memset(t, 0, sizeof *t);
}

int timer_set(TimerTable *t, unsigned short id, unsigned short callback,
              unsigned long interval, int repeat, unsigned long now)
{
    int i;

    /* The engine scans the live entries for this id and overwrites in place,
     * appending only when it is new -- so re-arming an existing timer does not
     * consume a slot. */
    for (i = 0; i < t->count; i++)
        if (t->slot[i].live && t->slot[i].id == id)
            break;

    if (i >= TIMER_SLOTS)
        return -1;
    if (i == t->count)
        t->count++;

    t->slot[i].id       = id;
    t->slot[i].callback = callback;
    t->slot[i].interval = interval;
    t->slot[i].repeat   = (unsigned char)(repeat != 0);
    t->slot[i].deadline = now + interval;
    t->slot[i].live     = 1;
    return i;
}

int timer_service(TimerTable *t, unsigned long now,
                  void (*fire)(unsigned short, unsigned short, void *), void *user)
{
    int i, n = 0;

    for (i = 0; i < t->count; i++) {
        Timer *s = &t->slot[i];
        if (!s->live || now < s->deadline)
            continue;

        if (s->repeat && s->interval)
            s->deadline += s->interval;      /* accumulate; do NOT use now */
        else
            s->live = 0;

        if (fire)
            fire(s->id, s->callback, user);
        n++;
    }
    return n;
}
