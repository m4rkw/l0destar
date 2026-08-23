/*
 * Switched power-domain sequencing.
 *
 * Every l0destar PCB gates parts of the circuit behind GPIO-controlled rails
 * (load switches / FETs / buck enables).  Any nRF pin that terminates inside
 * a switched domain must never be driven high while that domain is off: the
 * peripheral's ESD clamps sit at VDD + 0.3 V (MCP2518FD) or VCCA + 0.5 V
 * (TXS0104E), so a high pin backfeeds the dead rail through the clamp
 * diodes.  Pull-ups that live on a switched rail (CAN_INT/CAN_CS 10K,
 * v3.0/v3.1 K_TX/K_RX 10K) float when it is down, so inputs get the nRF
 * internal pulldown while parked.
 *
 * Rules implemented here:
 *   - park  (domain off): every domain pin -> INPUT | PULL_DOWN.  The nRF
 *     sources nothing, floating pull-ups are defeated.
 *   - release (domain on): outputs that idle low (SPI SCK/SDI, L_SEND,
 *     K_SLEEP) -> OUTPUT_LOW; everything else -> INPUT no-pull, letting the
 *     now-powered board pull-ups define the idle level (CS/INT high, K
 *     recessive).
 *   - order: release pins BEFORE raising the enable (inputs float up with
 *     the rail via their pull-ups; low outputs source nothing), park pins
 *     BEFORE dropping the enable.
 *
 * Domains are reference-counted so shared rails (v3.0 OBD feeds both CAN
 * and K-line; AUX feeds GPS bias plus, on v2.x, the OBD circuits) only drop
 * when the last user lets go.  v3.1 splits OBD into independent CAN_EN and
 * K_EN domains, so the unused interface never has to be powered.
 *
 * v3.1 also feeds each rail's actual state back on a sense input, so the
 * enable is no longer taken on trust: hw_domain_request() waits for the
 * sense line(s) before it reports success, and re-parks everything and
 * fails if the rail never appears.  Without that check a failed load switch
 * would leave the drivers bit-banging 3.3 V into an unpowered MCP2518FD or
 * TJA1027T through their clamp diodes — exactly what the parking rules above
 * exist to prevent.
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "hw_common.h"
#include "hw_domain.h"
#include "pins.h"

LOG_MODULE_REGISTER(hw_domain, CONFIG_APP_LOG_LEVEL);

/* Rail settle after the enable rises: covers the SiP32431 soft-start, the
 * ITS4060 turn-on and the v2.5K/v2.6K LT8609 5V buck start-up. */
#define DOMAIN_SETTLE_MS 15

/* How long a sensed rail is given to reach its expected state.  Coming up is
 * fast (soft-start, tens of microseconds to a few ms).  Going down is much
 * slower: nothing on these rails is actively discharged, so the fall time is
 * set by the load.  PP3V3_CAN is the worst case — 10.2 uF against the parked
 * CAN_CS/CAN_INT pulldowns in series with their 10K pull-ups (~11.5 kOhm once
 * the MCP2518FD and MAX33041 drop out of regulation), i.e. ~130 ms to fall
 * below the sense threshold; PP12V_K is ~100 nF against the 280K sense
 * divider plus the TJA1027T's sleep current, i.e. tens of ms.  Only the
 * up-direction is waited on here; hw_selftest owns the (much slower)
 * check that a disabled rail actually falls. */
#define RAIL_UP_TIMEOUT_MS 50
#define RAIL_POLL_MS       5

enum pin_role {
	ROLE_OUT_LOW,   /* released as OUTPUT_LOW (idle-low outputs) */
	ROLE_IN,        /* released as INPUT no-pull (board pulls define idle) */
};

struct domain_pin {
	int8_t pin;
	uint8_t role;
};

/* A rail-status input and the level it reads while its rail is up.  The 3.3V
 * senses are plain dividers (high = up); PP12V_K is inverted by a 2N7002
 * (low = up), per CONFIG_APP_BOARD_RAIL_ST_12V_ACTIVE_LOW. */
struct domain_sense {
	int8_t pin;
	uint8_t up_level;
};

#define DOMAIN_MAX_PINS 12
#define DOMAIN_MAX_SENSE 2

struct domain {
	int8_t enable_pin;               /* -1 = domain absent on this board */
	uint8_t users;
	uint8_t npins;
	uint8_t nsense;
	bool faulted;                    /* rail failed to come up; alerted once */
	struct domain_pin pins[DOMAIN_MAX_PINS];
	struct domain_sense sense[DOMAIN_MAX_SENSE];
};

static struct domain s_dom[HW_DOMAIN_COUNT];

static void dom_add(struct domain *d, int pin, enum pin_role role)
{
	if (pin < 0 || d->npins >= DOMAIN_MAX_PINS) {
		return;
	}
	d->pins[d->npins++] = (struct domain_pin){ (int8_t)pin, (uint8_t)role };
}

