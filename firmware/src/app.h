/*
 * Shared application state, types and inter-module function prototypes.
 * The original Arduino code kept these as file-scope globals across all .ino
 * files; here they live behind a single header included by every module.
 */

#ifndef APP_H_
#define APP_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

#include "config.h"

/* -- settings (was struct settings in firmware.ino) ------------------------ */
struct app_settings {
    char    apn[64];
    char    user[20];
    char    pwd[20];
    char    imei[20];
    int     loop_interval;
    int8_t  always_on;
    int8_t  movement_alarm;
    uint8_t psk[32];
};

extern struct app_settings g_settings;

/* -- GNSS fix exposed to telemetry ----------------------------------------- */
struct gnss_fix {
    bool    valid;
    int64_t fix_uptime_ms;
    char    lat_str[16];     /* preformatted "%.6f" — matches the legacy CSV */
    char    lon_str[16];
    float   speed_kmh;
    float   altitude_m;
    float   heading_deg;
    long    hdop_x10;        /* HDOP × 10 to match legacy gps_hdop scale */
    long    sats;
    char    time_iso[40];    /* "DD/MM/YY,HH:MM:SS.uuuuuu+00" */
};

extern struct gnss_fix g_gnss;

/* -- cell tower info ------------------------------------------------------- */
struct cell_info {
    int mcc;
    int mnc;
    uint32_t cid;
    uint32_t tac;
    bool valid;
    bool dirty;
};

extern struct cell_info g_cell;

/* -- shared runtime state -------------------------------------------------- */
extern char  data_current[DATA_LIMIT];
extern int   data_index;
extern char  pending_server_cmd[128];
extern bool  send_int_to_server;
extern bool  read_udp_response;
extern bool  last_send_ok;
extern bool  power_reboot;
extern int   gsm_send_failures;
extern bool  network_ready;
extern bool  use_cached_gps;
extern bool  powered_on;
extern char  ignition;
extern int8_t previous_ignition;
extern bool  engine_running;
extern float battery_v;

/* -- module init / lifecycle ----------------------------------------------- */
int  crypto_init(void);
int  crypto_random(uint8_t *out, size_t len);
int  crypto_psk_from_hex(const char *hex, uint8_t out[32]);

void settings_load(void);
void settings_print(void);

int  modem_init(void);
int  modem_connect(void);
int  modem_get_imei(char *out, size_t out_len);
int  modem_get_network_status(void);   /* 1=home, 5=roaming */
void modem_set_apn(const char *apn);
int  modem_at(const char *cmd, char *resp, size_t resp_len);
int  modem_recover(int failure_count);
int  modem_update_cell_info(void);

int  gnss_init(void);
int  gnss_start(void);
int  gnss_stop(void);
int  gnss_collect(int timeout_ms, struct gnss_fix *out);  /* blocking with timeout */
int  gnss_resume(void);   /* restart without resetting fix state (warm) */

int  transport_open(void);
void transport_close(void);
int  transport_send(const uint8_t *plaintext, size_t pt_len);
int  transport_recv_response(char *out_plaintext, size_t out_len, int timeout_ms);

int  collect_data(int ignition_state);
void data_reset(void);
int  send_data(void);

void cmd_run(char *cmd);

void alert_enqueue(const char *msg, int priority);
int  alert_send(void);
int  alert_send_standalone(void);
extern int  alert_count;

void led_on(void);
void led_off(void);
void led_toggle(void);

void watchdog_init(void);
void watchdog_kick(void);

void reboot_now(void);
void status_delay(long ms);   /* watchdog-aware sleep */

/* Hardware modules */
int  hw_gpio_init(void);
int  hw_power_init(void);
void hw_power_shutdown(void);
void hw_power_wake(void);
void hw_aux_power_on(void);
int  hw_accel_init(void);
int  relay_init(void);
int  relay_set(void);
int  relay_reset(void);
int  kline_init(void);
int  kline_self_test(void);
void kline_power_on(void);
void kline_power_off(void);
uint8_t kline_tx_rx_byte(uint8_t tx);

int   accel_read(int *ax, int *ay, int *az);
int   ignition_read(void);
float battery_read_voltage(void);

int  accel_read_baseline(void);
int  accel_confirm_movement(void);
void accel_get_movement_info(int *tilt_tenths, int *delta_mg);
int  accel_enable_wake_int(void);
int  accel_disable_wake_int(void);
void movement_reset(void);

#endif /* APP_H_ */
