/*
 * Installation mode.
 *
 * A unit being fitted may have no 12 V and no antenna, so it must not assume
 * normal function and must not alert on what is just "not installed yet".
 * While the mode is on (the server's im=<0|1> on every reply; compiled default
 * APP_INSTALL_MODE_DEFAULT) the unit:
 *
 *  - tries for a modem connection forever and holds it (streaming: the socket
 *    and RRC connection stay up), never sleeps, never reboots for a failure;
 *  - sends a small status record every 10 s and reads the reply, which is how
 *    `instreport` and `instenable` reach it;
 *  - raises no alerts (alert_enqueue() drops them, see alert.c), except the
 *    one-line summary of an `instreport`;
 *  - on `instreport` runs a quick self-test and sends it as ir= on the next
 *    record, plus the summary alert (repeatable);
 *  - on `instenable` gets a GNSS fix (up to INSTALL_FIX_S, polling the server
 *    meanwhile), sends it with ie=1 and leaves the mode, or sends ie=0:<why>
 *    and stays.
 *
 * The mode survives a soft reset in a __noinit note (install_core.h); a power
 * loss or pin reset falls back to the compiled default until a reply says
 * otherwise.  Nothing here logs while it works: one line when the mode is
 * entered or left, and failures.  The pure parts are in install_core.h, checked
 * on the host (tests/test_install.py).
 */

#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "hw_domain.h"
#include "install.h"
#include "extflash.h"
#include "pins.h"

LOG_MODULE_REGISTER(install, CONFIG_APP_LOG_LEVEL);

#define POLL_MS          10000
#define INSTALL_FIX_S    120
#define V12_PRESENT_V    9.0f          /* below this the 12 V is not there */
#define HOLDOFF_MS       120000        /* after instenable: a stale im=1 must not re-enter */

static __noinit struct install_crumb s_crumb;
static bool     s_on;
static bool     s_report_req, s_enable_req;
static uint32_t s_part_failed;          /* bit per enum install_part */
static char     s_ir[INSTALL_IR_MAX_LEN + 1];   /* pending ir= value, "" none */
static char     s_ie[24];               /* pending ie= value, "" none */
static int64_t  s_holdoff_until_ms;

void install_init(void)
{
    s_on = install_crumb_load(&s_crumb, IS_ENABLED(CONFIG_APP_INSTALL_MODE_DEFAULT)) != 0;
    install_crumb_store(&s_crumb, s_on);
    if (s_on) {
        LOG_INF("installation mode on (at boot)");
    }
}

bool install_active(void)
{
    return s_on;
}

void install_set(bool on, const char *why)
{
    if (on == s_on) {
        return;
    }
    if (on && k_uptime_get() < s_holdoff_until_ms) {
        return;
    }
    s_on = on;
    install_crumb_store(&s_crumb, on);
    LOG_INF("installation mode %s (%s)", on ? "on" : "off", why);
    if (on) {
        g_settings.track_mode = 0;     /* ends do_track(); nothing else to stop here */
        alert_drop_queued();
    } else {
        s_report_req = s_enable_req = false;
        s_ir[0] = s_ie[0] = '\0';
    }
}

void install_note(enum install_part p, bool ok)
{
    if (ok) {
        s_part_failed &= ~BIT(p);
    } else {
        s_part_failed |= BIT(p);
    }
}

void install_take_token(char *cmd)
{
    int v = install_take_im(cmd);

    if (v >= 0) {
        install_set(v != 0, "server");
    }
}

void install_request_report(void) { s_report_req = true; }
void install_request_enable(void) { s_enable_req = true; }

int install_fields(char *buf, size_t size)
{
    int used = 0, n;

    if (!s_on) {
        return 0;
    }
    n = snprintf(buf, size, ",im=1");
    if (n < 0 || (size_t)n >= size) {
        return 0;
    }
    used = n;
    if (s_ir[0]) {
        n = snprintf(buf + used, size - used, ",ir=%s", s_ir);
        if (n > 0 && (size_t)n < size - used) {
            used += n;
        }
    }
    if (s_ie[0]) {
        n = snprintf(buf + used, size - used, ",ie=%s", s_ie);
        if (n > 0 && (size_t)n < size - used) {
            used += n;
        }
    }
    return used;
}

