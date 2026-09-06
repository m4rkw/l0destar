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

    /* Track mode.  The server puts track=<0|1> on every response so the
     * device converges on its setting after a reboot or a missed reply;
     * only a change is acted on, so the routine repeat is silent. */
    tmp = strstr(cmd, "track=");
    if (tmp) {
        int8_t want = atoi(tmp + strlen("track=")) ? 1 : 0;
        if (want && ignition != 0) {
            /* Track mode is meaningless with the ignition off, and the
             * server clears its switch at key-off anyway; a track=1 seen
             * now is stale (a reply to the final record, or a switch left
             * on by a page that was never revisited).  Not adopted, so a
             * key-on does not start in the mode before the server has had
             * a chance to say otherwise, and not announced. */
            LOG_INF("track=1 with ignition off — ignored");
        } else if (want != g_settings.track_mode) {
            g_settings.track_mode = want;
            LOG_INF("track mode %s", want ? "ON" : "OFF");
            alert_enqueue(want ? "track mode ON" : "track mode OFF", 0);
        }
    }

    if (strstr(cmd, "movereset")) {
        movement_reset();
        alert_enqueue("movement alarm reset", 0);
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

    /* Update indication / force.  The check itself runs from the state
     * machine (STATE_IDLE or the sleep telemetry wake), not from here — a
     * download must not start in the middle of a send.
     *
     * "fota=<version>" rides on every server response: compare against the
     * running build and only mark a check pending when the server has
     * something newer (fota.c logs it once).  A bare "fota" is the manual
     * override — check now regardless of version or failure holdoff. */
    tmp = strstr(cmd, "fota");
    if (tmp) {
        if (tmp[4] == '=') {
            char ver[16];
            int n = 0;
            tmp += 5;
            while (tmp[n] && tmp[n] != ',' && tmp[n] != ' ' &&
                   n < (int)sizeof(ver) - 1) {
                ver[n] = tmp[n];
                n++;
            }
            ver[n] = '\0';
            fota_notify_available(ver);
        } else {
            fota_request_check();
            alert_enqueue("fota: check queued", 0);
        }
    }

    if (strstr(cmd, "config")) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "fw=%s int=%d ma=%d tm=%d bat=%.1fV ign=%s up=%llus",
                 fota_version(),
                 g_settings.loop_interval,
                 (int)g_settings.movement_alarm,
                 (int)g_settings.track_mode,
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
