/*
 * Commands that answer with a report: netscan, selftest, status, gnssstat,
 * backlog, logdump.
 *
 * cmd_run() only asks (job_request); jobs_service() runs them from the main
 * loop's safe points, the same two places a reboot is acted on, so a sweep
 * or a flash probe never runs under a state handler.  Each answer is one
 * short line sent as its own datagram:
 *
 *     F,rep,<name>,<text>        (netscan: F,scan,<n>,<cells>)
 *
 * The server files the newest of each per device.  Nothing here waits for a
 * GNSS fix or anything else slow; the self-test in particular only looks at
 * what is already there.
 */

#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "hw_domain.h"
#if IS_ENABLED(CONFIG_APP_FLASH_BACKLOG)
#include "extflash.h"
#endif

LOG_MODULE_REGISTER(reports, CONFIG_APP_LOG_LEVEL);

static uint32_t s_pending;      /* bit per enum job */

void job_request(enum job j)
{
    s_pending |= 1U << j;
}

static void report(const char *name, const char *text)
{
    char line[300];

    snprintf(line, sizeof(line), "F,rep,%s,%s", name, text);
    if (data_send_line(line) != 0) {
        LOG_WRN("%s report not sent", name);
    }
}

static void do_status(void)
{
    char t[240];
    int64_t up = k_uptime_get() / 1000;
    const char *rst = dbglog_reset_cause();

    snprintf(t, sizeof(t),
             "fw=%s bd=%s up=%llu rst=%s bat=%.1f ign=%d reg=%d "
             "rsrp=%d snr=%d band=%u cell=%u tac=%u plmn=%d-%02d "
             "int=%d backup=%d bl=%d",
             fota_version(), fota_board_id(), up, rst ? rst : "-",
             (double)battery_v, ignition_read(), modem_is_registered(),
             g_cell.signal_valid ? g_cell.rsrp_dbm : 0,
             g_cell.signal_valid ? g_cell.snr_db : 0,
             (unsigned)g_cell.band, (unsigned)g_cell.cid,
             (unsigned)g_cell.tac, g_cell.mcc, g_cell.mnc,
             g_settings.loop_interval, g_settings.backup_interval,
             databuf_count());
    report("status", t);
}

static void do_gnssstat(void)
{
    char t[200];
    int64_t age = g_gnss.valid
                      ? (k_uptime_get() - g_gnss.fix_uptime_ms) / 1000 : -1;
    const char *ver = gnss_module_version();

    snprintf(t, sizeof(t),
             "fix=%d age=%llds sats=%ld hdop=%ld.%ld blocked_ms=%u "
             "backup=%d mod=%s",
             g_gnss.valid, age, g_gnss.sats, g_gnss.hdop_x10 / 10,
             g_gnss.hdop_x10 % 10, (unsigned)gnss_blocked_ms(),
             hw_gnss_backup_wanted(), ver ? ver : "-");
    report("gnssstat", t);
}

static void do_backlog(bool flush)
{
    char t[120];
    int sent = 0;

    if (flush) {
        sent = databuf_flush(20);
    }
    snprintf(t, sizeof(t), "n=%d ram=%d dropped=%u unacked=%d%s%d",
             databuf_count(), databuf_ring_count(),
             (unsigned)databuf_dropped(), databuf_unacked(),
             flush ? " flushed=" : "", flush ? sent : 0);
    report("backlog", t);
}

static void do_logdump(void)
{
#if IS_ENABLED(CONFIG_APP_DEBUG_LOG)
    static char buf[600];

    buf[0] = '\0';
    int n = dbglog_take(buf, sizeof(buf));

    if (n <= 0) {
        report("logdump", "empty");
        return;
    }
    /* the captured lines are "L," lines already, one per row */
    buf[n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1] = '\0';
    dbglog_ack(data_send_line(buf) == 0);
#else
    report("logdump", "log capture off");
#endif
}

/* Light touch: only what is there now, nothing that waits.  Every item is
 * pass/fail/n.a. on one line; a failure also raises an alert. */
