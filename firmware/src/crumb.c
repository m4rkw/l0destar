/*
 * Where the hold breaker was when the unit last gave a sign of life, kept in
 * noinit RAM so that the next boot can say so after a reset nobody asked for
 * (CONFIG_APP_HOLD_BREAK).  holdbreak.h has the phases and the field's format.
 *
 * Added after 2026-10-26 13:10, when the car's watchdog reset it 130 s after
 * a cut: the log lines that would have said where it was were in RAM, and no
 * stall address was captured (nor in the car's three earlier watchdog resets,
 * which suggests the software callback never ran: the hardware fallback fired
 * on a wedge below thread level).  A word written to RAM before the reset
 * does not need anything to run after it.
 *
 * The main thread writes it, at the breaker's transitions and from
 * watchdog_kick(), and nothing else does: no lock.  The kicks are what make
 * it a sign of life: the last one says when the main thread stopped, and the
 * phase says what it had last begun.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>

#include "app.h"
#include "holdbreak.h"

#define CRUMB_MAGIC 0xB4EAD0C5u

/* __noinit for the same reason as fatal.c's note: zeroing it at boot would
 * erase what it exists to carry. */
static __noinit struct {
    uint32_t magic;
    uint32_t cut_ms;        /* when the cycle began: time zero */
    uint32_t phase_ms;      /* when the phase began */
    uint32_t alive_ms;      /* the last watchdog kick while a phase was open */
    char     phase;         /* 0: no cycle under way */
} s_live;

static char s_prev[16];     /* what the last boot left, as the field */

/* Before main(), so that no kick of this boot has written over the last one's
 * time: keep what the last boot left and start this one's note clean.  Memory
 * that was never written (a power-on) fails the magic and reads as nothing. */
static int crumb_boot(void)
{
    if (s_live.magic == CRUMB_MAGIC && s_live.phase != 0) {
        (void)hb_crumb_format(s_prev, sizeof(s_prev), s_live.phase,
                              s_live.cut_ms, s_live.phase_ms, s_live.alive_ms);
    }
    s_live.magic = CRUMB_MAGIC;
    s_live.cut_ms = 0;
    s_live.phase_ms = 0;
    s_live.alive_ms = 0;
    s_live.phase = 0;
    return 0;
}
SYS_INIT(crumb_boot, APPLICATION, 0);

void crumb_cut(void)
{
    const uint32_t now = k_uptime_get_32();

    s_live.cut_ms = now;
    s_live.phase_ms = now;
    s_live.alive_ms = now;
    s_live.phase = HB_PH_CUT;
}

/* The phase last, so a reset between the writes finds the old phase with a
 * newer time (read as 0 s in it) and not a new phase with an old one. */
void crumb_phase(char phase)
{
    const uint32_t now = k_uptime_get_32();

    s_live.phase_ms = now;
    s_live.alive_ms = now;
    s_live.phase = phase;
}

char crumb_phase_get(void)
{
    return s_live.phase;
}

void crumb_alive(void)
{
    if (s_live.phase != 0) {
        s_live.alive_ms = k_uptime_get_32();
    }
}

const char *crumb_previous(void)
{
    return s_prev[0] ? s_prev : NULL;
}
