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

static void lte_handler(const struct lte_lc_evt *evt)
{
    switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS:
        LOG_INF("nw reg status: %d", evt->nw_reg_status);
        if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
            evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
            s_connected = true;
            network_ready = true;
        } else {
            s_connected = false;
            network_ready = false;
        }
        break;
    case LTE_LC_EVT_RRC_UPDATE:
        LOG_DBG("RRC mode: %s",
                evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED
                    ? "Connected" : "Idle");
        break;
    case LTE_LC_EVT_CELL_UPDATE:
        LOG_INF("cell %u tac %u", evt->cell.id, evt->cell.tac);
        if (evt->cell.id != 0 && evt->cell.id != UINT32_MAX) {
            g_cell.cid = evt->cell.id;
            g_cell.tac = evt->cell.tac;
            g_cell.valid = true;
            g_cell.dirty = true;
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

    nrf_modem_at_printf("AT+CPSMS=0");
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

    for (int waited = 0; waited < timeout_s; waited++) {
        k_sleep(K_SECONDS(1));
        watchdog_kick();

        int reg = modem_get_network_status();
        if (reg == 1 || reg == 5) {
            s_connected = true;
            network_ready = true;
            LOG_INF("re-registered after %ds on %s", waited + 1, modem_rat());
            return 0;
        }
    }

    LOG_WRN("no registration %ds after re-scan", timeout_s);
    return -ETIMEDOUT;
}

int modem_connect(void)
{
    modem_set_apn(g_settings.apn);

    apply_link_settings();

    LOG_INF("connecting (this can take 30s+)...");
    int err = lte_lc_connect();
    if (err) {
        LOG_ERR("lte_lc_connect: %d", err);
        return err;
    }
    s_connected = true;
    network_ready = true;

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

int modem_update_cell_info(void)
{
    char resp[128];

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
        nrf_modem_lib_shutdown();
        k_msleep(1000);

        int err = nrf_modem_lib_init();

        if (err && err != -EALREADY) {
            LOG_ERR("modem reinit: %d", err);
            return err;
        }
        lte_lc_register_handler(lte_handler);
        modem_set_apn(g_settings.apn);
        watchdog_kick();
        err = lte_lc_connect();
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
        lte_lc_offline();
        k_msleep(2000);
        modem_set_apn(g_settings.apn);
        watchdog_kick();

        int err = lte_lc_connect();

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
