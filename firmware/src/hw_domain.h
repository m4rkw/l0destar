#ifndef HW_DOMAIN_H_
#define HW_DOMAIN_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

/* Switched power domains and the GPIO sequencing they require.
 *
 * Signal pins that terminate inside a switched domain are "parked"
 * (input + pulldown, so the nRF can never source current into a dead rail)
 * whenever that domain is off, and "released" to their functional idle
 * state only while it is powered.  See Kconfig.boards for the per-board
 * topology this encodes.
 *
 * On boards with rail-sense inputs (v3.1) hw_domain_request() also confirms
 * the rail actually came up before it lets go of the pins; a domain whose
 * load switch fails stays parked and reports the fault rather than letting a
 * driver bit-bang into an unpowered peripheral.  A domain with no signal
 * pins (v3.x AUX, the GPS bias tee) is the exception: its enable is left
 * asserted through a rail fault, since nothing can be backfed and dropping
 * it would only guarantee no GPS.
 */
enum hw_domain {
	HW_DOMAIN_AUX,   /* AUX_SW: v2.x aux rail(s), v3.0/v3.1 GPS bias rail,
			  *         v4.0 PP3V3_GNSS (receiver and bias tee) */
	HW_DOMAIN_OBD,   /* OBD_EN: v3.0 PP3V3_OBD + PP12V_OBD (CAN + K-line) */
	HW_DOMAIN_CAN,   /* CAN_EN: v3.1 PP3V3_CAN */
	HW_DOMAIN_K,     /* K_EN:   v2.5K/v2.6K 5V/12V K rails,
			  *         v3.1 PP3V3_K + PP12V_K */
	HW_DOMAIN_COUNT
};

/* Reference-count users of a domain: the domain powers down only when the
 * last user releases it. */
#define HW_DOMAIN_USER_MAIN   0x01   /* boot / awake-state baseline */
#define HW_DOMAIN_USER_KLINE  0x02
#define HW_DOMAIN_USER_CAN    0x04
#define HW_DOMAIN_USER_GNSS   0x08   /* GPS bias tee during sleep-state fixes */
#define HW_DOMAIN_USER_TEST   0x10   /* the self-test power-cycling a rail */

int  hw_domain_init(void);

/* Power a domain up (or add a user to an already-powered one).  Returns 0 on
 * success, -EIO if the board has rail sensing and the rail did not come up —
 * in that case the domain is left off with every pin parked and the caller
 * must not touch its signals.  A domain with no signal pins never returns
 * -EIO: on a rail fault it stays enabled, counts as on, and
 * hw_domain_faulted() reports the fault (already logged and alerted). */
int  hw_domain_request(enum hw_domain d, uint8_t user);
void hw_domain_release(enum hw_domain d, uint8_t user);
bool hw_domain_is_on(enum hw_domain d);
bool hw_domain_faulted(enum hw_domain d);
bool hw_domain_fitted(enum hw_domain d);   /* has an enable pin on this board */
const char *hw_domain_name(enum hw_domain d);

/* For a driver that owns a peripheral on pins inside a domain (v4.0's GNSS
 * UART).  The callback runs with true once the domain's first user has it up
 * (pins released, enable high, rail sensed), and with false just before the
 * last user lets go, ahead of the pins being parked and the enable dropped:
 * a peripheral still connected to a pin overrides the parking, so it has to
 * be let go of first.  Both run on the caller's thread.  Setting one for a
 * domain that is already on calls it with true at once.  One per domain. */
typedef void (*hw_domain_notify_t)(bool up);
void hw_domain_notify_set(enum hw_domain d, hw_domain_notify_t cb);

/* v4.0, APP_GNSS_BACKUP_OFF_UTC_HOUR: call before each wait in the sleep loop
 * with its length.  If that hour falls inside the wait the receiver's backup
 * supply is dropped now, since the unit is not woken for it.  It comes back
 * with the receiver's rail, the next time anything requests it.  Does nothing
 * without the setting, or while the clock is not valid. */
void hw_gnss_backup_cutoff(int sleep_s);

/* The server's window, in UTC hours, in which the supply is kept off even
 * when the unit is driven (gw=).  -1 for either hour clears it. */
void hw_gnss_backup_window(int from_utc, int to_utc);

/* True while the enable is driven high. */
bool hw_gnss_backup_is_on(void);

/* The changes of the receiver's backup supply the server has not been told of
 * (each cut-off and return, not the state at boot), as the text of a bk=
 * field: "<state>:<age_s>|..." oldest first (backup_log.h).  Returns its
 * length, 0 when there is nothing (also on a board with no switched supply),
 * and in *count how many changes it holds.  Once that text is in a record,
 * call hw_gnss_backup_reported(*count); until then the changes stay queued. */
/* The server wants the supply on (gb=1) or dropped (gb=0, held until the
 * ignition comes on).  True when that changed what the unit does. */
bool hw_gnss_backup_want(bool on);
/* What the unit last was told: false while the supply is held down. */
bool hw_gnss_backup_wanted(void);

/* The ignition is on: the supply is no longer held down. */
void hw_gnss_backup_hold_clear(void);

int hw_gnss_backup_report(char *buf, size_t size, int *count);
void hw_gnss_backup_reported(int count);

/* The domain the CAN controller (MCP2518FD) is powered from on this board.
 * Anything else wired to its SPI bus has to wait for that rail: with it down
 * the controller's pins are clamp diodes into a dead supply (see the parking
 * rules at the top of hw_domain.c), and whatever drives the bus pushes current
 * into it.  hw_can.c and extflash.c both ask here. */
static inline enum hw_domain hw_can_domain(void)
{
	if (IS_ENABLED(CONFIG_APP_BOARD_SPLIT_OBD_DOMAIN)) {
		return HW_DOMAIN_CAN;   /* v3.1+: CAN_EN gates PP3V3_CAN */
	}
	return IS_ENABLED(CONFIG_APP_BOARD_OBD_DOMAIN) ? HW_DOMAIN_OBD
						       : HW_DOMAIN_AUX;
}

#endif
