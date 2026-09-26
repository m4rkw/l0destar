/*
 * LTE modem: bring-up, cell info tracking, and progressive error recovery.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <modem/modem_key_mgmt.h>
#include <nrf_modem_at.h>
#include <modem/at_monitor.h>

#include "app.h"
#include "ca_cert.h"

LOG_MODULE_REGISTER(modem, CONFIG_APP_LOG_LEVEL);

static void rai_urc_handler(const char *notif)
{
    LOG_INF("RAI URC: %s", notif);
}
AT_MONITOR(rai_urc, "%RAI", rai_urc_handler, PAUSED);

struct cell_info g_cell;

static bool s_connected;

/* Uptime at which registration was last lost, 0 while registered or powered
 * off on purpose (modem_power_off()).  Losing the network is logged at WRN so
 * the outage and its length are in the captured log: a unit that sits silent
 * for a quarter of an hour while the modem searches would otherwise leave no
 * trace of why. */
static int64_t s_lost_ms;

/* Uptime at which the search now in progress began: the CFUN=1 that has not
 * registered yet, or the registration that was lost.  0 while registered or
 * powered off.  Distinct from s_lost_ms, which only ever marks a loss: a
 * bring-up that never registers has lost nothing, but it is searching all the
 * same, and searching is what main.c's APP_NETWORK_SEARCH_TIMEOUT bounds. */
static int64_t s_search_ms;

/* How long the last attach took: CFUN=1 to the registration that ended the
 * search, in milliseconds.  -1 when the search now in progress has not
 * finished (or never did).
 *
 * Taken from the registration event rather than from wait_for_registration()'s
 * return, because that wait polls at 1 Hz and sleeps a second before its first
 * look — so every attach reads as at least 1 s and is rounded to the second.
 * The event handler runs on the URC, so this is the modem's own timing.  It is
 * the figure worth having: an engine-off wake is dominated by the attach, and
 * without splitting it out a slow wake cannot be told from a slow send. */
static int32_t s_attach_ms = -1;

/* Given by the LTE event handler the moment registration arrives, so the
 * wait below is woken by the URC instead of polling for it. */
static K_SEM_DEFINE(s_reg_sem, 0, 1);

static void search_started(void)
{
    if (!s_connected) {
        s_search_ms = k_uptime_get();
        s_attach_ms = -1;
        /* A registration that beats this reset is not lost: the wait checks
         * the status once before it ever blocks. */
        k_sem_reset(&s_reg_sem);
    }
}

/* The search is over.  Idempotent: whichever of the event handler and the
 * polling wait notices first records the time, and the handler normally wins. */
static void search_ended(void)
{
    if (s_search_ms) {
        s_attach_ms = (int32_t)(k_uptime_get() - s_search_ms);
        s_search_ms = 0;
    }
}

int modem_attach_ms(void)
{
    return s_attach_ms;
}

#if IS_ENABLED(CONFIG_APP_PSM_SLEEP)
/* Uptime of the radio's last sign of life: a registration, an RRC change
 * either way, or a wake from PSM.  The sleep loop's PSM check measures its
 * window from here rather than from a deadline set on an earlier pass: that
 * deadline could fall due on the pass of the next timed report, a second
 * after the send had woken the modem out of PSM, and it then powered off a
 * modem that PSM was working on (bench, 2026-09-25: one wake in four). */
static int64_t s_radio_active_ms;

static void radio_active(void)
{
    s_radio_active_ms = k_uptime_get();
}

int64_t modem_radio_quiet_ms(void)
{
    return k_uptime_get() - s_radio_active_ms;
}
#else
static inline void radio_active(void) { }
#endif

#if IS_ENABLED(CONFIG_APP_PSM_PROBE)
/* The network's answer, as last reported.  active_time < 0 is the library's
 * "PSM is deactivated" — which is also what a deliberate CFUN=0 produces, so
 * s_psm_offline says which of the two we are looking at. */
static int  s_psm_active_s = -1;
static int  s_psm_tau_s = -1;
static bool s_psm_offline;

/* The modem's own account of whether it is in PSM, from
 * LTE_LC_EVT_MODEM_SLEEP_ENTER/EXIT.  A modem that stays registered without
 * ever entering PSM looks identical from telemetry — fast wake, no attach —
 * and draws milliamps instead of microamps, so nothing infers this. */
static bool s_psm_asleep;

bool modem_psm_granted(void)
{
    return s_psm_active_s >= 0;
}

bool modem_psm_asleep(void)
{
    return s_psm_asleep;
}

/* What the network said about PSM, once per answer.
 *
 * At WRN rather than INF so it reaches the server: a deployed unit has no
 * console, and dbglog carries warnings out with the next record that gets
 * through.  Repeats are demoted to INF — the same answer comes back on every
 * attach, and one line an hour saying nothing has changed would drown the
 * log it is supposed to inform. */
static void psm_report(const struct lte_lc_psm_cfg *cfg)
{
    static bool seen;
    bool changed = !seen || cfg->tau != s_psm_tau_s ||
                   cfg->active_time != s_psm_active_s;

    seen = true;
    s_psm_tau_s = cfg->tau;
    s_psm_active_s = cfg->active_time;

    if (!changed) {
        LOG_INF("PSM unchanged: tau=%ds active=%ds",
                cfg->tau, cfg->active_time);
        return;
    }

    /* -1 means PSM is deactivated — but so does every deliberate CFUN=0,
     * which deregisters and takes the PSM config with it.  Reporting that
     * as a refusal put a warning an hour into the captured log saying the
     * network had turned us down, seconds after it had granted us: the
     * same mistake as reading a modem powered off on purpose as a lost
     * registration.  A power-off is logged at INF and says what it is. */
    if (cfg->active_time < 0) {
        if (s_psm_offline) {
            LOG_INF("PSM cleared by the modem going offline");
        } else {
            LOG_WRN("PSM: refused (active_time=-1, tau=%ds) — every wake "
                    "keeps paying a full attach", cfg->tau);
        }
        s_psm_asleep = false;
    } else {
        LOG_WRN("PSM: granted, active=%ds tau=%ds — a wake can resume "
                "instead of re-attaching", cfg->active_time, cfg->tau);
    }
}

