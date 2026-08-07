/*
 * MCP2518FD CAN controller — power/domain handling and low-power control.
 *
 * This is not a bus driver yet: it owns the part-specific safety and power
 * logic so the controller and transceiver are always in a defined state.
 *
 *  - The CAN circuit lives on a switched rail (AUX domain on v2.1/v2.5C/
 *    v2.6C, OBD domain on v3.0).  All SPI pins are parked by hw_domain
 *    while that rail is off — the MCP2518FD's absolute maximum on any I/O
 *    is VDD + 0.3 V, so a driven-high pin would power the dead rail through
 *    the clamp diodes.  CAN_INT's and CAN_CS's 10K pull-ups are on the
 *    switched rail too, hence the parked-input pulldowns.
 *
 *  - On boards with APP_BOARD_CAN_XSTBY the transceiver standby pin (TCAN334
 *    STB / MAX33041 STBY) is wired to MCP2518FD GPIO0.  On v2.5C/v2.6C that
 *    net has no other drive, so it floats until IOCON is configured.  We set
 *    IOCON.XSTBYEN so the MCP drives the transceiver to standby whenever it
 *    is in Sleep mode and back to normal when it wakes.
 *
 *  - On v2.5C/v2.6C the CAN rail is shared with the GPS bias tee (both on
 *    AUX), so during engine-off telemetry wakes the CAN circuit is
 *    unavoidably powered.  The efficiency path is MCP Sleep mode (~10 uA)
 *    plus transceiver standby via XSTBYEN — hw_can_init() leaves the chip
 *    that way, and hw_can_power_off() returns it there.  On v3.0 the OBD
 *    domain is simply switched off.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "hw_common.h"
#include "hw_domain.h"
#include "pins.h"

LOG_MODULE_REGISTER(hw_can, CONFIG_APP_LOG_LEVEL);

#define MCP_REG_C1CON  0x000
#define MCP_REG_OSC    0xE00
#define MCP_REG_IOCON  0xE04

#define MCP_INSTR_RESET 0x0
#define MCP_INSTR_READ  0x3
#define MCP_INSTR_WRITE 0x2

/* C1CON.REQOP / OPMOD values */
#define MCP_MODE_SLEEP  1
#define MCP_MODE_CONFIG 4

#define OSC_OSCRDY      BIT(10)

#define IOCON_TRIS0     BIT(0)
#define IOCON_XSTBYEN   BIT(6)
#define IOCON_PM0       BIT(24)

static bool s_ok;

static bool can_fitted(void)
{
	return IS_ENABLED(CONFIG_APP_BOARD_HAS_CAN) &&
	       PIN_CAN_SCK >= 0 && PIN_CAN_SDI >= 0 &&
	       PIN_CAN_SDO >= 0 && PIN_CAN_CS >= 0;
}

static enum hw_domain can_domain(void)
{
	return IS_ENABLED(CONFIG_APP_BOARD_OBD_DOMAIN) ? HW_DOMAIN_OBD
						       : HW_DOMAIN_AUX;
}

/* --- bit-banged SPI mode 0, MSB first ------------------------------------ */

static inline void spi_dly(void) { k_busy_wait(1); }

static void spi_cs(int assert)
{
	gpio_pin_set(hw_gpio0, PIN_CAN_CS, assert ? 0 : 1);
	spi_dly();
}

static uint8_t spi_xfer(uint8_t out)
{
	uint8_t in = 0;

	for (int i = 7; i >= 0; i--) {
		gpio_pin_set(hw_gpio0, PIN_CAN_SDI, (out >> i) & 1);
		spi_dly();
		gpio_pin_set(hw_gpio0, PIN_CAN_SCK, 1);
		in = (in << 1) | (gpio_pin_get(hw_gpio0, PIN_CAN_SDO) & 1);
		spi_dly();
		gpio_pin_set(hw_gpio0, PIN_CAN_SCK, 0);
	}
	return in;
}

static void spi_pins_active(void)
{
	gpio_pin_configure(hw_gpio0, PIN_CAN_CS,  GPIO_OUTPUT_HIGH);
	gpio_pin_configure(hw_gpio0, PIN_CAN_SCK, GPIO_OUTPUT_LOW);
	gpio_pin_configure(hw_gpio0, PIN_CAN_SDI, GPIO_OUTPUT_LOW);
	gpio_pin_configure(hw_gpio0, PIN_CAN_SDO, GPIO_INPUT);
}

/* Return the SPI pins to the released-idle state hw_domain uses: CS back to
 * input so the board's 10K pull-up holds it deselected without the nRF
 * sourcing into the rail if it later drops. */
static void spi_pins_idle(void)
{
	gpio_pin_configure(hw_gpio0, PIN_CAN_CS,  GPIO_INPUT);
	gpio_pin_configure(hw_gpio0, PIN_CAN_SCK, GPIO_OUTPUT_LOW);
	gpio_pin_configure(hw_gpio0, PIN_CAN_SDI, GPIO_OUTPUT_LOW);
}

static void mcp_reset(void)
{
	spi_cs(1);
	spi_xfer(MCP_INSTR_RESET << 4);
	spi_xfer(0x00);
	spi_cs(0);
	k_msleep(3);
}

static uint32_t mcp_read32(uint16_t addr)
{
	uint32_t v = 0;

	spi_cs(1);
	spi_xfer((MCP_INSTR_READ << 4) | ((addr >> 8) & 0x0F));
	spi_xfer(addr & 0xFF);
	for (int i = 0; i < 4; i++) {
		v |= (uint32_t)spi_xfer(0x00) << (8 * i);   /* little-endian */
	}
	spi_cs(0);
	return v;
}

