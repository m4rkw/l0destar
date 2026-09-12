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
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include "app.h"

LOG_MODULE_REGISTER(wdt, CONFIG_APP_LOG_LEVEL);

static const struct device *const s_hw = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int  s_channel = -1;
static bool s_hw_backed;

static void wdt_timeout_cb(int channel_id, void *user_data)
{
    ARG_UNUSED(channel_id);
    ARG_UNUSED(user_data);

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
    if (s_channel >= 0) {
        task_wdt_feed(s_channel);
    }
}