#if IS_ENABLED(CONFIG_APP_PSM_SLEEP)
/* Proof, not inference: the modem says when it has actually gone to sleep
 * and what kind.  The first PSM entry is a WRN so it reaches the server's
 * log as evidence the adoption works; the rest are INF, since one line an
 * hour saying the same thing would bury what it was meant to show. */
static void sleep_report(const struct lte_lc_evt *evt, bool entering)
{
    static bool proved;
    enum lte_lc_modem_sleep_type type = evt->modem_sleep.type;

    if (entering) {
        s_psm_asleep = (type == LTE_LC_MODEM_SLEEP_PSM ||
                        type == LTE_LC_MODEM_SLEEP_PROPRIETARY_PSM);
        if (s_psm_asleep && !proved) {
            proved = true;
            LOG_WRN("modem entered PSM (type %d, %lld ms) — registered and "
                    "drawing what CFUN=0 would", (int)type,
                    evt->modem_sleep.time);
        } else {
            LOG_INF("modem sleep enter: type %d, %lld ms",
                    (int)type, evt->modem_sleep.time);
        }
    } else {
        s_psm_asleep = false;
        radio_active();
        LOG_INF("modem sleep exit: type %d", (int)type);
    }
}
#endif
#endif

#if IS_ENABLED(CONFIG_APP_NCELLMEAS)
/* Defined with the scan that triggers it, further down; the handler is where
 * its result arrives. */
static void ncell_report(const struct lte_lc_cells_info *info);
#endif

static void lte_handler(const struct lte_lc_evt *evt)
{
    switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS: {
        bool registered =
            evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
            evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING;

        if (registered && !s_connected && s_lost_ms) {
            LOG_WRN("registered again after %lld s (status %d)",
                    (k_uptime_get() - s_lost_ms) / 1000,
                    evt->nw_reg_status);
            s_lost_ms = 0;
        } else if (!registered && s_connected) {
            s_lost_ms = k_uptime_get();
            s_search_ms = s_lost_ms;
            LOG_WRN("registration lost (status %d) — waiting for the modem",
                    evt->nw_reg_status);
        } else {
            LOG_INF("nw reg status: %d", evt->nw_reg_status);
        }
        s_connected = registered;
        network_ready = registered;
        if (registered) {
            /* Timed before the semaphore is given, so a waiter that wakes
             * on it already sees the finished figure. */
            search_ended();
            radio_active();
            k_sem_give(&s_reg_sem);
        }
        break;
    }
    case LTE_LC_EVT_RRC_UPDATE:
        /* Either way: going idle is where the active timer starts, and PSM
         * is due that long after it. */
        radio_active();
        LOG_DBG("RRC mode: %s",
                evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED
                    ? "Connected" : "Idle");
        break;
#if IS_ENABLED(CONFIG_APP_NCELLMEAS)
    case LTE_LC_EVT_NEIGHBOR_CELL_MEAS:
        ncell_report(&evt->cells_info);
        break;
#endif
#if IS_ENABLED(CONFIG_APP_PSM_PROBE)
    case LTE_LC_EVT_PSM_UPDATE:
        psm_report(&evt->psm_cfg);
        break;
#endif
#if IS_ENABLED(CONFIG_APP_PSM_SLEEP)
    case LTE_LC_EVT_MODEM_SLEEP_ENTER:
        sleep_report(evt, true);
        break;
    case LTE_LC_EVT_MODEM_SLEEP_EXIT:
        sleep_report(evt, false);
        break;
#endif
    case LTE_LC_EVT_CELL_UPDATE:
        LOG_INF("cell %u tac %u", evt->cell.id, evt->cell.tac);
        if (evt->cell.id != 0 && evt->cell.id != UINT32_MAX) {
            g_cell.cid = evt->cell.id;
            g_cell.tac = evt->cell.tac;
            g_cell.valid = true;
            g_cell.dirty = true;
            /* Whatever signal we hold was measured on the cell we have just
             * left.  Dropped rather than carried over: this flag raises
             * dirty, and dirty is what puts the cell group on the next
             * record, so a stale figure would ride out as that record's own
             * measurement.  The next modem_read_signal() replaces it. */
            g_cell.signal_valid = false;
        }
        break;
    default:
        break;
    }
}

int modem_init(void)
{
    int err = nrf_modem_lib_init();
    if (err && err != -EALREADY) {
        LOG_ERR("nrf_modem_lib_init: %d", err);
        return err;
    }
    lte_lc_register_handler(lte_handler);
    LOG_INF("init ok");
    return 0;
}

static int provision_fota_tag(void);

int modem_provision_tls(void)
{
    int err;
    bool exists;

    err = modem_key_mgmt_exists(TLS_SEC_TAG,
                                MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                                &exists);
    if (err) {
        LOG_ERR("key_mgmt_exists: %d", err);
        return err;
    }
    if (exists) {
        LOG_INF("TLS CA already provisioned (sec_tag %d)", TLS_SEC_TAG);
        return provision_fota_tag();
    }

    err = modem_key_mgmt_write(TLS_SEC_TAG,
                               MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                               ca_cert_pem, sizeof(ca_cert_pem) - 1);
    if (err) {
        LOG_ERR("key_mgmt_write: %d", err);
        return err;
    }
    LOG_INF("TLS CA provisioned (sec_tag %d)", TLS_SEC_TAG);
    return provision_fota_tag();
}

