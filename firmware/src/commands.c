/*
 * Server command dispatch.  Format: "key=value[,key=value...]".
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(cmd, CONFIG_APP_LOG_LEVEL);

void cmd_run(char *cmd)
{
    char *tmp;
    long val;

    LOG_INF("cmd: %s", cmd);

    tmp = strstr(cmd, "int=");
    if (tmp) {
        tmp += strlen("int=");
        val = atol(tmp);
        if (val != 0 && val < 10) val = 10;
        int old = g_settings.loop_interval;
        g_settings.loop_interval = val;
        send_int_to_server = true;
        char msg[60];
        snprintf(msg, sizeof(msg),
                 "engine-off interval changed; %d -> %ld", old, val);
        alert_enqueue(msg, 0);
    }

    tmp = strstr(cmd, "movealarm=");
    if (tmp) {
        tmp += strlen("movealarm=");
        val = atoi(tmp);
        g_settings.movement_alarm = (val != 0) ? 1 : 0;
        send_int_to_server = true;
        alert_enqueue(g_settings.movement_alarm
                         ? "movement alarm ON"
                         : "movement alarm OFF",
                      0);
    }

    if (strstr(cmd, "movereset")) {
        movement_reset();
        alert_enqueue("movement alarm reset", 0);
    }

    tmp = strstr(cmd, "ao=");
    if (tmp) {
        tmp += 3;
        int8_t old_ao = g_settings.always_on;
        g_settings.always_on = (atoi(tmp) != 0) ? 1 : 0;
        send_int_to_server = true;
        if (g_settings.always_on != old_ao) {
            if (g_settings.always_on) {
                relay_set();
            } else if (ignition != 0) {
                relay_reset();
            }
        }
        alert_enqueue(g_settings.always_on
                         ? "always-on ON"
                         : "always-on OFF",
                      0);
    }

    tmp = strstr(cmd, "relay=");
    if (tmp) {
        tmp += 6;
        if (atoi(tmp)) {
            relay_set();
            alert_enqueue("relay SET", 0);
        } else {
            relay_reset();
            alert_enqueue("relay RESET", 0);
        }
    }

    if (strstr(cmd, "locatenow")) {
        collect_data(ignition);
        send_data();
        char msg[60];
        snprintf(msg, sizeof(msg), "google: %s,%s",
                 g_gnss.lat_str, g_gnss.lon_str);
        alert_enqueue(msg, 0);
    } else if (strstr(cmd, "locate")) {
        char msg[60];
        snprintf(msg, sizeof(msg), "google: %s,%s",
                 g_gnss.lat_str, g_gnss.lon_str);
        alert_enqueue(msg, 0);
    }

    if (strstr(cmd, "tomtomnow")) {
        collect_data(ignition);
        send_data();
        char msg[120];
        snprintf(msg, sizeof(msg), "tomtom: %s,%s",
                 g_gnss.lat_str, g_gnss.lon_str);
        alert_enqueue(msg, 0);
    } else if (strstr(cmd, "tomtom")) {
        char msg[120];
        snprintf(msg, sizeof(msg), "tomtom: %s,%s",
                 g_gnss.lat_str, g_gnss.lon_str);
        alert_enqueue(msg, 0);
    }

    if (strstr(cmd, "config")) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "int=%d ao=%d ma=%d bat=%.1fV ign=%s up=%llus",
                 g_settings.loop_interval,
                 (int)g_settings.always_on,
                 (int)g_settings.movement_alarm,
                 (double)battery_v,
                 (ignition == 0) ? "on" : "off",
                 k_uptime_get() / 1000);
        alert_enqueue(msg, 0);
    }

    if (strstr(cmd, "reboot")) {
        power_reboot = true;
        alert_enqueue("rebooting", 0);
    }
}
