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
int  crypto_encrypt(const uint8_t *pt, size_t pt_len,
                    const uint8_t *aad, size_t aad_len,
                    const uint8_t nonce[12],
                    uint8_t *out, size_t out_size, size_t *out_len);
int  crypto_decrypt(const uint8_t *ct, size_t ct_len,
                    const uint8_t *aad, size_t aad_len,
                    const uint8_t nonce[12],
                    uint8_t *out, size_t out_size, size_t *out_len);

void settings_load(void);
void settings_print(void);

int  modem_init(void);
int  modem_provision_tls(void);
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

int  agnss_init(void);
int  agnss_fetch(void *agnss_request);  /* NULL = request all; else nrf_modem_gnss_agnss_data_frame* */

int  transport_open(void);
void transport_close(void);
void transport_teardown(void);
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
void led_boot_animation(void);
void led_gps_searching(void);
void led_gps_fixed(void);
void led_sending(void);
void led_sent(void);
void led_idle(void);
void led_sleep_enter(void);
void led_all_off(void);

void watchdog_init(void);
void watchdog_kick(void);

void reboot_now(void);
void status_delay(long ms);   /* watchdog-aware sleep */

/* Hardware modules */
int  hw_gpio_init(void);
int  hw_power_init(void);
bool hw_power_available(void);
void hw_power_shutdown(void);
void hw_power_wake(void);
void hw_aux_power_on(void);
int  hw_accel_init(void);
bool accel_available(void);
int  relay_init(void);
int  relay_set(void);
int  relay_reset(void);
bool relay_available(void);
int  kline_init(void);
int  kline_self_test(void);
int  kline_test(void);
void kline_power_on(void);
void kline_power_off(void);
uint8_t kline_tx_rx_byte(uint8_t tx);

int  hw_can_test(void);

int   accel_read(int *ax, int *ay, int *az);
int   accel_read_gyro(int *gx, int *gy, int *gz);
int   accel_gyro_autozero(void);
int   accel_read_temp(float *temp_c);
int   modem_read_temp(float *temp_c);
int   ignition_read(void);
float battery_read_voltage(void);

int  accel_crash_int_enable(int threshold_mg);
int  accel_crash_int_disable(void);
int  accel_read_wake_src(uint8_t *src);
int  accel_read_d6d_src(uint8_t *src);
int  accel_d6d_tamper(uint8_t *src);
int  accel_snapshot_tilt_ref(void);
int  accel_tilt_from_ref_tenths(void);

/* impact forensics from the IMU FIFO ring buffer */
struct accel_impact {
    int peak_mg;          /* peak |a| vector magnitude (mg) */
    int peak_delta_mg;    /* peak | |a| - 1g | — the impact metric */
    int pax, pay, paz;    /* per-axis mg at the peak sample */
    int peak_gyro_dps10;  /* peak |ω| (degrees/sec × 10) */
    int samples;          /* accel samples drained from the ring */
    int over_ms;          /* time with | |a| - 1g | > 250 mg */
};
int  accel_fifo_enable(void);
int  accel_fifo_disable(void);
int  accel_fifo_drain_impact(struct accel_impact *out);

int  accel_read_baseline(void);
int  accel_confirm_movement(void);
int  accel_confirm_peak_mg(void);
void accel_get_movement_info(int *tilt_tenths, int *delta_mg);
int  accel_enable_wake_int(void);
int  accel_disable_wake_int(void);
void movement_reset(void);

#endif /* APP_H_ */
