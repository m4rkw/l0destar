/*
 * Server command dispatch.  Format: "key=value[,key=value...]".
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "hw_domain.h"

LOG_MODULE_REGISTER(cmd, CONFIG_APP_LOG_LEVEL);

/* locatenow and tomtomnow build and send a record before saying where the
 * unit is.  Awake with the ignition on, STATE_SEND has stopped GNSS for the
 * send this reply answers, so resume it first; otherwise the collection waits
 * out its timeout on a stopped receiver and reports the last known position.
 * A parked unit's timed report keeps using that position. */
static void locate_now(void)
{
    if (ignition == 0) {
        gnss_resume();
    }
    collect_data(ignition);
    send_data();
}

/* True for a command that is only the track=<0|1> (and gb=<0|1>) the server
 * puts on every reply (Track mode, below), saying what the device already
 * has. */
static bool track_echo(const char *cmd)
{
    if (strncmp(cmd, "track=", 6) != 0 ||
        (cmd[6] != '0' && cmd[6] != '1') ||
        (cmd[6] == '1') != (g_settings.track_mode != 0)) {
        return false;
    }
    if (cmd[7] == '\0') {
        return true;
    }
    /* ",gb=<0|1>", then the off window (",gw=...") if the server sends one. */
    return strncmp(cmd + 7, ",gb=", 4) == 0 &&
           (cmd[11] == '0' || cmd[11] == '1') &&
           (cmd[12] == '\0' || strncmp(cmd + 12, ",gw=", 4) == 0) &&
           (cmd[11] == '1') == hw_gnss_backup_wanted();
}

/* Names of the commands run since the last record carried them (ak=).  Small
 * on purpose: a few commands arrive between two records, and one that does
 * not fit is dropped rather than held. */
static char s_ack[48];

static void ack_add(const char *name)
{
    size_t have = strlen(s_ack), n = strlen(name);

    /* each name once, whole-word */
    for (const char *p = s_ack; *p; ) {
        const char *end = strchr(p, '|');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (len == n && strncmp(p, name, n) == 0) {
            return;
        }
        p += len + (end ? 1 : 0);
    }
    if (have + n + (have ? 1 : 0) >= sizeof(s_ack)) {
        return;
    }
    if (have) {
        s_ack[have++] = '|';
    }
    memcpy(&s_ack[have], name, n + 1);
}

int cmd_ack_text(char *buf, size_t size)
{
    if (!s_ack[0] || size <= strlen(s_ack)) {
        return 0;
    }
    strcpy(buf, s_ack);
    return (int)strlen(buf);
}

void cmd_ack_taken(void)
{
    s_ack[0] = '\0';
}

