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
    /* Set from the server's response (track=<0|1>); never persisted.  See
     * do_track() in main.c and APP_TRACK_MODE. */
    int8_t  track_mode;
    uint8_t psk[32];
};

extern struct app_settings g_settings;

/* -- GNSS fix exposed to telemetry ----------------------------------------- */
struct gnss_fix {
    bool    valid;
    int64_t fix_uptime_ms;
    /* Uptime when the receiver delivered the fix, which is when it was taken
     * give or take the receiver's latency.  fix_uptime_ms is when the caller
     * picked it up, which a busy loop can make seconds later. */
    int64_t epoch_ms;
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
    /* Serving-cell signal quality from AT%XMONITOR, converted to the units
     * the 3GPP indices mean (see modem_read_signal).  signal_valid is false
     * until a reading has been taken on this cell — an unregistered modem
     * has nothing to report — and snr_db is 0 both for "0 dB" and for a
     * network that gave no SNR, which is why only rsrp gates validity. */
    int16_t  rsrp_dbm;
    int16_t  snr_db;
    uint8_t  band;
    bool     signal_valid;
    /* Uptime of the reading.  A signal figure is a measurement, not a
     * condition: g_cell.dirty is also raised by a cell change, which carries
     * no fresh reading with it, so without a timestamp a record built after
     * one would have emitted an RSRP measured hours earlier on a different
     * cell — dressed up as this record's own.  See SIGNAL_FRESH_MS. */
    int64_t  signal_ms;
    /* From lte_lc_conn_eval_params_get(), which unlike %XMONITOR costs an
     * evaluation and can be refused outright — "radio busy" is what a drive
     * with GNSS running usually answers.  conn_valid says whether the last
     * attempt produced anything; the %XMONITOR figures above stand on their
     * own either way.
     *
     * pathloss_db is the one worth having.  RSRP says how strong the signal
     * arrived; path loss says how much was lost getting here, normalised
     * against what the cell says it transmits, so it compares across cells
     * and distances where a raw RSRP does not.  Excess attenuation in the
     * antenna path shows up in it directly. */
    int16_t  pathloss_db;
    int16_t  rsrq_x10;      /* tenths of a dB; the index has 0.5 dB steps */
    int16_t  tx_rep;
    int8_t   ce_level;
    bool     conn_valid;
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
/* How long the last engine-off telemetry wake took, and which record it
 * belongs to.  ms is measured from the moment the sleep loop woke to the
 * moment it settled back down with the send done and the modem off (or left
 * to fall into PSM); rec_id is the rid= of the record that wake produced, 0
 * when there is nothing to report (both are set and cleared together).
 *
 * The figure cannot ride the record it describes — the device is still awake
 * when it builds one, and the expensive part of a bad wake comes after the
 * send, while the loop waits for a registration that never arrives.  So it is
 * finished once the wake is over and carried on the next record as
 * wt=<rec_id>:<ms>, and the server files it against that record rather than
 * against whatever happened to arrive before it.  Naming the record is what
 * makes a wake whose own send failed reportable: that record goes to the
 * backlog and is delivered after the wt= that describes it, so arrival order
 * says nothing.  Cleared by append_sync_fields() as it emits them. */
struct wake_report {
    uint32_t rec_id;
    int64_t  ms;
    /* How much of ms went on the LTE attach, which is what actually varies:
     * the send is uniformly sub-second, so a wake that took 44 s spent 41 of
     * them registering.  -1 when there was no attach to pay for (the modem
     * was already registered) or it never completed. */
    int32_t  attach_ms;
    /* The serving cell as the wake left it, when its record went out without
     * a reading of its own.  A wake that finds the modem in PSM builds its
     * record before the radio is up, and a sleeping modem has no measurement
     * to give; the send wakes it, so the reading is taken once the reply is
     * in.  Carried beside wt= as ws=<rec_id>:<rsrp>:<snr>:<band> and filed
     * against the same record.  signal is false when there is none. */
    bool     signal;
    int16_t  rsrp_dbm;
    int16_t  snr_db;
    uint8_t  band;
};

extern struct wake_report wake_pending;
/* Live verdict from the ECU's RPM when a fresh figure exists, else from
 * battery_v, GNSS speed and a hold timer — see the note on the definition
 * for why the fallback cannot be a bare voltage threshold.  Callers deciding
 * cadence or state should use this rather than test battery_v themselves.
 * Reads battery_v and engine_running and advances the fallback's hold timer,
 * so call it once per fresh battery_v rather than treating it as a free
 * query. */
bool engine_is_running(void);

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
int  modem_radio_up(void);              /* settings + CFUN=1, no wait */
void modem_power_off(void);             /* CFUN=0 on purpose: not an outage */
/* The way into sleep: PSM when the network has granted it (the modem stays
 * registered and sleeps itself), else modem_power_off(). */
void modem_sleep(void);
#if IS_ENABLED(CONFIG_APP_PSM_PROBE)
bool modem_psm_granted(void);           /* the network gave us an active time */
bool modem_psm_asleep(void);            /* the modem says it is in PSM now */
#else
static inline bool modem_psm_granted(void) { return false; }
static inline bool modem_psm_asleep(void)  { return false; }
#endif
#if IS_ENABLED(CONFIG_APP_PSM_SLEEP)
/* Milliseconds since the radio's last registration, RRC change or wake from
 * PSM: how long a modem that is not in PSM has had to get there. */
int64_t modem_radio_quiet_ms(void);
#else
static inline int64_t modem_radio_quiet_ms(void) { return 0; }
#endif
int  modem_get_imei(char *out, size_t out_len);
int  modem_get_network_status(void);   /* 1=home, 5=roaming */
void modem_set_apn(const char *apn);
int  modem_at(const char *cmd, char *resp, size_t resp_len);
int  modem_recover(void);               /* policy lives in modem.c */
bool modem_is_registered(void);         /* from the LTE event handler, not inferred */
/* Called from the LTE event handler's thread each time registration arrives.
 * Keep it to signalling a waiter. */
void modem_on_registered(void (*cb)(void));
int  modem_unregistered_s(void);        /* seconds up without a registration, -1 if not searching */
void modem_send_ok(void);               /* a send got through: clear the stuck timer */
int  modem_update_cell_info(void);
/* Serving-cell RSRP/SNR/band into g_cell; folded into modem_update_cell_info,
 * so callers refreshing cell context get it without asking. */
int  modem_read_signal(void);
/* How long the last attach took, in ms, or -1 if none has completed since the
 * search began.  Measured from the registration event, not the 1 Hz poll. */
int  modem_attach_ms(void);
/* For explaining a gap between fixes (data.c), as running totals that wrap:
 * take differences.  RRC connected time is time the network held the radio,
 * which GNSS cannot then have; losses counts registrations lost. */
uint32_t modem_rrc_connected_ms(void);
uint32_t modem_reg_losses(void);
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
/* For explaining a gap between fixes (data.c).  Running totals in 32-bit
 * milliseconds and counts, which wrap: take differences.  Blocked is time
 * the receiver ran but LTE had the radio; starved is epochs it flagged as
 * short of radio time.  The last wait is the last gnss_collect()'s. */
uint32_t gnss_blocked_ms(void);
uint32_t gnss_starved_epochs(void);
int32_t  gnss_last_wait_ms(void);

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

/* -- telemetry transport ----------------------------------------------------
 * transport.c, the l0destar server's encrypted UDP, or traccar.c, Traccar's
 * OsmAnd protocol over HTTP (CONFIG_APP_TRACCAR); one of the two is built.
 * Both take the same payload: newline-separated lines in the server
 * protocol's format (records, "A," alerts, "D," fault codes, "L," log
 * lines), which is what data.c, databuf.c and alert.c produce. */
int  transport_open(void);
/* The exchange is over: let the radio go.  The UDP transport closes its
 * socket, after reading any replies already in, and a reply that arrives
 * later has nowhere to land — so its send stays unanswered and its records
 * are sent again; databuf_settle() first if that matters.  The Traccar one
 * keeps its TCP connection for the next send and only hints the release,
 * since closing would cost a fresh RRC connection for the FIN. */
void transport_close(void);
/* Streaming: keep the socket and the RRC connection up between sends
 * (RAI_ONGOING) instead of releasing the radio after each one.  Only
 * sensible with GNSS stopped, since the radio is then LTE's anyway. */
void transport_set_streaming(bool on);
/* Drop the socket, hint or no hint: before the modem is powered off or
 * reset, and when a send has failed on it. */
void transport_teardown(void);
int  transport_send(const uint8_t *plaintext, size_t pt_len);
/* The server's reply to the last send, "1,<interval>,<movement_alarm>[,cmd]".
 * UDP waits up to timeout_ms for the datagram; Traccar already has it (a
 * 2xx, and any queued command) and answers at once.  Replies to earlier
 * sends met on the way go to data_reply(). */
int  transport_recv_response(char *out_plaintext, size_t out_len, int timeout_ms);
/* Delivery.  A UDP send returning 0 means the modem queued the datagram,
 * nothing more; the server's reply is the proof it arrived.  Each send gets
 * an id, and a reply is matched to the send it answers however late it
 * comes, so the records in a datagram can be held until then (databuf_sent).
 * transport_sent_id() is the last send's id, or 0 when that send was its own
 * receipt (Traccar's HTTP 2xx) and there is nothing to wait for.
 * transport_poll() reads whatever replies have come in, without waiting,
 * and hands each to data_reply(); -ENOTCONN with no socket to read. */
uint32_t transport_sent_id(void);
int  transport_poll(void);
bool transport_is_streaming(void);
/* A reply to an earlier send that was not waited for: the receipt for that
 * datagram (databuf_ack), and whatever settings or commands it carries. */
void data_reply(uint32_t id, const char *resp);

int  collect_data(int ignition_state);
/* The rid= stamped on the most recently built record.  A record is only
 * identifiable to the server once it has one, so this is how a caller names
 * the record it has just built — see wake_pending.  0 before the first. */
uint32_t data_last_rec_id(void);
int  data_send_line(const char *line);   /* one raw line as its own datagram */
/* Track-mode record: no GNSS wait, last known position, the fast OBD poll and
 * an IMU burst (tm=1, acc=...).  Returns 1 with a record in data_current. */
int  collect_track_data(void);

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
/* Records a send has put out, held under its transport id until the server
 * answers for them (databuf_ack) — see APP_DATABUF_UNACKED_SLOTS.  Records
 * whose answer does not come in APP_ACK_TIMEOUT_S go back into the backlog
 * (databuf_expire), as does whatever is still unanswered when the socket
 * is about to go (databuf_settle). */
void     databuf_sent(uint32_t id, const char *recs, size_t len);
void     databuf_ack(uint32_t id);
int      databuf_expire(void);               /* returns datagrams requeued */
int      databuf_unacked(void);
int64_t  databuf_last_ack_ms(void);          /* when an answer last came in */
/* Wait up to timeout_ms for the answers still owed, then requeue what is
 * left; returns the datagrams requeued. */
int      databuf_settle(int timeout_ms);
/* The radio is about to go down: settle, then send as much of the backlog
 * as the link will take, a few datagrams at a time. */
void     databuf_deliver(int timeout_ms);
/* Copy newline-separated records, adding age=<s> to any built long enough
 * ago that the server would otherwise file it at the wrong time.  Returns
 * the bytes written, or -1 if they do not fit. */
int      databuf_stamp(const char *in, size_t len, char *out, size_t cap);
void data_reset(void);
int  send_data(void);

/* -- captured warnings/errors (src/dbglog.c) --------------------------------
 * A log backend keeps WRN/ERR lines in RAM; send_data() appends them to the
 * outgoing record as "L,<uptime_ms>,<E|W>,<module>: <text>" lines and frees
 * them only once the datagram has left.  See dbglog.c for the policy. */
#if IS_ENABLED(CONFIG_APP_DEBUG_LOG)
size_t      dbglog_pending(void);              /* bytes waiting */
int         dbglog_take(char *out, size_t max);/* append lines; bytes added */
void        dbglog_ack(bool sent);             /* free (or keep) what was taken */
const char *dbglog_reset_cause(void);          /* "wdt", "sw+pin", ... or NULL */
#else
static inline size_t      dbglog_pending(void) { return 0; }
static inline int         dbglog_take(char *out, size_t max) { return 0; }
static inline void        dbglog_ack(bool sent) { }
static inline const char *dbglog_reset_cause(void) { return NULL; }
#endif

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
bool        fota_image_on_probation(void); /* swapped in, not yet confirmed */
void        fota_verdict_on_boot(void); /* did the staged update take? */
void        fota_report_flush(void);   /* send that verdict once linked */
/* 0 = no update, 1 = updating (reboots, does not return), <0 = check failed */
int         fota_check(enum fota_ctx ctx);

void alert_enqueue(const char *msg, int priority);
int  alert_send(void);
int  alert_send_standalone(void);
extern int  alert_count;             /* waiting for a send */
/* Alerts are held until the reply to the datagram that carried them comes in,
 * like records (see alert.c).  databuf_ack() passes every reply on, and
 * databuf_settle() waits for these too before the socket goes. */
void alert_ack(uint32_t id);
int  alert_unanswered(void);         /* sent, reply not yet in */
void alert_expire(void);             /* unanswered past APP_ACK_TIMEOUT_S: queue again */
void alert_settle(void);             /* all unanswered: queue again */

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
const char *fatal_last_crash(void); /* "fatal:<reason>@<pc>" once, else NULL */

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
	bool use_l;             /* the init that worked needed the L line */
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

/* The poll: RPM, speed, throttle and load every call, plus one of the
 * slow-moving PIDs in rotation — four or five exchanges (~0.5 s) rather than
 * thirteen — merged into a live snapshot.  obd_poll_fast() always goes to
 * the bus (track mode, one per record); obd_snapshot_take() copies the
 * snapshot and polls first only if it is more than a second old (normal
 * records, which the fix-wait tick has usually just refreshed). */
int  obd_poll_fast(struct obd_snapshot *s);
int  obd_snapshot_take(struct obd_snapshot *s);
int  obd_append(char *buf, int max, const struct obd_snapshot *s);
int  obd_dtc_report(char *buf, int max);
void obd_close(void);

/* Runs the poll ~1 Hz from the GNSS fix wait (see gnss_set_tick), so the
 * record built after the fix costs no bus time and engine RPM has useful
 * resolution instead of one sample per record.  Never opens a session. */
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

/* -- threshold alerts on the snapshot (src/obd_alert.c) --------------------
 * CONFIG_APP_OBD_ALERTS rules ("coolant>=90,rpm>6500"), judged on the live
 * snapshot after every poll.  No-ops on a build without APP_KLINE_TELEMETRY
 * or with no rules. */
#if IS_ENABLED(CONFIG_APP_KLINE_TELEMETRY)
void obd_alert_init(void);                          /* parse the rules; logs each */
void obd_alert_eval(const struct obd_snapshot *s);  /* queue alerts and clears */
void obd_alert_reset(void);                         /* re-arm: session closed */
#else
static inline void obd_alert_init(void) { }
static inline void obd_alert_eval(const struct obd_snapshot *s) { }
static inline void obd_alert_reset(void) { }
#endif

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
void kline_l_sense_test(void);           /* CONFIG_APP_L_SENSE_TEST; never returns */

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
int64_t ignition_last_on_ms(void);      /* uptime of the last read that found it on, -1 if none */
float battery_read_voltage(void);
float battery_poll_voltage(void);       /* one conversion from a shut-down INA, left shut down */
bool  battery_on_backup(float v);       /* supply is the inline backup module, not the car */
extern bool backup_woke;                /* this wake found the module taking over: alert without the settle wait */

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

/* One IMU sample out of the FIFO ring: accel in milli-g at the current
 * full-scale, gyro as bias-corrected raw LSB at +/-250 dps — the same units
 * as accel_read() / accel_read_gyro(), so the server scales them alike. */
struct accel_sample {
    int16_t ax, ay, az;
    int16_t gx, gy, gz;
};
/* Drain everything batched since the last drain and return up to `max`
 * samples, evenly spaced across the interval when there were more.  Returns
 * the count written, or negative.  Samples arrive at 26 Hz (FIFO_SAMPLE_MS). */
int  accel_fifo_drain_samples(struct accel_sample *out, int max);
#define ACCEL_FIFO_SAMPLE_MS 38
/* Drain the FIFO for the dead reckoning alone (every drain feeds it): a
 * no-op unless the awake configuration is batching the gyro.  Returns the
 * gyro samples read.  Not while an impact is waiting for crash_check(),
 * whose profile is in the FIFO — dr_fifo_service() in main.c minds that. */
int  accel_fifo_service(void);

/* -- dead reckoning across GNSS gaps (src/motion.c) -------------------------
 * The record that ends a gap between fixes carries the motion through it,
 * mv=<gap>:<v0>:<steps>:<end>, and the server fits the path between the
 * two fixes.  See motion.c for the field and why. */
#if IS_ENABLED(CONFIG_APP_DEAD_RECKONING)
void motion_reset(void);
/* A FIFO drain's words, raw: accel at the awake full-scale, gyro at
 * +/-250 dps less `bias`.  `lost` when the FIFO overran since the last. */
void motion_feed(const int16_t (*xl)[3], int nxl,
		 const int16_t (*gy)[3], int ngy,
		 const int bias[3], int64_t now_ms, bool lost);
void motion_note_speed(int kmh);          /* every ECU speed reading */
/* ",mv=..." for the gap from_ms..to_ms (fix epochs, uptime) into out, at
 * most max bytes and not NUL-terminated; 0 when there is no gap worth it,
 * nothing measured across it, it does not fit, or nothing moved (`moving`
 * says the fixes at either end did). */
int  motion_field(char *out, int max, int64_t from_ms, int64_t to_ms,
		  bool moving);
/* Remove every mv= field from the records in buf; returns the new length. */
int  motion_strip(char *buf, int len);
/* main.c: accel_fifo_service() unless an impact is pending or the key is
 * off.  The fix wait ticks it, and collect_data() runs it before a field. */
void dr_fifo_service(void);
#else
static inline void motion_reset(void) { }
static inline void motion_feed(const int16_t (*xl)[3], int nxl,
			       const int16_t (*gy)[3], int ngy,
			       const int bias[3], int64_t now_ms, bool lost) { }
static inline void motion_note_speed(int kmh) { }
static inline int  motion_field(char *out, int max, int64_t from_ms,
				int64_t to_ms, bool moving) { return 0; }
static inline int  motion_strip(char *buf, int len) { return len; }
static inline void dr_fifo_service(void) { }
#endif
/* Bytes of mv= fields in data_current, which the batch's flush threshold
 * leaves out: the motion must never make a batch go early. */
int  data_motion_bytes(void);
/* The drive's run of live fixes is over (sleep, track mode): the next fix
 * starts a new one rather than ending a gap from before. */
void data_gap_reset(void);

int  accel_read_baseline(void);
int  accel_confirm_movement(bool (*stop)(void));
int  accel_confirm_peak_mg(void);
void accel_get_movement_info(int *tilt_tenths, int *delta_mg);
int  accel_enable_wake_int(void);
int  accel_disable_wake_int(void);
void movement_reset(void);

#endif /* APP_H_ */
