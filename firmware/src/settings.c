/*
 * Settings: in-memory only for now.  Persistence to NVS will come later when
 * we wire the Zephyr settings subsystem; the original Polaris build kept
 * everything on the SD card, which doesn't exist on the DK.
 */

#include <string.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(settings, CONFIG_APP_LOG_LEVEL);

struct app_settings g_settings;

static void settings_defaults(void)
{
    memset(&g_settings, 0, sizeof(g_settings));
    const char *apn = CONFIG_APP_APN;
    if (!apn || apn[0] == '\0') apn = DEFAULT_APN;
    strncpy(g_settings.apn,  apn,  sizeof(g_settings.apn) - 1);
    strncpy(g_settings.user, DEFAULT_USER, sizeof(g_settings.user) - 1);
    strncpy(g_settings.pwd,  DEFAULT_PASS, sizeof(g_settings.pwd) - 1);
    g_settings.loop_interval  = ENGINE_OFF_LOOP_INTERVAL;
    g_settings.movement_alarm = DEFAULT_MOVEMENT_ALARM;

    const char *psk_hex = CONFIG_APP_PSK_HEX;
    if (!psk_hex || psk_hex[0] == '\0') {
        psk_hex = PSK_HEX_DEFAULT;
    }
    if (!crypto_psk_from_hex(psk_hex, g_settings.psk)) {
        LOG_ERR("PSK_HEX malformed — refusing to send");
        memset(g_settings.psk, 0, sizeof(g_settings.psk));
    }
}

void settings_load(void)
{
    settings_defaults();
    settings_print();
}

void settings_print(void)
{
    LOG_INF("apn=%s user=%s", g_settings.apn, g_settings.user);
    LOG_INF("int=%d ma=%d", g_settings.loop_interval,
            (int)g_settings.movement_alarm);
    LOG_INF("imei=%s", g_settings.imei[0] ? g_settings.imei : "(unset)");
}