/* The FOTA HTTPS fetch gets its own sec_tag with the same CA.  Sharing
 * TLS_SEC_TAG broke in the field: DTLS/PSK experiments left extra credential
 * types on tag 1 in modem NVM, and a tag whose contents mix PSK and CA
 * entries makes a certificate-mode TLS connect() fail with EINVAL before any
 * packet is sent.  A dedicated tag holds exactly one CA chain and nothing
 * else, and this provisioning (pre-CFUN=1, from modem_provision_tls) keeps
 * it that way. */
static int provision_fota_tag(void)
{
#if IS_ENABLED(CONFIG_APP_FOTA) && CONFIG_APP_FOTA_SEC_TAG >= 0
    int err;
    bool exists;

    if (CONFIG_APP_FOTA_SEC_TAG == TLS_SEC_TAG) {
        return 0;
    }

    err = modem_key_mgmt_exists(CONFIG_APP_FOTA_SEC_TAG,
                                MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                                &exists);
    if (err) {
        LOG_ERR("fota key_mgmt_exists: %d", err);
        return err;
    }
    if (exists) {
        return 0;
    }

    err = modem_key_mgmt_write(CONFIG_APP_FOTA_SEC_TAG,
                               MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
                               ca_cert_pem, sizeof(ca_cert_pem) - 1);
    if (err) {
        LOG_ERR("fota key_mgmt_write: %d", err);
        return err;
    }
    LOG_INF("FOTA CA provisioned (sec_tag %d)", CONFIG_APP_FOTA_SEC_TAG);
#endif
    return 0;
}

/* Everything that has to be in place before CFUN=1.  Factored out because
 * modem_rescan_plmn() drops to CFUN=4 and back, which loses the session these
 * set up — a re-scan that skipped them would come back registered but without
 * RAI, quietly costing the GNSS duty cycle the whole design depends on. */
static void apply_link_settings(void)
{
    /* +COPS selection mode persists in modem NVM across power cycles; force
     * automatic PLMN selection in case a manual selection was ever stored. */
    nrf_modem_at_printf("AT+COPS=0");

#if IS_ENABLED(CONFIG_APP_PSM_PROBE)
    /* Ask, so the network has something to answer.  This does not adopt PSM:
     * the wake powers the modem off at CFUN=0 within a second or two of its
     * send, long before the requested active time could expire, so PSM never
     * engages and neither sleep current nor wake behaviour moves.  All it
     * buys is LTE_LC_EVT_PSM_UPDATE telling us whether adoption is possible.
     * Set before CFUN=1, like everything else here — +CPSMS after attach
     * would not reach this registration. */
    {
        int psm_err = lte_lc_psm_param_set_seconds(CONFIG_APP_PSM_TAU_S,
                                                   CONFIG_APP_PSM_ACTIVE_S);
        if (psm_err) {
            LOG_WRN("PSM params rejected: %d", psm_err);
        } else {
            psm_err = lte_lc_psm_req(true);
            if (psm_err) {
                LOG_WRN("PSM request failed: %d", psm_err);
            }
        }
    }
#else
    nrf_modem_at_printf("AT+CPSMS=0");
#endif
    nrf_modem_at_printf("AT%%XEDRX=0");
    nrf_modem_at_printf("AT+CEDRXS=0,4");

    /* Enable Rel-14 features (incl. RAI) and request RAI URC on registration.
     * Both must be issued before CFUN=1 (which lte_lc_connect does). */
    char at_resp[64];
    int at_err;
    at_err = nrf_modem_at_cmd(at_resp, sizeof(at_resp),
                              "AT%%REL14FEAT=1,1,1,1,1");
    if (at_err) {
        LOG_WRN("%%REL14FEAT: %d", at_err);
    }
    at_err = nrf_modem_at_cmd(at_resp, sizeof(at_resp), "AT%%RAI=2");
    if (at_err) {
        LOG_WRN("%%RAI=2: %d", at_err);
    } else {
        at_monitor_resume(&rai_urc);
    }
}

/* Access technology actually in use, for the telemetry rat= field and the
 * FOTA gate.  This used to be hardcoded "CATM1", which hid the one thing that
 * would have explained a run of failed downloads. */
const char *modem_rat(void)
{
    enum lte_lc_lte_mode mode;

    if (lte_lc_lte_mode_get(&mode) != 0) {
        return "UNKNOWN";
    }
    switch (mode) {
    case LTE_LC_LTE_MODE_LTEM:  return "CATM1";
    case LTE_LC_LTE_MODE_NBIOT: return "NBIOT";
    default:                    return "UNKNOWN";
    }
}

bool modem_is_nbiot(void)
{
    enum lte_lc_lte_mode mode;

    return lte_lc_lte_mode_get(&mode) == 0 && mode == LTE_LC_LTE_MODE_NBIOT;
}

/* Wait for registration by polling, feeding the watchdog every second.
 *
 * Never lte_lc_connect(): it ends in a semaphore take of up to
 * CONFIG_LTE_NETWORK_TIMEOUT (600 s by default, and this build does not
 * override it) with nothing feeding the watchdog and nothing else in the
 * main loop running — no ignition read, no accelerometer service, no
 * telemetry, no console output.  A unit that entered it in bad coverage
 * looked wedged from outside and stayed that way, because the watchdog it
 * was outlasting had no teeth either.
 *
 * Returns the seconds waited, or -ETIMEDOUT with the radio left searching:
 * the caller's loop is a better place to keep waiting than this one. */