void cmd_run(char *cmd)
{
    char *tmp;
    long val;

    if (!track_echo(cmd)) {
        LOG_INF("cmd: %s", cmd);
    }

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
        ack_add("int");
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
        ack_add("movealarm");
    }

    /* Track mode.  The server puts track=<0|1> on every response so the
     * device converges on its setting after a reboot or a missed reply;
     * only a change is acted on, so the routine repeat is silent. */
    tmp = strstr(cmd, "track=");
    if (tmp && IS_ENABLED(CONFIG_APP_TRACK_MODE)) {
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
            ack_add("track");
        }
    }

    /* GNSS receiver backup supply: the server puts gb=<0|1> on every reply.
     * The first one after a boot is the unit catching up, applied silently
     * (acked only if it changed anything); a later change is the user's, so
     * it is announced, acked, and turning it on asks the next wake for a
     * fresh fix, since the receiver lost what the supply was keeping. */
    tmp = strstr(cmd, "gb=");
    if (tmp && (tmp == cmd || tmp[-1] == ',') &&
        (tmp[3] == '0' || tmp[3] == '1')) {
        static int8_t last = -1;
        int8_t want = tmp[3] == '1';
        bool was_on = hw_gnss_backup_is_on();
        bool changed = hw_gnss_backup_want(want);

        if (last < 0) {
            if (changed) {
                ack_add("gnssbk");
            }
        } else if (want != last && changed) {
            /* Only when the reply altered what the unit is doing: the supply
             * the ignition came on with is already on, and the server's
             * mirror of that arriving as gb=1 is not the user's change. */
            if (want) {
                request_gps_fix();
            }
            /* An OFF is the user's only if it dropped a supply that was
             * up: when the unit let it go itself (the cut-off hour, the
             * off window) the server's mirror of that arrives as gb=0 and
             * is not worth an alert. */
            if (want || was_on) {
                alert_enqueue(want ? "gnss backup supply ON"
                                   : "gnss backup supply OFF", 0);
            }
            ack_add("gnssbk");
        }
        last = want;
    }

    /* The off window, gw=<from>-<to> in UTC hours or gw=off: the supply is
     * kept off through it even when the unit is driven.  Sent on every reply
     * like gb=, applied silently. */
    /* The LED switch, le=<0|1>, rides every reply like gb= and gw=.  Matched at
     * the start of a token so that another key ending in "le" cannot trip it.
     * Silent: it is a setting, not an event. */
    tmp = strstr(cmd, "le=");
    while (tmp && tmp != cmd && tmp[-1] != ',') {
        tmp = strstr(tmp + 1, "le=");
    }
    if (tmp && (tmp[3] == '0' || tmp[3] == '1')) {
        led_enable(tmp[3] == '1');
    }

    tmp = strstr(cmd, "gw=");
    if (tmp && (tmp == cmd || tmp[-1] == ',')) {
        int from, to;

        if (strncmp(tmp + 3, "off", 3) == 0) {
            hw_gnss_backup_window(-1, -1);
        } else if (sscanf(tmp + 3, "%d-%d", &from, &to) == 2 &&
                   from >= 0 && from <= 23 && to >= 0 && to <= 23) {
            hw_gnss_backup_window(from, to);
        }
    }

    /* "gpsfix": search for a fresh fix at the next wake that has a network,
     * without touching the backup supply (turning it on does the same).  A
     * parked unit's other commands report the last position it has. */
    if (strstr(cmd, "gpsfix")) {
        if (ignition == 0) {
            /* Awake with the ignition on: the position is already live. */
            char msg[80];

            snprintf(msg, sizeof(msg), "apple: %s,%s",
                     g_gnss.lat_str, g_gnss.lon_str);
            alert_enqueue(msg, 0);
        } else {
            request_gps_fix();      /* answered when the search is done */
        }
        ack_add("gpsfix");
    }

    if (strstr(cmd, "movereset")) {
        movement_reset();
        alert_enqueue("movement alarm reset", 0);
        ack_add("movereset");
    }

    if (strstr(cmd, "locatenow")) {
        ack_add("locatenow");
        locate_now();
        char msg[60];
        snprintf(msg, sizeof(msg), "google: %s,%s",
                 g_gnss.lat_str, g_gnss.lon_str);
        alert_enqueue(msg, 0);
    } else if (strstr(cmd, "locate")) {
        ack_add("locate");
        char msg[60];
        snprintf(msg, sizeof(msg), "google: %s,%s",
                 g_gnss.lat_str, g_gnss.lon_str);
        alert_enqueue(msg, 0);
    }

    if (strstr(cmd, "tomtomnow")) {
        ack_add("tomtomnow");
        locate_now();
        char msg[120];
        snprintf(msg, sizeof(msg), "tomtom: %s,%s",
                 g_gnss.lat_str, g_gnss.lon_str);
        alert_enqueue(msg, 0);
    } else if (strstr(cmd, "tomtom")) {
        ack_add("tomtom");
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
        } else if (IS_ENABLED(CONFIG_APP_FOTA_INHIBIT)) {
            /* A bench build never checks, so do not claim one is queued. */
            alert_enqueue("fota: updates inhibited", 0);
        } else {
            fota_request_check();
            alert_enqueue("fota: check queued", 0);
            ack_add("fota");
        }
    }

    if (strstr(cmd, "config")) {
        ack_add("config");
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "fw=%s board=%s int=%d ma=%d tm=%d backup=%d bat=%.1fV "
                 "ign=%s up=%llus",
                 fota_version(), fota_board_id(),
                 g_settings.loop_interval,
                 (int)g_settings.movement_alarm,
                 (int)g_settings.track_mode,
                 g_settings.backup_interval,
                 (double)battery_v,
                 (ignition == 0) ? "on" : "off",
                 k_uptime_get() / 1000);
        alert_enqueue(msg, 0);
    }

    if (strstr(cmd, "netscan")) {
        job_request(JOB_NETSCAN);
        ack_add("netscan");
    }

    static const struct { const char *name; enum job job; } jobs[] = {
        { "selftest", JOB_SELFTEST }, { "status", JOB_STATUS },
        { "gnssstat", JOB_GNSSSTAT }, { "logdump", JOB_LOGDUMP },
    };

    for (size_t i = 0; i < ARRAY_SIZE(jobs); i++) {
        if (strstr(cmd, jobs[i].name)) {
            job_request(jobs[i].job);
            ack_add(jobs[i].name);
        }
    }
    if (strstr(cmd, "backlog")) {
        job_request(strstr(cmd, "backlog=flush") ? JOB_BACKLOG_FLUSH
                                                 : JOB_BACKLOG);
        ack_add("backlog");
    }

    tmp = strstr(cmd, "sleep=");
    if (tmp) {
        long secs = strtol(tmp + 6, NULL, 10);

        if (secs >= 10 && secs <= 86400) {
            sleep_once_request((int)secs);
            ack_add("sleep");
        } else {
            alert_enqueue("sleep= must be 10..86400 s", 0);
        }
    }

    if (strstr(cmd, "reboot")) {
        power_reboot = true;
        alert_enqueue("rebooting", 0);
    }
}