/* The record carried them (or the backlog has it): once each. */
void install_fields_taken(void)
{
    s_ir[0] = '\0';
    s_ie[0] = '\0';
}

/* -- the self-test --------------------------------------------------------- */

static int8_t state(bool fitted, bool ok)
{
    return !fitted ? INSTALL_NA : ok ? INSTALL_OK : INSTALL_FAIL;
}

/* Whether the receiver answers (not whether it has a fix).  The built-in one
 * has no way to say beyond its init; the u-blox says what it is. */
static bool gnss_answers(void)
{
    if (s_part_failed & BIT(INSTALL_PART_GNSS)) {
        return false;
    }
    if (IS_ENABLED(CONFIG_APP_BOARD_HAS_EXT_GNSS)) {
        /* It answers once its version is read; give a receiver that has only
         * just been powered a few seconds. */
        for (int i = 0; i < 30 && gnss_module_version()[0] == '\0'; i++) {
            watchdog_kick();
            k_msleep(100);
        }
        return gnss_module_version()[0] != '\0';
    }
    return true;
}

static int flash_state(void)
{
#if IS_ENABLED(CONFIG_APP_BOARD_HAS_EXT_FLASH)
    uint8_t id[3];

    extflash_park();                    /* reads the ID if nothing has yet */
    return extflash_boot_check(id) == 0 ? INSTALL_OK : INSTALL_FAIL;
#else
    return INSTALL_NA;
#endif
}

/* Runs the checks and fills the pending ir= text; raises the summary alert. */
static void do_report(void)
{
    signed char rail[HW_RAIL_COUNT];
    float v = hw_power_available() ? battery_read_voltage() : -1.0f;
    bool v12 = v >= V12_PRESENT_V;
    struct install_item it[] = {
        { "v12",    state(true, v12) },
        { "ign",    state(true, ignition_read() == 0) },
        { "gps3v3", 0 }, { "can3v3", 0 }, { "k3v3", 0 }, { "k12v", 0 },
        { "can",    state(IS_ENABLED(CONFIG_APP_BOARD_HAS_CAN), hw_can_available()) },
        { "imu",    state(true, accel_available()) },
        { "ina",    state(true, hw_power_available()) },
        { "modem",  state(true, modem_is_registered()) },
        { "gnss",   0 },
        { "flash",  (int8_t)flash_state() },
        { "backup", state(IS_ENABLED(CONFIG_APP_GNSS_BACKUP_SUPPLY) && PIN_GNSS_BCKP_EN >= 0,
                           hw_gnss_backup_is_on()) },
    };
    char msg[120];

    /* The rails are tested only with the 12 V there (a unit on the bench with
     * none cannot be told from a fault); otherwise they read "-".  Entries 2-5
     * are gps3v3 can3v3 k3v3 k12v in enum hw_rail's order, 10 is gnss. */
    for (int i = 0; i < HW_RAIL_COUNT; i++) {
        rail[i] = INSTALL_NA;
    }
    if (v12) {
        hw_rail_check_all(rail);
    }
    for (int i = 0; i < HW_RAIL_COUNT; i++) {
        it[2 + i].state = rail[i];
    }
    it[10].state = gnss_answers() ? INSTALL_OK : INSTALL_FAIL;

    install_ir_build(it, ARRAY_SIZE(it), s_ir, sizeof(s_ir));
    install_summary(it, ARRAY_SIZE(it), msg, sizeof(msg));
    alert_enqueue_install(msg, 0);
}

/* -- the poll -------------------------------------------------------------- */

/* One exchange with the server: a status record, the reply, any command in it.
 * Quiet unless something is wrong. */
static bool poll_once(bool gnss_busy)
{
    if (!modem_is_registered()) {
        if (modem_unregistered_s() < 0) {
            modem_radio_up();           /* the modem is off: the search is its own once on */
        }
        return false;
    }
    /* Held open so the next poll does not pay for a connection, except while
     * the built-in receiver is searching: an RRC connection that is held
     * leaves it no radio (the same blindness the drive stream has). */
    transport_set_streaming(!gnss_busy);
    stream_keeps_records = false;
    data_reset();
    ignition = (char)ignition_read();
    read_udp_response = true;
    if (collect_install_data() > 0) {
        send_data();
        data_reset();
        if (pending_server_cmd[0] != '\0') {
            cmd_run(pending_server_cmd);
            pending_server_cmd[0] = '\0';
        }
        if (alert_count > 0) {
            alert_send();
        }
        if (!last_send_ok) {
            modem_recover();
            return false;
        }
        return true;
    }
    return false;
}