static int wait_for_registration(int timeout_s)
{
    int64_t start = k_uptime_get();

    /* Once, before blocking: the URC can land between CFUN=1 and this call,
     * and on a modem that was already registered there is nothing to wait
     * for at all. */
    int reg = modem_get_network_status();

    if (reg == 1 || reg == 5) {
        s_connected = true;
        network_ready = true;
        search_ended();
        return 0;
    }

    /* Woken by the event handler rather than polling.  The poll this
     * replaced slept a whole second before its first look and stepped in
     * seconds after that, so every attach cost at least 1 s of the wake
     * however fast the modem actually was — a quarter of a 4 s wake, and
     * the dominant cost if PSM ever removes the attach.  The slices below
     * exist only to feed the watchdog; a registration arriving inside one
     * ends the wait immediately. */
    for (int left = timeout_s; left > 0; left -= REG_WAIT_SLICE_S) {
        int slice = MIN(left, REG_WAIT_SLICE_S);

        if (k_sem_take(&s_reg_sem, K_SECONDS(slice)) == 0) {
            /* The handler has already set the flags and timed the attach. */
            return (int)((k_uptime_get() - start) / 1000);
        }
        watchdog_kick();
    }
    return -ETIMEDOUT;
}

/* Drop the link and make the modem choose a cell/PLMN from scratch.
 *
 * A stationary unit can sit on one marginal cell for hours: telemetry is a few
 * hundred bytes and goes through, while a multi-minute bulk transfer on the
 * same cell fails every time.  Reselection only happens on its own terms, so
 * the escape is to force it — CFUN=4 then CFUN=1 with automatic selection.
 * This is the automated form of power-cycling the unit, which is what made a
 * stuck download complete on the bench.
 *
 * Registration is polled rather than waited on with lte_lc_connect(): the task
 * watchdog is a 32 s window and a blocking connect can outlast it. */
int modem_rescan_plmn(int timeout_s)
{
    LOG_INF("forcing PLMN re-scan (was %s)", modem_rat());

    s_connected = false;
    network_ready = false;

    int err = lte_lc_offline();
    if (err) {
        LOG_WRN("lte_lc_offline: %d", err);
    }
    k_msleep(500);

    apply_link_settings();

    err = lte_lc_normal();
    if (err) {
        LOG_ERR("lte_lc_normal: %d", err);
        return err;
    }
    search_started();

    int waited = wait_for_registration(timeout_s);

    if (waited < 0) {
        LOG_WRN("no registration %ds after re-scan", timeout_s);
        return -ETIMEDOUT;
    }
    LOG_INF("re-registered after %ds on %s", waited, modem_rat());
    return 0;
}

/* Reapply everything the radio needs and take it out of offline, without
 * waiting to see whether it registers.
 *
 * Split out because a modem that has just been reinitialised — by the
 * fault handler's reset thread, or by modem_recover()'s last-resort restart
 * — comes back at CFUN=0 with none of this in place: no APN, no %REL14FEAT,
 * no %RAI, and +COPS wherever modem NVM left it.  Registration alone would
 * then come back without RAI, quietly costing the GNSS duty cycle the whole
 * design depends on.  Idempotent, so a caller that is not sure whether the
 * radio needs it can just call it. */
int modem_radio_up(void)
{
    /* Only a modem that is actually down needs this.  One already in
     * normal mode is registered or searching, and every setting below is
     * in place — we are the only ones who ever set CFUN=1, and always
     * after applying them.  Reapplying is not free: +CGDCONT is refused
     * while attached, and +COPS=0 makes the modem start PLMN selection
     * over even in automatic mode (Nordic's own advice for leaving an
     * unwanted network), so a bring-up issued a few seconds into a
     * routine cell change (status 4 while it reselects) is the likeliest
     * reason one on 2026-09-19 at 10:43 became a 63 s outage, and a 30 s
     * hole followed the one at 11:20; blips left alone that day cost
     * under 15 s.  The one case this is for — the fault handler's reinit,
     * a power-off — leaves the modem at CFUN=0, and that still gets the
     * full treatment.  A modem that is up but not registering is left to
     * its own periodic search. */
    enum lte_lc_func_mode mode;

    if (lte_lc_func_mode_get(&mode) == 0 &&
        mode == LTE_LC_FUNC_MODE_NORMAL) {
        LOG_INF("radio already up — leaving the search to the modem");
        return 0;
    }

#if IS_ENABLED(CONFIG_APP_PSM_PROBE)
    s_psm_offline = false;
#endif

    modem_set_apn(g_settings.apn);

    apply_link_settings();

    int err = lte_lc_normal();

    if (err) {
        LOG_ERR("lte_lc_normal: %d", err);
        return err;
    }
    search_started();
    return 0;
}

/* Power the modem off on purpose: the end of a timed wake, the way into
 * sleep, the moment before a FOTA reboot.  The modem answers CFUN=0 with a
 * not-registered URC, and lte_handler cannot tell that from the network
 * going away — so every hourly wake in the field used to log "registration
 * lost" on the way down and "registered again after 3628 s" on the way back
 * up: an hour of sleep dressed up as an outage, drowning the real ones the
 * two warnings exist to show.  Clearing the state first makes the handler
 * take the URC as the plain status change it is, and leaves nothing for the
 * next registration to be measured against.
 *
 * A genuine outage that is still open gets its closing line here, so the
 * captured log never shows a loss without an end. */
/* The way into sleep for the radio.
 *
 * With PSM granted the modem is left registered: it falls into PSM on its
 * own once the active timer runs down, the network holds the context, and
 * the next wake resumes instead of re-attaching — which on this unit was
 * 2.2 s at best and 41 s at worst.  Its sleep current there is what CFUN=0
 * would have cost, which is the whole point of PSM.
 *
 * Without a grant this is the CFUN=0 it always was, so a network that
 * refuses costs nothing.  Whether the modem then really sleeps is not
 * assumed — see modem_psm_asleep(), and the fallback in do_sleep(). */
