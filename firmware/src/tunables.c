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
#if IS_ENABLED(CONFIG_APP_TRACK_MODE)
#define BUILT_TRACK_MS CONFIG_APP_TRACK_PERIOD_MS
#else
#define BUILT_TRACK_MS 1000
#endif
#ifdef CONFIG_APP_OBD_ALERTS
#define BUILT_OBD_ALERTS CONFIG_APP_OBD_ALERTS
#else
#define BUILT_OBD_ALERTS ""
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
	[TUN_XTP] = BUILT_TRACK_MS,
	[TUN_XDD] = CONFIG_APP_DATABUF_DRAIN_S,
	[TUN_XNS] = CONFIG_APP_NETWORK_SEARCH_TIMEOUT,
	[TUN_XRP] = CONFIG_APP_RESP_POLL_S,
	[TUN_XLW] = IS_ENABLED(CONFIG_APP_LED_ACCEL_WAKE),
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

int tun_built_string(char *out, size_t cap)
{
	return tun_format_built(out, cap, s_built, BUILT_OBD_ALERTS);
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

	if (len == 1 && list[0] == '-') {
		len = 0;
	}
	for (int k = 0; k < TUN_N_INT; k++) {
		before[k] = get(k);
	}
	strncpy(oa_before, oa_get(&s_t), sizeof(oa_before) - 1);
	oa_before[sizeof(oa_before) - 1] = '\0';

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
}

int  tun_tow_tilt_deg(void)          { return get(TUN_XTT); }
int  tun_parked_impact_mg(void)      { return get(TUN_XPI); }
int  tun_impact_immediate_mg(void)   { return get(TUN_XII); }
int  tun_accel_alert_priority(void)  { return get(TUN_XAP); }
int  tun_accel_alert_backoff_s(void) { return get(TUN_XAB); }
const char *tun_obd_alerts(void)     { return oa_get(&s_t); }
int  tun_ignition_on_sleep_s(void)   { return get(TUN_XIS); }
int  tun_battery_warning_mv(void)    { return get(TUN_XBW); }
int  tun_battery_poweroff_mv(void)   { return get(TUN_XBP); }
bool tun_drive_stream(void)          { return get(TUN_XDS) != 0; }
int  tun_drive_stream_interval_s(void) { return get(TUN_XDI); }
int  tun_track_period_ms(void)       { return get(TUN_XTP); }
int  tun_databuf_drain_s(void)       { return get(TUN_XDD); }
int  tun_network_search_s(void)      { return get(TUN_XNS); }
int  tun_resp_poll_s(void)           { return get(TUN_XRP); }
bool tun_led_accel_wake(void)        { return get(TUN_XLW) != 0; }
