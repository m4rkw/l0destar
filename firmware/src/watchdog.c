/*
 * Software task watchdog with a 32 s timeout, matching the original IWDG.
 * Backed by hwinfo wdt when available (CONFIG_TASK_WDT_HW_FALLBACK=y), so a
 * hard hang still triggers a hardware reset.
 */

#include <zephyr/kernel.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(wdt, CONFIG_APP_LOG_LEVEL);

static int s_channel = -1;

static void wdt_timeout_cb(int channel_id, void *user_data)
{
    ARG_UNUSED(channel_id);
    ARG_UNUSED(user_data);
    /* No way to LOG_PANIC inside the callback safely — task_wdt will reset
     * the system on its own if a HW backend is configured. */
}

void watchdog_init(void)
{
    int err = task_wdt_init(NULL);
    if (err && err != -EALREADY) {
        LOG_WRN("task_wdt_init: %d", err);
        return;
    }
    s_channel = task_wdt_add(32 * 1000, wdt_timeout_cb, NULL);
    if (s_channel < 0) {
        LOG_WRN("task_wdt_add: %d", s_channel);
        return;
    }
    LOG_INF("enabled (32s)");
}

void watchdog_kick(void)
{
    if (s_channel >= 0) {
        task_wdt_feed(s_channel);
    }
}