void modem_sleep(void)
{
#if IS_ENABLED(CONFIG_APP_PSM_SLEEP)
    if (modem_psm_granted() && s_connected) {
        LOG_INF("sleep: modem left registered (PSM, active %ds)",
                s_psm_active_s);
        return;
    }
#endif
    modem_power_off();
}

void modem_power_off(void)
{
    if (s_lost_ms) {
        LOG_WRN("powering off still unregistered, %lld s after losing "
                "the network", (k_uptime_get() - s_lost_ms) / 1000);
    }
    s_connected = false;
    s_lost_ms = 0;
    s_search_ms = 0;
    network_ready = false;
#if IS_ENABLED(CONFIG_APP_PSM_PROBE)
    /* So the PSM deactivation this causes is read as our own doing. */
    s_psm_offline = true;
    s_psm_asleep = false;
#endif

    int err = lte_lc_power_off();

    if (err) {
        LOG_WRN("lte_lc_power_off: %d", err);
    }
}

int modem_connect(void)
{
    LOG_INF("connecting (this can take 30s+)...");

    int err = modem_radio_up();

    if (err) {
        return err;
    }

    if (wait_for_registration(NETWORK_REGISTRATION_TIMEOUT) < 0) {
        LOG_WRN("no registration after %ds — radio left searching",
                NETWORK_REGISTRATION_TIMEOUT);
        return -ETIMEDOUT;
    }

    char resp[128];
    if (nrf_modem_at_cmd(resp, sizeof(resp), "AT+CEDRXRDP") == 0) {
        char *nl = strchr(resp, '\r');
        if (nl) *nl = '\0';
        LOG_INF("eDRX: %s", resp);
    }
    if (nrf_modem_at_cmd(resp, sizeof(resp), "AT+CPSMS?") == 0) {
        char *nl = strchr(resp, '\r');
        if (nl) *nl = '\0';
        LOG_INF("PSM: %s", resp);
    }

    LOG_INF("connected");
    return 0;
}

int modem_get_imei(char *out, size_t out_len)
{
    int err = nrf_modem_at_cmd(out, out_len, "AT+CGSN");
    if (err) {
        LOG_ERR("AT+CGSN: %d", err);
        return err;
    }
    char *eol = strchr(out, '\r');
    if (eol) *eol = '\0';
    return 0;
}

int modem_get_network_status(void)
{
    enum lte_lc_nw_reg_status status;
    int err = lte_lc_nw_reg_status_get(&status);
    if (err) return err;
    switch (status) {
    case LTE_LC_NW_REG_REGISTERED_HOME:    return 1;
    case LTE_LC_NW_REG_REGISTERED_ROAMING: return 5;
    default:                                return 0;
    }
}

void modem_set_apn(const char *apn)
{
    if (!apn || apn[0] == '\0') return;
    int err = nrf_modem_at_printf("AT+CGDCONT=0,\"IP\",\"%s\"", apn);
    if (err) LOG_WRN("CGDCONT: %d", err);
}

int modem_at(const char *cmd, char *resp, size_t resp_len)
{
    return nrf_modem_at_cmd(resp, resp_len, "%s", cmd);
}

/* One field of a %XMONITOR line, 0-based after the "%XMONITOR: " prefix, with
 * any surrounding quotes stripped.  Commas inside quotes do not separate
 * fields — operator names contain them — and empty fields are normal: the
 * modem leaves out whatever the network did not provide, and returns the
 * short form with nothing past the registration status when it is not
 * registered. */
static bool xmon_field(const char *line, int want, char *out, size_t out_sz)
{
    const char *p = strchr(line, ':');

    if (!p || out_sz == 0) {
        return false;
    }
    p++;
    while (*p == ' ') p++;

    const char *start = p;
    int idx = 0;
    bool quoted = false;

    for (;; p++) {
        if (*p == '"') {
            quoted = !quoted;
            continue;
        }
        if (*p == '\0' || *p == '\r' || *p == '\n') {
            break;
        }
        if (*p == ',' && !quoted) {
            if (idx == want) {
                break;
            }
            idx++;
            start = p + 1;
        }
    }
    if (idx != want) {
        return false;
    }

    size_t o = 0;

    for (const char *q = start; q < p && o < out_sz - 1; q++) {
        if (*q != '"') out[o++] = *q;
    }
    out[o] = '\0';
    return true;
}

#if IS_ENABLED(CONFIG_APP_CONN_EVAL)
/* The RSRQ index has half-dB steps, so it is carried in tenths rather than
 * rounded to a whole dB — the same trick the OBD fields use to keep a
 * decimal point off the wire.  Index 0 is "not used"; the mapping either
 * side of it is offset by one step, which is why this is not one line. */
static int16_t rsrq_idx_to_x10(int16_t idx)
{
    if (idx >= 1) {
        return (int16_t)(idx * 5 - 200);     /* 1 -> -19.5 dB, 34 -> -3.0 */
    }
    return (int16_t)(idx * 5 - 195);         /* -1 -> -20.0 dB */
}

/* Path loss, RSRQ, CE level and TX repetitions.  Failure is ordinary and
 * quiet: a positive return is the modem declining (4 is "radio busy", which
 * is what a drive with GNSS running answers most of the time), and the
 * %XMONITOR figures taken alongside stand without it. */