static void dom_park(const struct domain *d)
{
	for (int i = 0; i < d->npins; i++) {
		gpio_pin_configure(hw_gpio0, d->pins[i].pin,
				   GPIO_INPUT | GPIO_PULL_DOWN);
	}
}

static void dom_release_pins(const struct domain *d)
{
	for (int i = 0; i < d->npins; i++) {
		gpio_pin_configure(hw_gpio0, d->pins[i].pin,
				   d->pins[i].role == ROLE_OUT_LOW ?
				   GPIO_OUTPUT_LOW : GPIO_INPUT);
	}
}

static void dom_add_sense(struct domain *d, int pin, bool active_low)
{
	if (pin < 0 || d->nsense >= DOMAIN_MAX_SENSE) {
		return;
	}
	d->sense[d->nsense++] = (struct domain_sense){ (int8_t)pin,
						       active_low ? 0 : 1 };
}

/* True once every sense line for this domain reads the wanted state. */
static bool dom_rail_at(const struct domain *d, bool up)
{
	for (int i = 0; i < d->nsense; i++) {
		int want = up ? d->sense[i].up_level : !d->sense[i].up_level;
		if (gpio_pin_get(hw_gpio0, d->sense[i].pin) != want) {
			return false;
		}
	}
	return true;
}

static bool dom_wait_rail(const struct domain *d, bool up, int timeout_ms)
{
	if (d->nsense == 0) {
		return true;
	}
	for (int waited = 0;; waited += RAIL_POLL_MS) {
		if (dom_rail_at(d, up)) {
			return true;
		}
		if (waited >= timeout_ms) {
			return false;
		}
		k_msleep(RAIL_POLL_MS);
	}
}

static void dom_add_can_pins(struct domain *d)
{
	dom_add(d, PIN_CAN_SCK, ROLE_OUT_LOW);
	dom_add(d, PIN_CAN_SDI, ROLE_OUT_LOW);
	dom_add(d, PIN_CAN_SDO, ROLE_IN);
	dom_add(d, PIN_CAN_CS,  ROLE_IN);      /* 10K pull-up on the CAN rail */
	dom_add(d, PIN_CAN_INT, ROLE_IN);      /* 10K pull-up on the CAN rail */
}

static void dom_add_kline_pins(struct domain *d)
{
	dom_add(d, PIN_K1_TX,   ROLE_IN);      /* board/shifter pull-ups idle high */
	dom_add(d, PIN_K1_RX,   ROLE_IN);
	dom_add(d, PIN_L_SEND,  ROLE_OUT_LOW); /* gates the L pulldown FET */
	dom_add(d, PIN_L_RECV,  ROLE_IN);
	dom_add(d, PIN_K_SLEEP, ROLE_OUT_LOW); /* TJA1027 stays asleep until used */
}

int hw_domain_init(void)
{
	struct domain *aux = &s_dom[HW_DOMAIN_AUX];
	struct domain *obd = &s_dom[HW_DOMAIN_OBD];
	struct domain *can = &s_dom[HW_DOMAIN_CAN];
	struct domain *k   = &s_dom[HW_DOMAIN_K];

	aux->enable_pin = PIN_AUX_SW;
	obd->enable_pin = PIN_OBD_EN;
	can->enable_pin = PIN_CAN_EN;
	k->enable_pin   = PIN_K_EN;

	if (IS_ENABLED(CONFIG_APP_BOARD_HAS_CAN)) {
		if (IS_ENABLED(CONFIG_APP_BOARD_CAN_ON_AUX)) {
			dom_add_can_pins(aux);
		} else if (IS_ENABLED(CONFIG_APP_BOARD_SPLIT_OBD_DOMAIN)) {
			dom_add_can_pins(can);
		} else if (IS_ENABLED(CONFIG_APP_BOARD_OBD_DOMAIN)) {
			dom_add_can_pins(obd);
		}
	}
	if (IS_ENABLED(CONFIG_APP_BOARD_HAS_KLINE)) {
		if (IS_ENABLED(CONFIG_APP_BOARD_KLINE_ON_AUX)) {
			dom_add_kline_pins(aux);
		} else if (IS_ENABLED(CONFIG_APP_BOARD_KLINE_SHIFT_ON_AUX)) {
			/* Shifter A-side rail is AUX but the pins are only
			 * usable with the K rails up too: gate them on K. */
			dom_add_kline_pins(k);
		} else if (IS_ENABLED(CONFIG_APP_BOARD_SPLIT_OBD_DOMAIN)) {
			dom_add_kline_pins(k);
		} else if (IS_ENABLED(CONFIG_APP_BOARD_OBD_DOMAIN)) {
			dom_add_kline_pins(obd);
		}
	}
	if (IS_ENABLED(CONFIG_APP_BOARD_RELAY_FB_ON_AUX)) {
		dom_add(aux, PIN_RLY_SET_FB, ROLE_IN);
		dom_add(aux, PIN_RLY_RST_FB, ROLE_IN);
	}

	/* Everything starts parked with the enables low. */
	for (int i = 0; i < HW_DOMAIN_COUNT; i++) {
		struct domain *d = &s_dom[i];
		d->users = 0;
		dom_park(d);
		if (d->enable_pin >= 0) {
			gpio_pin_configure(hw_gpio0, d->enable_pin,
					   GPIO_OUTPUT_LOW);
		}
	}

	/* Rail-sense inputs (v3.1+): always-on status from the load switches.
	 * The external divider / pull-up defines both levels, so no internal
	 * pull (an internal pulldown would fight the 100K/1M dividers).
	 * Attaching them to their domains is what lets hw_domain_request()
	 * verify the rail instead of assuming the enable worked. */
	if (IS_ENABLED(CONFIG_APP_BOARD_HAS_RAIL_SENSE)) {
		const bool inv_12v =
			IS_ENABLED(CONFIG_APP_BOARD_RAIL_ST_12V_ACTIVE_LOW);

		dom_add_sense(aux, PIN_GPS_RAIL_ST,  false);
		dom_add_sense(can, PIN_CAN_RAIL_ST,  false);
		dom_add_sense(k,   PIN_K3V3_RAIL_ST, false);
		dom_add_sense(k,   PIN_K12V_RAIL_ST, inv_12v);

		static const int8_t rail_st[] = {
			PIN_GPS_RAIL_ST, PIN_CAN_RAIL_ST,
			PIN_K3V3_RAIL_ST, PIN_K12V_RAIL_ST
		};
		for (int i = 0; i < (int)ARRAY_SIZE(rail_st); i++) {
			if (rail_st[i] >= 0) {
				gpio_pin_configure(hw_gpio0, rail_st[i],
						   GPIO_INPUT);
			}
		}
	}

	/* AIO inputs (v2.1): external 100K/10K dividers define the level. */
	static const int8_t aio[] = { PIN_AIO1, PIN_AIO2, PIN_AIO3,
				      PIN_AIO4, PIN_AIO5, PIN_AIO6 };
	for (int i = 0; i < (int)ARRAY_SIZE(aio); i++) {
		if (aio[i] >= 0) {
			gpio_pin_configure(hw_gpio0, aio[i], GPIO_INPUT);
		}
	}

	return 0;
}

