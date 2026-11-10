/*
 * Settings the server can change at run time (the "tu=" token of a reply).
 * Each accessor returns the override if the server has set one, else the
 * value the firmware was built with (its Kconfig).  The overrides live in RAM
 * only: after a reboot the built values hold until the first reply.
 * Parsing and limits: tunables_core.h.
 */
#ifndef TUNABLES_H
#define TUNABLES_H

#include <stdbool.h>
#include <stddef.h>

/* Make the overrides equal to a reply's tu= value (up to the next ',' or the
 * end of the string); one log line per effective value that changed. */
void tun_apply(const char *list);

int  tun_tow_tilt_deg(void);
int  tun_parked_impact_mg(void);
int  tun_impact_immediate_mg(void);
int  tun_accel_alert_priority(void);
int  tun_accel_alert_backoff_s(void);
const char *tun_obd_alerts(void);
int  tun_ignition_on_sleep_s(void);
int  tun_battery_warning_mv(void);
int  tun_battery_poweroff_mv(void);
bool tun_drive_stream(void);
int  tun_drive_stream_interval_s(void);
int  tun_track_period_ms(void);
int  tun_databuf_drain_s(void);
int  tun_network_search_s(void);
int  tun_resp_poll_s(void);
bool tun_led_accel_wake(void);

/* The built values as "tb=" text (tunables_core.h); returns the length or -1. */
int tun_built_string(char *out, size_t cap);

#endif