static void mcp_write32(uint16_t addr, uint32_t v)
{
	spi_cs(1);
	spi_xfer((MCP_INSTR_WRITE << 4) | ((addr >> 8) & 0x0F));
	spi_xfer(addr & 0xFF);
	for (int i = 0; i < 4; i++) {
		spi_xfer((v >> (8 * i)) & 0xFF);
	}
	spi_cs(0);
}

/* Transceiver-standby control via GPIO0/XSTBY.  Re-applied after every rail
 * cycle: the MCP loses its registers whenever its domain powers down. */
static void mcp_config_xstby(void)
{
	if (!IS_ENABLED(CONFIG_APP_BOARD_CAN_XSTBY)) {
		return;
	}
	uint32_t iocon = mcp_read32(MCP_REG_IOCON);

	iocon |= IOCON_XSTBYEN | IOCON_PM0;
	iocon &= ~IOCON_TRIS0;
	mcp_write32(MCP_REG_IOCON, iocon);
}

static int mcp_set_mode(uint8_t mode)
{
	uint32_t con = mcp_read32(MCP_REG_C1CON);

	con &= ~(0x7u << 24);
	con |= (uint32_t)mode << 24;              /* REQOP */
	mcp_write32(MCP_REG_C1CON, con);

	if (mode == MCP_MODE_SLEEP) {
		return 0;                          /* OPMOD unreadable asleep */
	}
	for (int i = 0; i < 20; i++) {
		uint32_t opmod = (mcp_read32(MCP_REG_C1CON) >> 21) & 0x7;
		if (opmod == mode) {
			return 0;
		}
		k_msleep(1);
	}
	return -ETIMEDOUT;
}

/* --- public API ----------------------------------------------------------- */

bool hw_can_available(void)
{
	return s_ok;
}

/* Power the CAN domain and bring the MCP2518FD to Configuration mode. */
int hw_can_power_on(void)
{
	if (!can_fitted()) {
		return -ENODEV;
	}
	hw_domain_request(can_domain(), HW_DOMAIN_USER_CAN);
	spi_pins_active();

	if (!s_ok) {
		hw_domain_release(can_domain(), HW_DOMAIN_USER_CAN);
		return -ENODEV;
	}
	/* A dummy CS pulse wakes the chip from Sleep; then request config. */
	spi_cs(1);
	spi_dly();
	spi_cs(0);
	k_msleep(1);
	int err = mcp_set_mode(MCP_MODE_CONFIG);
	if (err) {
		LOG_ERR("MCP2518FD wake failed");
		return err;
	}
	/* the domain may have power-cycled since init — registers are POR */
	mcp_config_xstby();
	return 0;
}

/* Put the controller (and via XSTBYEN the transceiver) into their lowest
 * defined power state, then drop our claim on the rail.  On v3.0 this powers
 * the OBD domain off entirely; on v2.x the rail belongs to AUX and stays up
 * as long as the main state machine holds it. */
void hw_can_power_off(void)
{
	if (!can_fitted()) {
		return;
	}
	if (s_ok && hw_domain_is_on(can_domain())) {
		spi_pins_active();
		mcp_set_mode(MCP_MODE_SLEEP);
		spi_pins_idle();
	}
	hw_domain_release(can_domain(), HW_DOMAIN_USER_CAN);
}

/* Boot-time bring-up: verify the controller, configure the transceiver
 * standby control, and leave everything in the low-power idle state. */
int hw_can_init(void)
{
	if (!can_fitted()) {
		return 0;
	}

	hw_domain_request(can_domain(), HW_DOMAIN_USER_CAN);
	spi_pins_active();
	mcp_reset();

	uint32_t osc = mcp_read32(MCP_REG_OSC);
	if (osc == 0x00000000 || osc == 0xFFFFFFFF || !(osc & OSC_OSCRDY)) {
		LOG_ERR("MCP2518FD not responding (OSC=0x%08x)", osc);
		spi_pins_idle();
		hw_domain_release(can_domain(), HW_DOMAIN_USER_CAN);
		return -EIO;
	}

	/* GPIO0 drives the transceiver standby pin.  XSTBYEN makes it track
	 * Sleep mode automatically; PM0 GPIO mode + TRIS0 clear give it a
	 * defined push-pull drive (on v2.5/v2.6 the TCAN334 STB net floats
	 * until this runs). */
	mcp_config_xstby();

	/* CAN_INT is open-drain with its pull-up on the switched rail: with
	 * the rail up and no interrupt pending it must read high. */
	if (PIN_CAN_INT >= 0) {
		gpio_pin_configure(hw_gpio0, PIN_CAN_INT, GPIO_INPUT);
		LOG_INF("CAN_INT idle=%d (expect 1)",
			gpio_pin_get(hw_gpio0, PIN_CAN_INT));
	}

	s_ok = true;
	LOG_INF("MCP2518FD ready (OSC=0x%08x)%s — entering sleep", osc,
		IS_ENABLED(CONFIG_APP_BOARD_CAN_XSTBY) ?
		", XSTBY -> transceiver standby" : "");

	mcp_set_mode(MCP_MODE_SLEEP);
	spi_pins_idle();
	hw_domain_release(can_domain(), HW_DOMAIN_USER_CAN);
	return 0;
}