static void do_selftest(void)
{
    char t[240];
    int len = 0, fails = 0;

#define ITEM(name, ok, extra)                                              \
    do {                                                                   \
        len += snprintf(t + len, sizeof(t) - len, "%s%s=%s", len ? " " : "", \
                        name, (ok) ? "ok" : "FAIL");                       \
        if (!(ok)) { fails++; }                                            \
        extra;                                                             \
    } while (0)

    /* battery: a real reading, not an open or shorted divider */
    float v = battery_read_voltage();

    ITEM("bat", v > 1.0f && v < 40.0f,
         len += snprintf(t + len, sizeof(t) - len, "(%.1fV)", (double)v));

    /* accelerometer: present and answers a read */
    int ax = 0, ay = 0, az = 0;
    bool acc = accel_available() && accel_read(&ax, &ay, &az) == 0;

    ITEM("accel", acc, (void)0);

    /* modem: answers AT */
    char resp[32];

    ITEM("modem", modem_at("AT", resp, sizeof(resp)) == 0, (void)0);

    bool aux_held = false;

    /* Switched rails: one that is off is turned on, checked and turned off
     * again; one already on (in use) is only checked, never cycled.  A
     * board without the rail's enable pin skips it. */
    for (enum hw_domain d = 0; d < HW_DOMAIN_COUNT; d++) {
        if (!hw_domain_fitted(d)) {
            continue;
        }
        bool was_on = hw_domain_is_on(d);
        bool good;

        if (was_on) {
            good = !hw_domain_faulted(d);
        } else {
            good = hw_domain_request(d, HW_DOMAIN_USER_TEST) == 0 &&
                   !hw_domain_faulted(d);
            if (d == HW_DOMAIN_AUX && good) {
                aux_held = true;     /* the receiver is on it: look first */
            } else {
                hw_domain_release(d, HW_DOMAIN_USER_TEST);
            }
        }
        char nm[8];

        snprintf(nm, sizeof(nm), "%s", hw_domain_name(d));
        for (char *p = nm; *p; p++) {
            *p = (char)(*p | 0x20);
        }
        ITEM(nm, good, len += snprintf(t + len, sizeof(t) - len, "(%s)",
                                       was_on ? "on" : "cycled"));
    }

    /* GNSS: the receiver is talking.  No fix needed, none waited for */
    const char *ver = gnss_module_version();

    ITEM("gnss", ver != NULL && ver[0] != '\0',
         len += snprintf(t + len, sizeof(t) - len, "(sats=%ld)", g_gnss.sats));
    if (aux_held) {
        hw_domain_release(HW_DOMAIN_AUX, HW_DOMAIN_USER_TEST);
    }

#if IS_ENABLED(CONFIG_APP_FLASH_BACKLOG)
    uint32_t sectors = 0;
    int ferr = extflash_begin();

    if (ferr == 0) {
        ferr = extflash_probe(&sectors);
        extflash_end();
    }
    ITEM("flash", ferr == 0,
         len += snprintf(t + len, sizeof(t) - len, "(%d)", ferr));
#endif

    report("selftest", t);
    if (fails) {
        char a[48];

        snprintf(a, sizeof(a), "selftest: %d failed", fails);
        alert_enqueue(a, 0);
    }
}

void jobs_service(void)
{
    if (!s_pending) {
        return;
    }
    for (enum job j = JOB_NETSCAN; j <= JOB_LOGDUMP; j++) {
        if (!(s_pending & (1U << j))) {
            continue;
        }
        s_pending &= ~(1U << j);
        switch (j) {
        case JOB_NETSCAN: {
#if IS_ENABLED(CONFIG_APP_NETSCAN)
            static char line[480];
            int err = modem_net_scan(line, sizeof(line));

            if (err) {
                LOG_WRN("network scan failed: %d", err);
            } else if (data_send_line(line) != 0) {
                LOG_WRN("network scan result not sent");
            }
#endif
            break;
        }
        case JOB_SELFTEST:      do_selftest();      break;
        case JOB_STATUS:        do_status();        break;
        case JOB_GNSSSTAT:      do_gnssstat();      break;
        case JOB_BACKLOG:       do_backlog(false);  break;
        case JOB_BACKLOG_FLUSH: do_backlog(true);   break;
        case JOB_LOGDUMP:       do_logdump();       break;
        }
    }
}
