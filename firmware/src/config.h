/*
 * Compile-time configuration ported from the Polaris firmware.
 *
 * Default values target initial bring-up on the nRF9151 DK with the BG96
 * external modem replaced by the SoC's built-in LTE-M + GNSS, and with no
 * accelerometer or ignition pin attached. Anything that
 * depends on missing hardware is stubbed via the dedicated *_stub.c modules,
 * not by adding new conditionals here.
 */

#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

/* -- protocol / endpoint --------------------------------------------------- */
#define HOSTNAME            ""
#define UDP_PORT            65480
#define TLS_PORT            65481
#define DTLS_PORT           5684
#define UDP_PACKET_SIZE     1200
/* How long a send that asked for a reply listens for it.  Two seconds was
 * too tight for LTE-M: RAI releases the radio after each datagram, so the
 * reply arrives after an idle-to-connected transition and, if the modem has
 * already gone idle, a paging cycle on top.  A reply later than the window
 * is not lost — it lands during the next exchange, where it fails the tag
 * check against that request's nonce — so waiting a little longer here is
 * what stops a slow link turning into a stream of decrypt warnings.  Only
 * paid when a reply is actually late or lost, and only on the sends that
 * ask for one (transitions, standstill, and every APP_RESP_POLL_S). */
#define RESPONSE_TIMEOUT_MS 4000
#define TLS_SEC_TAG         1

/* Fallback PSK — all zeros disables sends.  Real keys go in local.conf
 * (gitignored) as CONFIG_APP_PSK_HEX. */
#define PSK_HEX_DEFAULT \
    "0000000000000000000000000000000000000000000000000000000000000000"

#define DEFAULT_APN         "sensor.net"
#define DEFAULT_USER        ""
#define DEFAULT_PASS        ""

/* -- intervals (seconds unless noted) -------------------------------------- */
#define ENGINE_OFF_LOOP_INTERVAL    CONFIG_APP_ENGINE_OFF_LOOP_INTERVAL
#define IGNITION_ON_SLEEP_INTERVAL  CONFIG_APP_IGNITION_ON_SLEEP_INTERVAL
#define VOLTAGE_POLL_INTERVAL       CONFIG_APP_VOLTAGE_POLL_INTERVAL
#define BATTERY_CHECK_INTERVAL      CONFIG_APP_BATTERY_CHECK_INTERVAL
#define NETWORK_REGISTRATION_TIMEOUT CONFIG_APP_NETWORK_REGISTRATION_TIMEOUT
#define NETWORK_RETRY_INTERVAL      CONFIG_APP_NETWORK_RETRY_INTERVAL
#define GPS_FIX_TIMEOUT_MS          CONFIG_APP_GPS_FIX_TIMEOUT_MS
#define GPS_COLD_FIX_TIMEOUT_MS     CONFIG_APP_GPS_COLD_FIX_TIMEOUT_MS

/* -- voltage thresholds (Kconfig uses mV, code uses float V) -------------- */
#define BATTERY_WARNING_LEVEL       (CONFIG_APP_BATTERY_WARNING_MV / 1000.0f)
#define BATTERY_POWEROFF_LEVEL      (CONFIG_APP_BATTERY_POWEROFF_MV / 1000.0f)
#define SLEEP_SAFETY_VOLTAGE        (CONFIG_APP_SLEEP_SAFETY_MV / 1000.0f)
#define ENGINE_RUNNING_VOLTAGE      (CONFIG_APP_ENGINE_RUNNING_MV / 1000.0f)
#define IMPLAUSIBLE_VOLTAGE         5.0f

/* -- engine-running detection, voltage fallback ---------------------------- */
/* Only consulted when the ECU is not answering — see engine_is_running().
 * Demoting to "engine stopped" needs the rail low AND the vehicle standing
 * still for this long; any GNSS speed above ENGINE_MOVING_KMH restarts the
 * hold.  Generous on purpose: holding the driving cadence a few minutes too
 * long after a genuine key-on-engine-off stop costs some power, dropping it
 * mid-journey costs the journey. */