static void read_conn_eval(void)
{
    struct lte_lc_conn_eval_params p;

    g_cell.conn_valid = false;

    int err = lte_lc_conn_eval_params_get(&p);

    if (err) {
        LOG_DBG("conn eval unavailable (%d)", err);
        return;
    }

    g_cell.pathloss_db = p.dl_pathloss;
    g_cell.rsrq_x10 = (p.rsrq == LTE_LC_CELL_RSRQ_INVALID)
                          ? 0 : rsrq_idx_to_x10(p.rsrq);
    g_cell.ce_level = (int8_t)p.ce_level;
    g_cell.tx_rep = p.tx_rep;
    g_cell.conn_valid = true;

    LOG_INF("conn eval: pathloss=%ddB rsrq=%d.%ddB ce=%d txrep=%d",
            p.dl_pathloss, g_cell.rsrq_x10 / 10, abs(g_cell.rsrq_x10 % 10),
            (int)p.ce_level, p.tx_rep);
}
#endif

#if IS_ENABLED(CONFIG_APP_NCELLMEAS)
/* Neighbouring cells, measured once per wake.
 *
 * The scan is asynchronous — lte_lc_neighbor_cell_measurement() returns
 * immediately and the result arrives as an event — so this waits for it
 * rather than letting the wake power the modem off underneath the scan.
 *
 * What it is for: camped on -111 dBm while a neighbour reads -95 is a
 * selection problem wearing an antenna problem's clothes, and the two want
 * entirely different fixes.  The count matters as well as the levels — a
 * weak antenna pulls the serving cell and its neighbours down together, so
 * levels alone cannot separate it from a bad location, but marginal
 * neighbours fall below the detection floor and the list thins. */
static K_SEM_DEFINE(s_ncell_sem, 0, 1);

static void ncell_report(const struct lte_lc_cells_info *info)
{
    int16_t serving = info->current_cell.rsrp;
    int count = info->ncells_count;

    /* Timing advance is the distance to the serving tower, and the scan is
     * the only thing that reports it — %XMONITOR and a connection evaluation
     * both leave it out.  One TA step is 16 Ts, a 520.8 ns round trip, so
     * 78 m one way.
     *
     * It is what makes the path loss mean anything.  126 dB is unremarkable
     * for a tower two kilometres off and around 20 dB more than a tower at
     * 300 m should cost, and only the distance says which of those this is —
     * which is the whole question when a signal looks weak and the antenna
     * is suspect.  The modem answers it without any tower database, which
     * matters here: neither OpenCelliD nor Unwired Labs holds these cells,
     * since crowdsourced data comes from phones and phones never camp on an
     * LTE-M carrier.
     *
     * May be stale: it is only measured when the uplink transmits, so the
     * age goes out with it rather than being silently trusted. */
    uint16_t ta = info->current_cell.timing_advance;

    if (ta != LTE_LC_CELL_TIMING_ADVANCE_INVALID && ta <= LTE_LC_CELL_TIMING_ADVANCE_MAX) {
        int64_t age_ms = (int64_t)info->current_cell.measurement_time -
                         (int64_t)info->current_cell.timing_advance_meas_time;

        LOG_WRN("serving cell: ta=%u (~%u m), measured %llds before the "
                "scan", ta, (unsigned)(ta * 78),
                age_ms > 0 ? age_ms / 1000 : 0);
    } else {
        LOG_WRN("serving cell: no valid timing advance — distance unknown");
    }

    if (count == 0) {
        LOG_WRN("neighbours: none heard (serving %d dBm) — nothing else "
                "reaches this antenna", serving - 141);
    } else {
        int best = -1;
        int16_t best_rsrp = INT16_MIN;

        for (int i = 0; i < count; i++) {
            if (info->neighbor_cells[i].rsrp > best_rsrp) {
                best_rsrp = info->neighbor_cells[i].rsrp;
                best = i;
            }
        }
        LOG_WRN("neighbours: %d heard, serving %d dBm, best %d dBm "
                "(pci %u, earfcn %u)", count, serving - 141, best_rsrp - 141,
                info->neighbor_cells[best].phys_cell_id,
                info->neighbor_cells[best].earfcn);
    }
    k_sem_give(&s_ncell_sem);
}

static void read_neighbours(void)
{
    /* The default search type only measures the carrier already camped on,
     * so "none heard" from it means "no same-frequency neighbour" and not
     * "nothing out there" — a distinction worth having when the question is
     * whether the antenna is deaf.  The complete search sweeps properly, at
     * the cost of radio time, which is why it is a choice and not a
     * default. */
    struct lte_lc_ncellmeas_params params = {
        .search_type = IS_ENABLED(CONFIG_APP_NCELLMEAS_COMPLETE)
                           ? LTE_LC_NEIGHBOR_SEARCH_TYPE_EXTENDED_COMPLETE
                           : LTE_LC_NEIGHBOR_SEARCH_TYPE_DEFAULT,
    };

    k_sem_reset(&s_ncell_sem);

    int err = lte_lc_neighbor_cell_measurement(&params);

    if (err) {
        LOG_WRN("neighbour scan refused: %d", err);
        return;
    }

    /* Waited out in slices so the watchdog keeps being fed: a scan is
     * seconds and the window is 32. */
    for (int left = CONFIG_APP_NCELLMEAS_TIMEOUT_S; left > 0; left--) {
        if (k_sem_take(&s_ncell_sem, K_SECONDS(1)) == 0) {
            return;
        }
        watchdog_kick();
    }
    LOG_WRN("neighbour scan did not report in %ds",
            CONFIG_APP_NCELLMEAS_TIMEOUT_S);
    (void)lte_lc_neighbor_cell_measurement_cancel();
}
#endif

