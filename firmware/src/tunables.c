/* Run-time overrides of build-time settings; see tunables.h. */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "tunables.h"
#include "tunables_core.h"

LOG_MODULE_REGISTER(tun, CONFIG_APP_LOG_LEVEL);

#if IS_ENABLED(CONFIG_APP_DRIVE_STREAM)
#define BUILT_DS_IV CONFIG_APP_DRIVE_STREAM_INTERVAL_S
#else
#define BUILT_DS_IV 0
#endif
#ifdef CONFIG_APP_DRIVE_STREAM_IMU_MS
#define BUILT_DS_IMU_MS CONFIG_APP_DRIVE_STREAM_IMU_MS
#else
#define BUILT_DS_IMU_MS 0
#endif
#if IS_ENABLED(CONFIG_APP_DRIVE_STREAM_ACCEL) || !defined(CONFIG_APP_DRIVE_STREAM)
#define BUILT_DS_ACCEL 1
#else
#define BUILT_DS_ACCEL 0
#endif
#if IS_ENABLED(CONFIG_APP_DRIVE_STREAM_IMPACT) || !defined(CONFIG_APP_DRIVE_STREAM)
#define BUILT_DS_IMPACT 1
#else
#define BUILT_DS_IMPACT 0
#endif
#ifdef CONFIG_APP_OBD_ALERTS
#define BUILT_OBD_ALERTS CONFIG_APP_OBD_ALERTS
#else
#define BUILT_OBD_ALERTS ""
#endif

#ifdef CONFIG_APP_OBD_PIDS
#define BUILT_OBD_PIDS CONFIG_APP_OBD_PIDS
#else
#define BUILT_OBD_PIDS "0C05"
#endif

#if IS_ENABLED(CONFIG_APP_WEAK_HOLD)
#define BUILT_WH_RSRP CONFIG_APP_WEAK_HOLD_RSRP_DBM
#else
#define BUILT_WH_RSRP 0
#endif

#if IS_ENABLED(CONFIG_APP_LOG_HOLD)
#define BUILT_LH_RSRP CONFIG_APP_LOG_HOLD_RSRP_DBM
#else
#define BUILT_LH_RSRP 0
#endif

static const int32_t s_built[TUN_N_INT] = {
	[TUN_XTT] = CONFIG_APP_TOW_TILT_DEG,
	[TUN_XPI] = CONFIG_APP_PARKED_IMPACT_MG,
	[TUN_XII] = CONFIG_APP_IMPACT_IMMEDIATE_MG,
	[TUN_XAP] = CONFIG_APP_ACCEL_ALERT_PRIORITY,
	[TUN_XAB] = CONFIG_APP_ACCEL_ALERT_BACKOFF_S,
	[TUN_XIS] = CONFIG_APP_IGNITION_ON_SLEEP_INTERVAL,
	[TUN_XBW] = CONFIG_APP_BATTERY_WARNING_MV,
	[TUN_XBP] = CONFIG_APP_BATTERY_POWEROFF_MV,
	[TUN_XDS] = IS_ENABLED(CONFIG_APP_DRIVE_STREAM),
	[TUN_XDI] = BUILT_DS_IV,
	[TUN_XIM] = BUILT_DS_IMU_MS,
	[TUN_XIA] = BUILT_DS_ACCEL,
	[TUN_XIC] = BUILT_DS_IMPACT,
	[TUN_XDD] = CONFIG_APP_DATABUF_DRAIN_S,
	[TUN_XNS] = CONFIG_APP_NETWORK_SEARCH_TIMEOUT,
	[TUN_XRP] = CONFIG_APP_RESP_POLL_S,
	[TUN_XLW] = IS_ENABLED(CONFIG_APP_LED_ACCEL_WAKE),
	[TUN_XWR] = BUILT_WH_RSRP,
	[TUN_XWS] = CONFIG_APP_WEAK_HOLD_SNR_DB,
	[TUN_XWM] = CONFIG_APP_WEAK_HOLD_MAX_S,
	[TUN_XSP] = IS_ENABLED(CONFIG_APP_SMART_POWER),
	[TUN_XSR] = CONFIG_APP_SMART_POWER_RSRP_DBM,
	[TUN_XSB] = CONFIG_APP_SMART_POWER_BATTERY_MV,
	[TUN_XSD] = CONFIG_APP_SMART_POWER_BATTERY_DROP_MV,
	[TUN_XSM] = CONFIG_APP_SMART_POWER_MAX_DEFER_S,
	[TUN_XLH] = BUILT_LH_RSRP,
};

