/*
 * Software task watchdog, 32 s window, backed by the SoC's hardware watchdog.
 *
 * Both layers have to be asked for explicitly, and this module used to ask
 * for neither:
 *
 *   - task_wdt only configures the hardware fallback when task_wdt_init() is
 *     given a device.  CONFIG_TASK_WDT_HW_FALLBACK=y alone does nothing;
 *     passing NULL leaves the task watchdog as a bare kernel timer.
 *   - task_wdt only reboots on its own for a channel registered with *no*
 *     callback.  A registered callback replaces that reboot, so an empty one
 *     turns the timeout into a no-op.
 *
 * With NULL and an empty callback, as this was, a thread that stopped feeding
 * was never noticed at all: a unit that blocked in a modem call sat dead in a
 * car for hours and only came back when it was power-cycled by hand.
 *
 * The callback deliberately does not reboot.  task_wdt stops rescheduling its
 * timer once a channel has expired, so leaving it alone means the hardware
 * watchdog goes unfed and resets the SoC a couple of seconds later — and that
 * reset is a watchdog reset, which RESETREAS records and dbglog reports as
 * rst=wdt on the next record that reaches the server.  sys_reboot() here
 * would report rst=sw instead, indistinguishable from a commanded reboot, and
 * hide exactly the event worth seeing.  Without a hardware backend there is
 * nothing to fall back on, so then it does reboot.
 *
 * Before it goes, the callback notes where the loop was stuck.  rst=wdt
 * alone said nothing at 14:56 on 2026-10-11: the car reset mid-drive during
 * an operator change, and the warnings that would have explained it were in
 * RAM.  The main thread is blocked at that moment, so its stack holds the
 * return addresses of the calls it is blocked in: the callback takes the
 * first few code addresses found there into noinit RAM, as fatal.c does,
 * and the next boot reports them in rst= ("stall:<addr>/<addr>/...", look
 * them up in the map file).  A scan, not an unwind, so an address can be a
 * stale one left on the stack; the first few are the innermost and the
 * most telling.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/linker/linker-defs.h>
#include <stdio.h>

#include "app.h"

LOG_MODULE_REGISTER(wdt, CONFIG_APP_LOG_LEVEL);

static const struct device *const s_hw = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int  s_channel = -1;
static bool s_hw_backed;
static struct k_thread *s_fed_by;       /* the thread that kicks: main() */

#define STALL_MAGIC 0x57A11ED0u
#define STALL_ADDRS 4               /* rst= is varchar(48) on the server */
#define STALL_SCAN  192                 /* words of stack looked through */

/* __noinit for the same reason as fatal.c's note. */
static __noinit struct {
    uint32_t magic;
    uint32_t addr[STALL_ADDRS];
} s_stall;

/* The return addresses on the stalled thread's stack, innermost first:
 * Thumb code addresses (odd) inside the image's text. */
static void note_stall(void)
{
    struct k_thread *t = s_fed_by;

    s_stall.magic = 0;
    if (t == NULL) {
        return;
    }

    uint32_t *sp = (uint32_t *)t->callee_saved.psp;
    uint32_t *top = (uint32_t *)(t->stack_info.start + t->stack_info.size);
    uint32_t lo = (uint32_t)__text_region_start;
    uint32_t hi = (uint32_t)__text_region_end;
    int n = 0;

    if (sp < (uint32_t *)t->stack_info.start || sp >= top) {
        return;
    }
    for (int i = 0; i < STALL_SCAN && sp + i < top && n < STALL_ADDRS;
         i++) {
        uint32_t w = sp[i];

        if ((w & 1u) && w >= lo && w < hi) {
            s_stall.addr[n++] = w & ~1u;
        }
    }
    while (n < STALL_ADDRS) {
        s_stall.addr[n++] = 0;
    }
    s_stall.magic = STALL_MAGIC;
}

const char *watchdog_last_stall(void)
{
    static char s_str[64];
    static bool s_read;

    if (!s_read) {
        s_read = true;
        if (s_stall.magic == STALL_MAGIC) {
            int pos = snprintf(s_str, sizeof(s_str), "stall:");

            for (int i = 0; i < STALL_ADDRS && s_stall.addr[i] &&
                 pos < (int)sizeof(s_str); i++) {
                pos += snprintf(s_str + pos, sizeof(s_str) - pos, "%s%x",
                                i ? "/" : "", (unsigned)s_stall.addr[i]);
            }
            LOG_ERR("previous boot's watchdog: %s", s_str);
        }
        s_stall.magic = 0;
    }
    return s_str[0] ? s_str : NULL;
}

static void wdt_timeout_cb(int channel_id, void *user_data)
{
    ARG_UNUSED(channel_id);
    ARG_UNUSED(user_data);

    note_stall();
    LOG_ERR("no kick for %ds — resetting", WATCHDOG_TIMEOUT_S);
    /* Deferred logging: without this the message dies with the reset. */
    LOG_PANIC();

    if (!s_hw_backed) {
        sys_reboot(SYS_REBOOT_COLD);
    }
}

void watchdog_init(void)
{
    const struct device *hw = s_hw;

    if (!device_is_ready(hw)) {
        LOG_ERR("%s not ready — no hardware backing", hw->name);
        hw = NULL;
    }

    int err = task_wdt_init(hw);
    if (err && err != -EALREADY) {
        LOG_WRN("task_wdt_init: %d", err);
        return;
    }
    s_hw_backed = (hw != NULL);
    s_fed_by = k_current_get();

    s_channel = task_wdt_add(WATCHDOG_TIMEOUT_S * 1000, wdt_timeout_cb, NULL);
    if (s_channel < 0) {
        LOG_WRN("task_wdt_add: %d", s_channel);
        return;
    }
    LOG_INF("enabled (%ds, %s)", WATCHDOG_TIMEOUT_S,
            s_hw_backed ? "hw-backed" : "software only");
}

void watchdog_kick(void)
{
    crumb_alive();      /* the breaker's note of the last sign of life */
    if (s_channel >= 0) {
        task_wdt_feed(s_channel);
    }
}