/* Waits out the rest of a poll interval; ends early when the mode changes or a
 * command asks for work. */
static void wait_until(int64_t t_ms)
{
    while (s_on && !s_report_req && !s_enable_req && k_uptime_get() < t_ms) {
        watchdog_kick();
        jobs_service();
        if (power_reboot) {
            reboot_now();
        }
        k_msleep(100);
    }
}

/* instenable.  Returns when it has an outcome (or the mode ended). */
static void do_enable(void (*retry)(void))
{
    const bool builtin = !IS_ENABLED(CONFIG_APP_BOARD_HAS_EXT_GNSS);
    int64_t deadline = k_uptime_get() + (int64_t)INSTALL_FIX_S * 1000;
    int64_t next_poll = k_uptime_get();
    const char *fail = "nofix";
    struct gnss_fix fix = {0};
    bool got = false;

    if (s_part_failed & BIT(INSTALL_PART_GNSS)) {
        fail = "gnssfail";
        goto done;
    }
    if (builtin) {
        gnss_stop();
        gnss_start();
    }
    collect_fix_wait_ms = 1000;         /* running out of a wait is not a fault here */
    while (s_on && k_uptime_get() < deadline) {
        watchdog_kick();
        if (k_uptime_get() >= next_poll) {
            if (retry) {
                retry();
            }
            poll_once(builtin);
            if (s_report_req) {
                s_report_req = false;
                do_report();
            }
            next_poll = k_uptime_get() + POLL_MS;
            continue;
        }
        int wait = (int)MIN(next_poll - k_uptime_get(), 2000);

        fix.valid = false;
        int err = gnss_collect(wait, &fix);

        if (err == 0 && fix.valid) {
            g_gnss = fix;
            got = true;
            break;
        }
        if (err == -ENODEV || err == -EIO) {
            fail = "gnssfail";
            break;
        }
        if (err == -EINTR) {
            k_msleep(200);              /* the key turned: the wait ends at once */
        }
    }
    collect_fix_wait_ms = 0;
    if (builtin) {
        gnss_stop();
    }
    if (!s_on) {
        return;
    }
done:
    if (got) {
        snprintf(s_ie, sizeof(s_ie), "1");
    } else {
        snprintf(s_ie, sizeof(s_ie), "0:%s", fail);
    }
    /* The record that carries it must actually go: a link that is down for
     * the moment is waited out a little, and the outcome stays pending. */
    for (int i = 0; i < 6 && s_on && s_ie[0] && !poll_once(false); i++) {
        for (int w = 0; w < 50; w++) {
            watchdog_kick();
            k_msleep(100);
        }
    }
    s_enable_req = false;
    if (got && s_on && s_ie[0] == '\0') {
        s_holdoff_until_ms = k_uptime_get() + HOLDOFF_MS;
        install_set(false, "fix obtained");
    }
}

void install_run(void (*retry)(void))
{
    const bool builtin = !IS_ENABLED(CONFIG_APP_BOARD_HAS_EXT_GNSS);

    /* The built-in receiver and LTE share one radio: it stays off until a fix
     * is asked for. */
    if (builtin && !(s_part_failed & BIT(INSTALL_PART_GNSS))) {
        gnss_stop();
    }

    while (s_on) {
        int64_t t0 = k_uptime_get();

        watchdog_kick();
        if (retry) {
            retry();
        }
        poll_once(false);
        if (!s_on) {
            break;
        }
        if (s_report_req) {
            s_report_req = false;
            do_report();
            continue;                   /* the next record carries it, at once */
        }
        if (s_enable_req) {
            s_enable_req = false;
            do_enable(retry);
            continue;
        }
        wait_until(t0 + POLL_MS);
    }

    /* Back to the tracker: the connection goes back to the sends' own use and
     * the receiver to what the normal states expect of it. */
    transport_set_streaming(false);
    if (!(s_part_failed & BIT(INSTALL_PART_GNSS))) {
        gnss_resume();
    }
}