static struct tun_table s_t;		/* empty: everything at its built value */

static int get(enum tun_key k)
{
	return (int)tun_table_get(&s_t, k, s_built);
}

static const char *oa_get(const struct tun_table *t)
{
	return t->oa_set ? t->oa : BUILT_OBD_ALERTS;
}

static const char *op_get(const struct tun_table *t)
{
	return t->op_set ? t->op : BUILT_OBD_PIDS;
}

int tun_built_string(char *out, size_t cap)
{
	return tun_format_built(out, cap, s_built, BUILT_OBD_ALERTS, BUILT_OBD_PIDS);
}

void tun_apply(const char *list)
{
	/* The value runs to the next ',' of the reply, or its end. */
	size_t len = 0;

	while (list[len] != '\0' && list[len] != ',' && list[len] != '\n') {
		len++;
	}

	struct tun_table n;
	int32_t before[TUN_N_INT];
	char oa_before[TUN_OA_MAX + 1];
	char op_before[TUN_OP_MAX + 1];

	if (len == 1 && list[0] == '-') {
		len = 0;
	}
	for (int k = 0; k < TUN_N_INT; k++) {
		before[k] = get(k);
	}
	strncpy(oa_before, oa_get(&s_t), sizeof(oa_before) - 1);
	oa_before[sizeof(oa_before) - 1] = '\0';
	strncpy(op_before, op_get(&s_t), sizeof(op_before) - 1);
	op_before[sizeof(op_before) - 1] = '\0';

	tun_table_apply(&n, list, len);

	/* One struct copy; readers are the main thread, as is this. */
	s_t = n;

	for (int k = 0; k < TUN_N_INT; k++) {
		int now = get(k);

		if (now != before[k]) {
			LOG_INF("tu: %s %d -> %d", tun_limits[k].code, before[k], now);
		}
	}
	if (strcmp(oa_before, oa_get(&s_t)) != 0) {
		LOG_INF("tu: xoa \"%s\" -> \"%s\"", oa_before, oa_get(&s_t));
	}
	if (strcmp(op_before, op_get(&s_t)) != 0) {
		LOG_INF("tu: xop \"%s\" -> \"%s\"", op_before, op_get(&s_t));
	}
}

int  tun_tow_tilt_deg(void)          { return get(TUN_XTT); }
int  tun_parked_impact_mg(void)      { return get(TUN_XPI); }
int  tun_impact_immediate_mg(void)   { return get(TUN_XII); }
int  tun_accel_alert_priority(void)  { return get(TUN_XAP); }
int  tun_accel_alert_backoff_s(void) { return get(TUN_XAB); }
const char *tun_obd_alerts(void)     { return oa_get(&s_t); }
const char *tun_obd_pids(void)       { return op_get(&s_t); }
int  tun_ignition_on_sleep_s(void)   { return get(TUN_XIS); }
int  tun_battery_warning_mv(void)    { return get(TUN_XBW); }
int  tun_battery_poweroff_mv(void)   { return get(TUN_XBP); }
bool tun_drive_stream(void)          { return get(TUN_XDS) != 0; }
int  tun_drive_stream_interval_s(void) { return get(TUN_XDI); }
int  tun_drive_stream_accel_ms(void) { return get(TUN_XIM); }
bool tun_drive_stream_accel(void)    { return get(TUN_XIA) != 0; }
bool tun_drive_stream_impact(void)   { return get(TUN_XIC) != 0; }
int  tun_databuf_drain_s(void)       { return get(TUN_XDD); }
int  tun_network_search_s(void)      { return get(TUN_XNS); }
int  tun_resp_poll_s(void)           { return get(TUN_XRP); }
bool tun_led_accel_wake(void)        { return get(TUN_XLW) != 0; }
int  tun_weak_hold_rsrp_dbm(void)    { return get(TUN_XWR); }
int  tun_weak_hold_snr_db(void)      { return get(TUN_XWS); }
int  tun_weak_hold_max_s(void)       { return get(TUN_XWM); }
bool tun_smart_power(void)           { return get(TUN_XSP) != 0; }
int  tun_smart_power_rsrp_dbm(void)  { return get(TUN_XSR); }
int  tun_smart_power_battery_mv(void) { return get(TUN_XSB); }
int  tun_smart_power_battery_drop_mv(void) { return get(TUN_XSD); }
int  tun_smart_power_max_defer_s(void) { return get(TUN_XSM); }
int  tun_log_hold_rsrp_dbm(void)     { return get(TUN_XLH); }
