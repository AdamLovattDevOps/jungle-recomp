#ifndef JUNGLE_TIMING_H
#define JUNGLE_TIMING_H

/* The engine's timer semantics, transcribed from JUNGLE.EXE.
 * Evidence: docs/FIDELITY.md, "Timing".
 *
 * Deliberately not a generic timer library. The details that are copied here
 * are the ones that decide whether the port feels like the original.
 */

#define TIMER_SLOTS 8          /* the engine's own ceiling */

typedef struct {
    unsigned long deadline;    /* absolute, milliseconds */
    unsigned long interval;    /* milliseconds */
    unsigned short id;         /* what the engine dispatches on */
    unsigned short callback;
    unsigned char  repeat;
    unsigned char  live;
} Timer;

typedef struct {
    Timer slot[TIMER_SLOTS];
    int   count;
} TimerTable;

void timer_reset(TimerTable *t);

/* Install or replace by id. Returns the slot, or -1 when full.
 * Matches the engine: it scans for a matching id first and overwrites, and
 * only appends when the id is new and there is room. */
int  timer_set(TimerTable *t, unsigned short id, unsigned short callback,
               unsigned long interval, int repeat, unsigned long now);

/* Service due timers. Calls fire(id, callback, user) for each, in slot order.
 * Returns how many fired. */
int  timer_service(TimerTable *t, unsigned long now,
                   void (*fire)(unsigned short id, unsigned short callback, void *user),
                   void *user);

#endif
