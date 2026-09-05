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
/* Build the record even without a GPS fix (last known position, flagged
 * cl=1).  Set only for ignition changes — see collect_data(). */
extern bool  force_record;
/* Set by collect_data(): the record it just built has no live fix behind it. */
extern bool  last_record_stale;
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
int  modem_recover(void);               /* policy lives in modem.c */
bool modem_is_registered(void);         /* from the LTE event handler, not inferred */
void modem_send_ok(void);               /* a send got through: clear the stuck timer */
int  modem_update_cell_info(void);
const char *modem_rat(void);           /* "CATM1" / "NBIOT" / "UNKNOWN" */
bool modem_is_nbiot(void);
int  modem_rescan_plmn(int timeout_s); /* force cell/PLMN reselection */

int  gnss_init(void);
int  gnss_start(void);
int  gnss_stop(void);
int  gnss_collect(int timeout_ms, struct gnss_fix *out);  /* blocking with timeout */
/* Register a callback run roughly once a second while gnss_collect() waits.
 * NULL to unregister.  Runs on the caller's thread, so it needs no locking
 * against code that runs outside the wait. */
void gnss_set_tick(void (*cb)(void));
int  gnss_resume(void);   /* restart without resetting fix state (warm) */

#ifdef CONFIG_APP_DEMO_MODE
/* Placeholder printed in place of a latitude or longitude in demo mode. */
#define DEMO_COORD_MASK "xxxx.xx"
/* Copy `len` bytes of `in` into `out` with any coordinate replaced by the
 * placeholder, NUL-terminated; returns `out`.  Console output only — see
 * gnss.c. */
const char *demo_mask_coords(const char *in, size_t len,
                             char *out, size_t out_sz);
#endif

int  agnss_init(void);
int  agnss_fetch(void *agnss_request);  /* NULL = request all; else nrf_modem_gnss_agnss_data_frame* */

int  transport_open(void);
void transport_close(void);
void transport_teardown(void);
int  transport_send(const uint8_t *plaintext, size_t pt_len);
int  transport_recv_response(char *out_plaintext, size_t out_len, int timeout_ms);

int  collect_data(int ignition_state);
int  data_send_line(const char *line);   /* one raw line as its own datagram */

/* -- backlog buffer (src/databuf.c) ----------------------------------------
 * Holds records that could not be sent, so a radio outage delays the track
 * instead of losing it.  Statically sized: see APP_DATABUF_SLOTS.  A full
 * buffer is thinned by half rather than truncated, so a long outage comes
 * back at coarser resolution instead of stopping partway. */
int      databuf_push_lines(const char *buf, size_t len);
int      databuf_flush(int max_datagrams);   /* returns records sent */
int      databuf_count(void);
uint32_t databuf_dropped(void);
void     databuf_reset(void);
void data_reset(void);
int  send_data(void);

void cmd_run(char *cmd);

/* -- over-the-air update (fota.c) ------------------------------------------ */
/* Whether the caller is holding GNSS up, which decides if fota_check() has to
 * put it back after a download that didn't end in a reboot. */
enum fota_ctx {
    FOTA_CTX_AWAKE,    /* GNSS running — restart it if no update is applied */
    FOTA_CTX_ASLEEP,   /* GNSS already stopped — leave it that way */
};

const char *fota_version(void);        /* APP_VERSION_STRING, e.g. "0.4.0" */
const char *fota_board_id(void);       /* board + fitted ifaces, e.g. "v3.0+kline" */
void        fota_request_check(void);  /* force a check now (bare `fota` cmd) */
void        fota_notify_available(const char *ver);  /* fota=<ver> from the
                                          server response: check only if newer */
bool        fota_check_requested(void);
void        fota_confirm_image(void);  /* stop MCUboot reverting this image */
/* 0 = no update, 1 = updating (reboots, does not return), <0 = check failed */
int         fota_check(enum fota_ctx ctx);

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
/* Direct steady-state control: stop every pattern timer and drive LED1-3
 * from the mask.  On single-LED builds the LED1 bit drives led0. */
