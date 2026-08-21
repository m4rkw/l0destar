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
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "hw_common.h"
#include "hw_domain.h"
#include "pins.h"

LOG_MODULE_REGISTER(hw_domain, CONFIG_APP_LOG_LEVEL);

/* Rail settle after the enable rises: covers the SiP32431 soft-start, the
 * ITS4060 turn-on and the v2.5K/v2.6K LT8609 5V buck start-up. */
#define DOMAIN_SETTLE_MS 15

enum pin_role {
	ROLE_OUT_LOW,   /* released as OUTPUT_LOW (idle-low outputs) */
	ROLE_IN,        /* released as INPUT no-pull (board pulls define idle) */
};

struct domain_pin {
	int8_t pin;
	uint8_t role;
};

#define DOMAIN_MAX_PINS 12

struct domain {
	int8_t enable_pin;               /* -1 = domain absent on this board */
	uint8_t users;
	uint8_t npins;
	struct domain_pin pins[DOMAIN_MAX_PINS];
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
	 * pull (an internal pulldown would fight the 100K/1M dividers). */
	if (IS_ENABLED(CONFIG_APP_BOARD_HAS_RAIL_SENSE)) {
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

void hw_domain_request(enum hw_domain dom, uint8_t user)
{
	struct domain *d = &s_dom[dom];

	if (d->enable_pin < 0) {
		return;
	}
	if (d->users & user) {
		return;
	}
	if (d->users == 0) {
		dom_release_pins(d);
		gpio_pin_configure(hw_gpio0, d->enable_pin, GPIO_OUTPUT_HIGH);
		k_msleep(DOMAIN_SETTLE_MS);
		LOG_INF("%s domain on (P0.%d)", dom_name(dom), d->enable_pin);
	}
	d->users |= user;
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
