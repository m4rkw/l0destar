/*
 * Compile-time configuration ported from the Polaris firmware.
 *
 * Default values target initial bring-up on the nRF9151 DK with the BG96
 * external modem replaced by the SoC's built-in LTE-M + GNSS, and with no
 * accelerometer, ignition pin, or relay board attached. Anything that
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
#define ENGINE_STOPPED_COUNT        10

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

/* -- error escalation ------------------------------------------------------ */
#define GSM_ESCALATION_POWERCYCLE   CONFIG_APP_GSM_ESCALATION_POWERCYCLE
#define GSM_ESCALATION_SLEEP        CONFIG_APP_GSM_ESCALATION_SLEEP
#define GSM_RECOVERY_SLEEP_INTERVAL CONFIG_APP_GSM_RECOVERY_SLEEP_INTERVAL

/* -- buffers --------------------------------------------------------------- */
#define DATA_LIMIT                  2500
#define BATCH_SIZE                  1
#define BATCH_HEADROOM              400
#define SPEED_MIN_SATS              4

/* -- hardware presence flags (compiled-out paths) -------------------------- */
#define ALWAYS_ON_POWER             1
#define RELAY_CONNECTED             IS_ENABLED(CONFIG_APP_RELAY_CONNECTED)
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
