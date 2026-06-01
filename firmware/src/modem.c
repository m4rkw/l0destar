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
#include <nrf_modem_at.h>

#include "app.h"

LOG_MODULE_REGISTER(modem, CONFIG_APP_LOG_LEVEL);

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

int modem_connect(void)
{
    modem_set_apn(g_settings.apn);

    nrf_modem_at_printf("AT+CPSMS=0");
    nrf_modem_at_printf("AT%%XEDRX=0");
    nrf_modem_at_printf("AT+CEDRXS=0,4");

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

int modem_recover(int failure_count)
{
    transport_close();

    if (failure_count >= GSM_ESCALATION_SLEEP) {
        LOG_WRN("modem power cycle (failures=%d)", failure_count);
        gnss_stop();
        nrf_modem_lib_shutdown();
        k_msleep(1000);
        int err = nrf_modem_lib_init();
        if (err && err != -EALREADY) {
            LOG_ERR("modem reinit: %d", err);
            return err;
        }
        lte_lc_register_handler(lte_handler);
        gnss_init();
        modem_set_apn(g_settings.apn);
        watchdog_kick();
        err = lte_lc_connect();
        if (err) {
            LOG_ERR("reconnect: %d", err);
            return err;
        }
        gnss_start();
        network_ready = true;
        g_cell.dirty = true;
        return 0;
    }

    if (failure_count >= GSM_ESCALATION_POWERCYCLE) {
        LOG_WRN("PDP reset (failures=%d)", failure_count);
        lte_lc_offline();
        k_msleep(2000);
        modem_set_apn(g_settings.apn);
        watchdog_kick();
        int err = lte_lc_connect();
        if (err) {
            LOG_ERR("reconnect: %d", err);
            return err;
        }
        network_ready = true;
        g_cell.dirty = true;
        return 0;
    }

    return 0;
}