/* Serving-cell signal quality, from a single AT%XMONITOR.
 *
 * Without it a slow wake cannot be attributed: the attach is what varies, and
 * whether a 40 s attach was bad coverage or a busy cell is not answerable from
 * a cell id alone.  LTE-M raises its repetition count as coverage worsens, so
 * the same procedure on the same cell takes several times longer at a lower
 * RSRP — which is the hypothesis this makes testable.
 *
 * Both indices are 3GPP-mapped and the mapping is fixed by the spec, so they
 * are converted here and stored as dBm and dB rather than kept raw: unlike the
 * IMU's full-scale range, nothing in firmware can reinterpret them later.
 *   rsrp  1..97 -> -140..-44 dBm   (0 = below range, 255 = unknown)
 *   snr   1..49 ->  -24..+24 dB    (0 = below range, 127 = unknown)
 *
 * Failure is not an error worth logging loudly: an unregistered modem answers
 * with the short form and there is simply nothing to read.
 */
int modem_read_signal(void)
{
    char resp[320];
    char field[24];

    g_cell.signal_valid = false;

    if (nrf_modem_at_cmd(resp, sizeof(resp), "AT%%XMONITOR") != 0) {
        return -EIO;
    }

    /* Field 0 is the registration status; anything but registered means the
     * rest of the line is absent. */
    if (!xmon_field(resp, 0, field, sizeof(field))) {
        return -EINVAL;
    }

    int reg = atoi(field);

    if (reg != 1 && reg != 5) {
        return -ENOTCONN;
    }

    int band = 0, rsrp_idx = 255, snr_idx = 127;

    if (xmon_field(resp, 6, field, sizeof(field)) && field[0]) {
        band = atoi(field);
    }
    if (xmon_field(resp, 10, field, sizeof(field)) && field[0]) {
        rsrp_idx = atoi(field);
    }
    if (xmon_field(resp, 11, field, sizeof(field)) && field[0]) {
        snr_idx = atoi(field);
    }

    if (rsrp_idx < 1 || rsrp_idx > 97) {
        return -ENODATA;
    }

    g_cell.rsrp_dbm = (int16_t)(rsrp_idx - 141);
    g_cell.snr_db = (snr_idx >= 1 && snr_idx <= 49)
                        ? (int16_t)(snr_idx - 25) : 0;
    g_cell.band = (uint8_t)band;
    g_cell.signal_ms = k_uptime_get();
    g_cell.signal_valid = true;
    g_cell.dirty = true;

#if IS_ENABLED(CONFIG_APP_CONN_EVAL)
    read_conn_eval();
#endif
#if IS_ENABLED(CONFIG_APP_NCELLMEAS)
    read_neighbours();
#endif

    LOG_INF("signal: rsrp=%ddBm snr=%ddB band=%d on %s",
            g_cell.rsrp_dbm, g_cell.snr_db, g_cell.band, modem_rat());
    return 0;
}

int modem_update_cell_info(void)
{
    char resp[128];

    (void)modem_read_signal();

    int err = nrf_modem_at_printf("AT+COPS=3,2");
    if (err) {
        LOG_WRN("AT+COPS=3,2: %d", err);
        return err;
    }

    err = nrf_modem_at_cmd(resp, sizeof(resp), "AT+COPS?");
    if (err) {
        LOG_WRN("AT+COPS?: %d", err);
        return err;
    }

    char *q1 = strchr(resp, '"');
    if (!q1) return -EINVAL;
    char *q2 = strchr(q1 + 1, '"');
    if (!q2) return -EINVAL;

    int len = (int)(q2 - q1 - 1);
    if (len >= 5 && len <= 6) {
        char plmn[8];
        memcpy(plmn, q1 + 1, len);
        plmn[len] = '\0';
        g_cell.mcc = (plmn[0] - '0') * 100 +
                     (plmn[1] - '0') * 10 +
                     (plmn[2] - '0');
        g_cell.mnc = atoi(plmn + 3);
        LOG_INF("PLMN: mcc=%d mnc=%d", g_cell.mcc, g_cell.mnc);
    }

    return 0;
}

int modem_read_temp(float *temp_c)
{
    int raw;
    int ret = nrf_modem_at_scanf("AT%XTEMP?", "%%XTEMP: %d", &raw);
    if (ret != 1) {
        return -EIO;
    }
    *temp_c = (float)raw;
    return 0;
}

/* Supply voltage at the nRF9151 VDD pin, in millivolts.  Despite the command
 * name this is NOT the Connect Kit's battery connector: schematic sheet 4 ties
 * the SiP VDD to VSYS, which sheet 2 shows is the BQ25180's SYS output.  So
 * what this reads depends on which side of the charger's power path is
 * feeding SYS:
 *
 *   USB-C (VBUS) connected   SYS is regulated from VIN, and this returns the
 *                            BQ25180 SYS setpoint (~4.4-4.9V, per the ifmcu
 *                            shell's `charger sysreg`) no matter what the
 *                            battery connector is fed with.  The bench PSU on
 *                            VBAT goes to zero draw at the same time — that is
 *                            the power path handing over, not a fault.
 *   USB-C absent             SYS comes off BAT through the path FET, so this
 *                            tracks the battery-connector feed (the carrier's
 *                            4.2V buck) less a few mV of I*Rds — tens of mV
 *                            under LTE TX bursts.
 *
 * Either way it is not the vehicle battery; that one is battery_read_voltage()
 * on the INA228.  In telemetry the radio is up, so the modem returns the value
 * it last sampled during modem activity: a few seconds old, and biased towards
 * the loaded case — which is the interesting one for spotting a supply that
 * sags under TX. */
int modem_read_vbat(int *mv)
{
    int val;
    int ret = nrf_modem_at_scanf("AT%XVBAT", "%%XVBAT: %d", &val);
    if (ret != 1) {
        return -EIO;
    }
    *mv = val;
    return 0;
}