#define ENGINE_STOPPED_HOLD_S       300
#define ENGINE_MOVING_KMH           3.0f  /* above this, something is driving */
/* How old a fix may be and still count as evidence of movement.  STATE_IDLE
 * only refreshes GNSS once per record, so this has to span a record interval
 * or a driving car reads as stationary between sends; it also has to stay
 * well under ENGINE_STOPPED_HOLD_S so a parked one still demotes. */
#define ENGINE_FIX_MAX_AGE_S        180
/* A low reading only counts as a low battery once the ignition has been off
 * this long.  Cranking pulls the rail to 9-10 V for a second or two, and the
 * sense line can read "off" for a moment inside that, which used to yield an
 * ignition-off record built at the bottom of the sag and a "low battery:
 * 9.8V" alert on every cold start.  A genuinely flat battery is still flat a
 * minute later. */
#define BATTERY_WARN_SETTLE_S       60

/* -- battery voltage sampling ---------------------------------------------- */
/* One INA228 conversion samples a single point on the alternator's ripple.
 * Every battery_read_voltage() averages this many conversions instead, which
 * is worth a few tens of mV.  It is not what keeps a charge-cut episode from
 * reading as "engine stopped" — see engine_is_running(). */
#define BATTERY_SAMPLES             8     /* conversions averaged per read   */
#define BATTERY_SAMPLE_GAP_MS       3     /* > one bus+shunt cycle (2.1 ms), */
                                          /* so each read is a new conversion */
#define BATTERY_SPREAD_WARN_V       0.5f  /* log min..max when wider than this */

/* -- accelerometer --------------------------------------------------------- */
#define ACC_MOVEMENT_THRESHOLD      CONFIG_APP_ACC_MOVEMENT_THRESHOLD
#define MOVEMENT_INACTIVITY_RESET   CONFIG_APP_MOVEMENT_INACTIVITY_RESET
#define CRASH_THRESHOLD_MG          CONFIG_APP_CRASH_THRESHOLD_MG
#define PARKED_IMPACT_MG            CONFIG_APP_PARKED_IMPACT_MG
#define IMPACT_IMMEDIATE_MG         CONFIG_APP_IMPACT_IMMEDIATE_MG
#define IMPACT_IMMEDIATE_MAX_MS     CONFIG_APP_IMPACT_IMMEDIATE_MAX_MS
#define TOW_TILT_DEG                CONFIG_APP_TOW_TILT_DEG
#define TOW_POLL_S                  CONFIG_APP_TOW_POLL_S
#define TOW_REARM_S                 CONFIG_APP_TOW_REARM_S
#define TOW_STABLE_TENTHS           10   /* attitude "still" band: 1.0 deg */
#define MOVEMENT_TEMPORARY_ENGINE_OFF_INTERVAL 14400
#define NO_MOVEMENT_GPS_SKIP        86400
#define DEFAULT_MOVEMENT_ALARM      IS_ENABLED(CONFIG_APP_MOVEMENT_ALARM)
#define LED_ACCEL_WAKE              IS_ENABLED(CONFIG_APP_LED_ACCEL_WAKE)
#define ACCEL_ALERT_PRIORITY        CONFIG_APP_ACCEL_ALERT_PRIORITY
#define ACCEL_ALERT_BACKOFF_S       CONFIG_APP_ACCEL_ALERT_BACKOFF_S
#define ACCEL_ALERT_BACKOFF_PRIORITY CONFIG_APP_ACCEL_ALERT_BACKOFF_PRIORITY

/* -- error escalation ------------------------------------------------------ */
#define MODEM_STUCK_CFUN_S          CONFIG_APP_MODEM_STUCK_CFUN_S
#define MODEM_STUCK_RESET_S         CONFIG_APP_MODEM_STUCK_RESET_S

/* -- watchdog -------------------------------------------------------------- */
/* How long any awake loop may go without feeding the watchdog.  Every long
 * operation in the firmware kicks as it waits (status_delay, gnss_collect,
 * the FOTA download, the registration polls), so this is a real ceiling on
 * one iteration, not a budget to be spent. */