#define LED_MASK_1  0x01
#define LED_MASK_2  0x02
#define LED_MASK_3  0x04
void led_mask(uint8_t mask);
/* Accelerometer wake indication — no-ops unless CONFIG_APP_LED_ACCEL_WAKE.
 * Both return at once; the pattern plays out on a timer in the background. */
void led_accel_movement(void);
void led_accel_impact(void);

void watchdog_init(void);
void watchdog_kick(void);

void reboot_now(void);
void status_delay(long ms);   /* watchdog-aware sleep */

/* Hardware modules */
int  hw_gpio_init(void);
int  hw_selftest(void);
int  hw_power_init(void);
bool hw_power_available(void);
void hw_power_shutdown(void);
void hw_power_wake(void);
void hw_aux_power_on(void);
void hw_aux_power_off(void);
int  hw_accel_init(void);
bool accel_available(void);
int  kline_init(void);
int  kline_self_test(void);
int  kline_test(void);
/* Open a KWP2000 (ISO 14230) session with the vehicle over K — 5-baud init,
 * optionally the fast init and address sweeps — and report how.  Sends
 * nothing beyond the init itself.  See KWIRE.md. */
struct kline_session {
	const char *how;        /* which init worked */
	const char *protocol;   /* decoded from the key bytes */
	uint8_t ecu;            /* responding ECU address */
	uint8_t kb1, kb2;
	int rx_edges;           /* K edges seen in the listen window, -1 if not run */
	uint32_t baud;          /* ECU bit rate measured from its sync byte */
	uint8_t addrs[8];       /* every address that completed a 5-baud handshake */
	int n_addrs;
};
int  kline_vehicle_init(void);
int  kline_vehicle_init_ex(struct kline_session *out);

/* -- discovery: a one-shot investigation of an unknown vehicle --------------
 * Hunts for the protocol, data rate and ECU addresses, asks each responder
 * what it supports, and prints a summary ending in a suggested local.conf.
 * Separate from the runtime path below, which does no probing at all. */
struct kline_ecu {
	uint8_t addr;
	bool responds;          /* answered at least one request */
	bool session;           /* accepted StartDiagnosticSession */
	bool obd;               /* answered OBD mode 01 */
	bool ident;             /* answered ReadEcuIdentification (0x1A) */
	bool vin;               /* answered OBD mode 09 */
	bool mode03, mode07, mode0a;
	uint8_t pids[4];        /* mode 01 PID 00 support bitmap */
	bool pids_valid;
};

struct kline_discovery {
	bool ok;
	const char *how;        /* which init worked */
	const char *protocol;   /* decoded from the key bytes */
	uint32_t baud;
	bool use_l;
	struct kline_ecu ecu[8];
	int n_ecu;
	int engine;             /* index into ecu[], or -1 if none found */
};

int  kline_discover(struct kline_discovery *out);

/* -- runtime session: what polling uses ------------------------------------
 * No probing, no sweeps, no address hunting.  Opens at the address and rate
 * discovery settled on (APP_KLINE_ECU_ADDR, APP_KLINE_BAUD), exchanges OBD
 * mode 01 requests, closes.  Any request resets the 5 s P3 timer, so polling
 * at 1 Hz keeps the session alive without a separate TesterPresent. */
int  kline_session_open(void);
void kline_session_close(void);
void kline_session_abort(void);   /* no StopCommunication: ECU already gone */
int  kline_obd_pid(uint8_t pid, uint8_t *buf, int max);
int  kline_obd_dtcs(uint8_t mode, uint16_t *codes, int max);
void kline_dtc_string(uint16_t v, char *out);   /* 6 bytes: "P0133" + NUL */

/* -- OBD-II telemetry and fault codes (src/kline_obd.c) --------------------
 * The application layer over the runtime session.  Every field is an integer
 * with a fixed scale so the telemetry packet needs no float formatting;
 * OBD_NOT_AVAILABLE marks a PID this ECU does not support or did not answer.
 * The server unscales them (OBD_FIELDS in main.py). */
