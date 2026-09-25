/* timing_test.c — the properties that preserve the original's feel. */
#include "../src/timing.h"
#include <stdio.h>

static int fired[64], nfired, ntotal;
static void fire(unsigned short id, unsigned short cb, void *user)
{
    (void)cb; (void)user;
    ntotal++;
    if (nfired < 64) fired[nfired++] = id;
}

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    TimerTable t;
    int i;

    /* 1. one-shot fires once and then goes away */
    timer_reset(&t);
    timer_set(&t, 1, 0x4fae, 200, 0, 1000);
    nfired = ntotal = 0;
    CHECK(timer_service(&t, 1199, fire, NULL) == 0);   /* not yet due */
    CHECK(timer_service(&t, 1200, fire, NULL) == 1);   /* due exactly */
    CHECK(timer_service(&t, 5000, fire, NULL) == 0);   /* spent */
    CHECK(nfired == 1 && fired[0] == 1);

    /* 2. a repeating timer does NOT drift: rearm accumulates from the previous
     *    deadline, so a late service still lands on the original grid. */
    timer_reset(&t);
    timer_set(&t, 7, 0, 100, 1, 0);
    timer_service(&t, 100, NULL, NULL);
    CHECK(t.slot[0].deadline == 200);
    timer_service(&t, 385, NULL, NULL);                /* 185 ms late */
    CHECK(t.slot[0].deadline == 300);                  /* NOT 485 */
    timer_service(&t, 385, NULL, NULL);
    CHECK(t.slot[0].deadline == 400);                  /* catching up */

    /* 3. over a long run the grid stays aligned to the start, no accumulated
     *    error -- the property a naive `now + interval` rearm destroys. */
    timer_reset(&t);
    timer_set(&t, 3, 0, 33, 1, 0);
    nfired = ntotal = 0;
    for (i = 1; i <= 100; i++)
        timer_service(&t, (unsigned long)i * 37, fire, NULL);   /* always late */
    CHECK(t.slot[0].deadline % 33 == 0);
    CHECK(ntotal == 100);        /* one fire per service pass, as df36 does:
                                  * a single `if`, not a `while`, so a very late
                                  * service does not replay the missed ticks */

    /* 4. re-arming an existing id reuses its slot rather than consuming one */
    timer_reset(&t);
    timer_set(&t, 5, 0, 10, 1, 0);
    timer_set(&t, 5, 0, 20, 1, 0);
    CHECK(t.count == 1);
    CHECK(t.slot[0].interval == 20);

    /* 5. the engine's eight-slot ceiling is respected */
    timer_reset(&t);
    for (i = 0; i < TIMER_SLOTS; i++) CHECK(timer_set(&t, (unsigned short)(100 + i), 0, 5, 1, 0) >= 0);
    CHECK(timer_set(&t, 999, 0, 5, 1, 0) == -1);

    printf(fails ? "timing: %d FAILED\n" : "timing: all checks passed\n", fails);
    return fails != 0;
}
