#ifndef HW_DOMAIN_H_
#define HW_DOMAIN_H_

#include <stdbool.h>
#include <stdint.h>

/* Switched power domains and the GPIO sequencing they require.
 *
 * Signal pins that terminate inside a switched domain are "parked"
 * (input + pulldown, so the nRF can never source current into a dead rail)
 * whenever that domain is off, and "released" to their functional idle
 * state only while it is powered.  See Kconfig.boards for the per-board
 * topology this encodes.
 */
enum hw_domain {
	HW_DOMAIN_AUX,   /* AUX_SW: v2.x aux rail(s), v3.0/v3.1 GPS bias rail */
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

int  hw_domain_init(void);
void hw_domain_request(enum hw_domain d, uint8_t user);
void hw_domain_release(enum hw_domain d, uint8_t user);
bool hw_domain_is_on(enum hw_domain d);

#endif
