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
#define UDP_PORT            0
#define TLS_PORT            65481
#define DTLS_PORT           65482
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
#define MOVEMENT_TEMPORARY_ENGINE_OFF_INTERVAL 14400
#define NO_MOVEMENT_GPS_SKIP        86400
#define DEFAULT_MOVEMENT_ALARM      IS_ENABLED(CONFIG_APP_MOVEMENT_ALARM)

/* -- error escalation ------------------------------------------------------ */
#define GSM_ESCALATION_POWERCYCLE   CONFIG_APP_GSM_ESCALATION_POWERCYCLE
#define GSM_ESCALATION_SLEEP        CONFIG_APP_GSM_ESCALATION_SLEEP
#define GSM_RECOVERY_SLEEP_INTERVAL CONFIG_APP_GSM_RECOVERY_SLEEP_INTERVAL

/* -- buffers --------------------------------------------------------------- */
#define DATA_LIMIT                  2500
#define BATCH_SIZE                  1
#define BATCH_HEADROOM              400
#define SPEED_MIN_SATS              5

/* -- hardware presence flags (compiled-out paths) -------------------------- */
#define ALWAYS_ON_POWER             1
#define RELAY_CONNECTED             0
#define LOW_POWER_STANDBY           1

/* -- coast-to-stop --------------------------------------------------------- */
#define COAST_STOP_SPEED_KMH        (CONFIG_APP_COAST_STOP_SPEED_KMH_X10 / 10.0f)
#define COAST_MAX_ITERATIONS        CONFIG_APP_COAST_MAX_ITERATIONS

/* -- movement confirmation ------------------------------------------------- */
#define MOVEMENT_CONFIRM_MS         CONFIG_APP_MOVEMENT_CONFIRM_MS
#define MOVEMENT_CONFIRM_HITS       CONFIG_APP_MOVEMENT_CONFIRM_HITS
#define ACCEL_POLL_INTERVAL         30

#endif /* APP_CONFIG_H_ */