/* -- recovery ---------------------------------------------------------------
 *
 * A failed datagram says very little about the radio.  These are UDP sends
 * with no delivery guarantee, and the ordinary cause is simply that there is
 * no registration at that instant — which the LTE event handler already knows
 * accurately and asynchronously, without having to infer it.
 *
 * The modem's own firmware handles coverage loss, cell reselection and RAT
 * reselection on its own, exactly as a phone does.  Losing signal is a normal
 * condition it is built to ride out, so the right response to "no signal" is
 * to wait for it.
 *
 * Tearing the modem down is worse than doing nothing.  lte_lc_offline()
 * deregisters, and shutting the modem library down discards everything the
 * modem had learned about local cells, so recovery afterwards needs a full
 * band scan — minutes on LTE-M and NB-IoT, at the exact moment conditions are
 * already bad.  Repeated forced attaches can also trip the 3GPP backoff
 * timers (T3346/T3402) and earn a longer exclusion than the original outage.
 * A 12-minute hole in a drive on 2026-09-05, ending on NB-IoT, is what that
 * looks like from the outside.
 *
 * So the policy here is:
 *
 *   not registered      do nothing.  Drop the socket so a stale one is not
 *                       reused, and let the modem get on with it.
 *   registered, failing  the interesting case: the network says we are
 *                       attached but datagrams are not leaving.  Usually a
 *                       PDP context the network has silently deactivated.
 *                       Reopening the socket (which the transport does by
 *                       itself) fixes most of it.
 *   registered, failing  a CFUN cycle, then as a genuine last resort a
 *   for a long time     library restart.  Minutes, not seconds.
 *
 * GNSS is never touched.  It has no relationship to the radio, and stopping
 * it during a modem problem loses position for the whole outage — which is
 * the difference between a gap in the telemetry and a gap in the journey.
 */

/* When registered but unable to send, how long before escalating. */
#define MODEM_STUCK_CFUN_MS  ((int64_t)MODEM_STUCK_CFUN_S * 1000)
#define MODEM_STUCK_RESET_MS ((int64_t)MODEM_STUCK_RESET_S * 1000)

static int64_t s_failing_since;   /* first failure while registered, 0 = none */
static int64_t s_last_escalation; /* so escalations cannot repeat immediately */

bool modem_is_registered(void)
{
    return s_connected;
}

/* Seconds the radio has been up without a registration — since the bring-up
 * that has not registered yet, or since the registration that was lost — or
 * -1 while it is registered or powered off.  The 60 s modem_connect() waits
 * is part of it: the search starts at CFUN=1, not when the wait gives up.
 * main.c compares this against APP_NETWORK_SEARCH_TIMEOUT to decide when a
 * parked unit stops waiting for the network and sleeps. */
int modem_unregistered_s(void)
{
    if (s_connected || s_search_ms == 0) {
        return -1;
    }
    return (int)((k_uptime_get() - s_search_ms) / 1000);
}

/* Escalates and returns immediately: bringing the radio back up is CFUN=1
 * and nothing more, and STATE_IDLE is already polling for registration once
 * a second while it services the ignition line, the K-wire keep-alive and
 * the accelerometer.  Waiting for the network in here instead — which is
 * what lte_lc_connect() did — stopped all of that for as long as the
 * network stayed away. */
int modem_recover(void)
{
    /* Always drop the socket: it is cheap, it is local, and a stale one is
     * the single most likely reason a send fails while the link is fine. */
    transport_teardown();

    if (!modem_is_registered()) {
        if (s_failing_since) {
            LOG_INF("send failing because we are not registered — waiting");
            s_failing_since = 0;
        }
        return 0;
    }

    int64_t now = k_uptime_get();

    if (!s_failing_since) {
        s_failing_since = now;
        LOG_INF("registered but send failed — socket reopened");
        return 0;
    }

    int64_t failing_ms = now - s_failing_since;

    /* Do not escalate twice in quick succession: an attach takes time, and
     * the failures that arrive during it are not new information. */
    if (s_last_escalation && (now - s_last_escalation) < MODEM_STUCK_CFUN_MS) {
        return 0;
    }

    if (failing_ms >= MODEM_STUCK_RESET_MS) {
        LOG_WRN("registered but unable to send for %lld s — modem restart",
                failing_ms / 1000);
        s_last_escalation = now;
        s_failing_since = 0;
        s_connected = false;
        network_ready = false;
        nrf_modem_lib_shutdown();
        watchdog_kick();
        k_msleep(1000);

        int err = nrf_modem_lib_init();

        watchdog_kick();
        if (err && err != -EALREADY) {
            LOG_ERR("modem reinit: %d", err);
            return err;
        }
        lte_lc_register_handler(lte_handler);
        /* The whole bring-up, not a bare CFUN=1: a reinitialised modem
         * has none of the link settings, and registering without
         * %REL14FEAT and %RAI would quietly cost the GNSS duty cycle —
         * the same gap modem_rescan_plmn() closes after its CFUN=4. */
        err = modem_radio_up();
        if (err) {
            LOG_ERR("reconnect: %d", err);
            return err;
        }
        g_cell.dirty = true;
        return 0;
    }

    if (failing_ms >= MODEM_STUCK_CFUN_MS) {
        LOG_WRN("registered but unable to send for %lld s — CFUN cycle",
                failing_ms / 1000);
        s_last_escalation = now;
        s_connected = false;
        network_ready = false;
        lte_lc_offline();
        k_msleep(2000);
        watchdog_kick();

        int err = modem_radio_up();

        if (err) {
            LOG_ERR("reconnect: %d", err);
            return err;
        }
        g_cell.dirty = true;
        return 0;
    }

    return 0;
}

/* Called on a successful send: the link is working, so the stuck timer starts
 * again from nothing. */
void modem_send_ok(void)
{
    s_failing_since = 0;
}