static const char *dom_name(enum hw_domain d)
{
	switch (d) {
	case HW_DOMAIN_AUX: return "AUX";
	case HW_DOMAIN_OBD: return "OBD";
	case HW_DOMAIN_CAN: return "CAN";
	case HW_DOMAIN_K:   return "K";
	default:            return "?";
	}
}

int hw_domain_request(enum hw_domain dom, uint8_t user)
{
	struct domain *d = &s_dom[dom];

	if (d->enable_pin < 0) {
		return 0;
	}
	if (d->users & user) {
		return 0;
	}
	if (d->users == 0) {
		dom_release_pins(d);
		gpio_pin_configure(hw_gpio0, d->enable_pin, GPIO_OUTPUT_HIGH);
		k_msleep(DOMAIN_SETTLE_MS);

		if (!dom_wait_rail(d, true, RAIL_UP_TIMEOUT_MS)) {
			/* Load-switch or rail fault: back out completely so
			 * nothing drives into the dead rail. */
			dom_park(d);
			gpio_pin_configure(hw_gpio0, d->enable_pin,
					   GPIO_OUTPUT_LOW);
			LOG_ERR("%s domain rail did not come up (P0.%d)",
				dom_name(dom), d->enable_pin);
			if (!d->faulted) {
				char msg[40];
				snprintf(msg, sizeof(msg),
					 "RAIL:%s rail fail", dom_name(dom));
				alert_enqueue(msg, 1);
				d->faulted = true;
			}
			return -EIO;
		}
		d->faulted = false;
		LOG_INF("%s domain on (P0.%d)", dom_name(dom), d->enable_pin);
	}
	d->users |= user;
	return 0;
}

void hw_domain_release(enum hw_domain dom, uint8_t user)
{
	struct domain *d = &s_dom[dom];

	if (d->enable_pin < 0 || !(d->users & user)) {
		return;
	}
	d->users &= ~user;
	if (d->users != 0) {
		return;
	}

	dom_park(d);
	/* v2.5K/v2.6K: the K-line pins' A-side rail is AUX — make sure they
	 * are parked whenever AUX drops, even if the K domain bookkeeping
	 * was skipped. */
	if (dom == HW_DOMAIN_AUX &&
	    IS_ENABLED(CONFIG_APP_BOARD_KLINE_SHIFT_ON_AUX)) {
		dom_park(&s_dom[HW_DOMAIN_K]);
	}
	gpio_pin_configure(hw_gpio0, d->enable_pin, GPIO_OUTPUT_LOW);
	LOG_INF("%s domain off (P0.%d)", dom_name(dom), d->enable_pin);
}

bool hw_domain_is_on(enum hw_domain dom)
{
	return s_dom[dom].enable_pin >= 0 && s_dom[dom].users != 0;
}