#define OBD_NOT_AVAILABLE INT32_MIN

struct obd_snapshot {
	bool valid;
	int32_t rpm;            /* rpm            */
	int32_t speed;          /* km/h           */
	int32_t coolant;        /* deg C          */
	int32_t intake;         /* deg C          */
	int32_t load;           /* %      x10     */
	int32_t throttle;       /* %      x10     */
	int32_t maf;            /* g/s    x100    */
	int32_t timing;         /* deg    x10     */
	int32_t stft, ltft;     /* %      x10     */
	int32_t rpm_min;        /* rpm, over the cycle */
	int32_t rpm_max;
	int32_t rpm_avg;
	int32_t fuel_status;    /* raw bitmap     */
	int32_t mil;            /* 0 / 1          */
	int32_t dtc_count;      /* stored codes   */
};

int  obd_poll(struct obd_snapshot *s);
int  obd_append(char *buf, int max, const struct obd_snapshot *s);
int  obd_dtc_report(char *buf, int max);
void obd_close(void);

/* Sampled ~1 Hz from the GNSS fix wait (see gnss_set_tick) so engine RPM has
 * useful resolution instead of one snapshot per telemetry record. */
void obd_sample_tick(void);

/* Bridge the gaps where nothing else is talking to the ECU (the send and the
 * idle between cycles), so the session is not dropped and re-initialised on
 * every cycle.  Cheap and self-throttling: a no-op unless the line has been
 * idle for 3 s. */
void obd_keepalive(void);

/* Engine RPM and vehicle speed as the ECU reports them, for the tracker's own
 * decisions rather than for the record.  Negative when the ECU is not
 * answering or the last reading has gone stale, so callers fall back to the
 * GNSS and battery-voltage proxies.  Speed is km/h: SAE J1979 defines PID
 * 0x0D that way regardless of what the dashboard displays. */
int  obd_rpm(void);
int  obd_speed_kmh(void);

/* The stored-code count rides in mode 01 PID 01, which every poll already
 * reads, so a code appearing or clearing mid-drive is visible for free.  That
 * is what triggers a mode 03 read; there is no periodic re-read. */
bool obd_dtc_pending(void);
int  obd_watch_dtc_count(void);   /* light PID 01 read, when telemetry is off */
int  proge_mode_on(void);
void proge_mode_off(void);
uint8_t kline_tx_rx_byte(uint8_t tx);

/* K-wire L line.  kline_l_send() is the only sanctioned way to drive the
 * pulldown FET: it returns -EPERM on the boards where doing so can destroy
 * the FET (and the nRF) if the wire is shorted to battery — see
 * APP_L_SEND_ENABLED.  The sense side (v3.3+) reads the wire back through
 * the SAADC; kline_l_line_probe() pulses the pulldown and reports whether
 * the line actually followed, which is how a short to battery is caught
 * before a 5-baud init runs into it. */
int  kline_l_send(bool on);
int  kline_l_sense_init(void);
bool kline_l_sense_available(void);
int  kline_l_sense_mv(void);            /* millivolts, or a negative errno */
int  kline_l_line_probe(int *idle_mv, int *pulled_mv);

int  hw_can_init(void);
bool hw_can_available(void);
int  hw_can_power_on(void);
void hw_can_power_off(void);
int  hw_can_test(void);
int  hw_can_selftest(void);
void can_bench_run(void);      /* CONFIG_APP_CAN_BENCH: host-driven test agent */

/* Interactive board bring-up rig (board_test.c, CONFIG_APP_BOARD_TEST). */
void board_test_run(void);

/* LTE TX power / brown-out rig (lte_power_test.c, CONFIG_APP_LTE_POWER_TEST). */
void lte_power_test_run(void);

int   accel_read(int *ax, int *ay, int *az);
int   accel_read_gyro(int *gx, int *gy, int *gz);
int   accel_gyro_autozero(void);
int   accel_read_temp(float *temp_c);
int   modem_read_temp(float *temp_c);
int   modem_read_vbat(int *mv);        /* nRF9151 VDD (= VSYS), millivolts */
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