#define WATCHDOG_TIMEOUT_S          32
/* The engine-off sleep waits up to an hour in one k_sem_take, far past the
 * window above, so it waits in slices this long and kicks between them.  A
 * slice that times out does not take the semaphore, so wake behaviour is
 * unchanged; it costs one CPU wake every 20 s, well under a microamp. */
#define WATCHDOG_SLEEP_SLICE_S      20

/* -- buffers --------------------------------------------------------------- */
#define DATA_LIMIT                  2500
/* Records per datagram while driving.  Each send costs an RRC connection
 * and, because GNSS and LTE share the antenna, a fix re-acquisition
 * afterwards — one to two seconds that the records themselves do not: with
 * the OBD poll folded into the fix wait a record costs about a second.  So
 * batching raises the record rate (0.33/s at 1, ~0.6/s at 3) at the price of
 * the page updating every N seconds instead of every cycle.  Three is about
 * the ceiling in practice: a record with ECU fields is 250-300 bytes and
 * BATCH_FLUSH_BYTES flushes before the datagram cap, so a larger setting
 * mostly hands the decision to that byte guard.  Ignition changes, settings
 * syncs and send failures still flush at once. */
#define BATCH_SIZE                  CONFIG_APP_BATCH_SIZE
/* Flush before the next record could overflow the datagram: room for one
 * more record plus the log lines that ride along.  Measured against the
 * transport's cap, not DATA_LIMIT — the buffer is bigger than a datagram. */
#define BATCH_HEADROOM              400
#define BATCH_FLUSH_BYTES           (UDP_PACKET_SIZE - 64 - BATCH_HEADROOM)
#define SPEED_MIN_SATS              4
/* Key-off means the vehicle is stopped, but GNSS speed does not settle to
 * exactly zero at a standstill: multipath and the receiver's own filter
 * leave a residual of a km/h or two, and the last fix before the key turned
 * can be a second or so old.  A record that says "ignition off at 2.4 km/h"
 * puts a phantom crawl on the end of every journey.  So on the ignition-off
 * record, and only there, a GNSS speed below this reads as stopped and goes
 * out as 0.  2 mph in km/h; above it the vehicle really was still rolling
 * (coasting to a stop, or the key cut while moving) and the figure stands. */
#define IGN_OFF_STOPPED_KMH         3.22f

/* -- hardware presence flags (compiled-out paths) -------------------------- */
#define LOW_POWER_STANDBY           1

/* -- coast-to-stop --------------------------------------------------------- */
#define COAST_STOP_SPEED_KMH        (CONFIG_APP_COAST_STOP_SPEED_KMH_X10 / 10.0f)
#define COAST_MAX_ITERATIONS        CONFIG_APP_COAST_MAX_ITERATIONS

/* -- gyro zero-rate auto-calibration --------------------------------------- */
/* The ASM330 gyro has a temperature-dependent zero-rate offset (bench data:
 * gy ~ -132 LSB, ~-1.2 dps, at standstill). When we have a good GNSS fix and
 * are stopped, learn that offset and subtract it so logged rates are honest. */
#define GYRO_REST_KMH               1.0f  /* treat as stationary below this  */
#define GYRO_AUTOZERO_SAMPLES       16    /* raw samples averaged per update */
#define GYRO_AUTOZERO_GAP_MS        5     /* spacing between those samples   */
#define GYRO_AUTOZERO_REJECT_LSB    250   /* |raw-bias| above this = real    */
                                          /* rotation, so skip the update    */
#define GYRO_AUTOZERO_EMA_SHIFT     2     /* drift tracking: new += (m-b)>>n */

/* -- movement confirmation ------------------------------------------------- */
#define MOVEMENT_CONFIRM_MS         CONFIG_APP_MOVEMENT_CONFIRM_MS
#define MOVEMENT_CONFIRM_HITS       CONFIG_APP_MOVEMENT_CONFIRM_HITS
#define ACCEL_POLL_INTERVAL         30

#endif /* APP_CONFIG_H_ */
