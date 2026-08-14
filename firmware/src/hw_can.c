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

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "hw_common.h"
#include "hw_domain.h"
#include "pins.h"

LOG_MODULE_REGISTER(hw_can, CONFIG_APP_LOG_LEVEL);

#define MCP_REG_C1CON    0x000
#define MCP_REG_C1NBTCFG 0x004
#define MCP_REG_C1INT    0x01C
#define MCP_REG_C1TREC   0x034
#define MCP_REG_C1BDIAG0 0x038
#define MCP_REG_C1BDIAG1 0x03C
#define MCP_REG_C1TXREQ  0x030
#define MCP_REG_C1FIFOCON(n)  (0x05C + 12 * ((n) - 1))
#define MCP_REG_C1FIFOSTA(n)  (0x060 + 12 * ((n) - 1))
#define MCP_REG_C1FIFOUA(n)   (0x064 + 12 * ((n) - 1))
#define MCP_REG_C1FLTCON0 0x1D0
#define MCP_REG_C1FLTOBJ0 0x1F0
#define MCP_REG_C1MASK0   0x1F4
#define MCP_REG_RAM       0x400
#define MCP_REG_OSC       0xE00
#define MCP_REG_IOCON     0xE04

#define MCP_INSTR_RESET 0x0
#define MCP_INSTR_READ  0x3
#define MCP_INSTR_WRITE 0x2

/* C1CON.REQOP / OPMOD values */
#define MCP_MODE_NORMAL_2_0  6
#define MCP_MODE_SLEEP       1
#define MCP_MODE_CONFIG      4
#define MCP_MODE_INT_LOOP    2
#define MCP_MODE_EXT_LOOP    5

#define OSC_OSCRDY      BIT(10)
#define OSC_OSCDIS      BIT(2)

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

	/* Clear OSCDIS (set during sleep) and wait for the crystal to start. */
	uint32_t osc = mcp_read32(MCP_REG_OSC);
	if (osc & OSC_OSCDIS) {
		osc &= ~OSC_OSCDIS;
		mcp_write32(MCP_REG_OSC, osc);
		for (int i = 0; i < 50; i++) {
			osc = mcp_read32(MCP_REG_OSC);
			if (osc & OSC_OSCRDY) {
				break;
			}
			k_msleep(1);
		}
	}

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

/* --- CAN bus test -------------------------------------------------------- */

static void mcp_write_n(uint16_t addr, const uint8_t *data, size_t len)
{
	spi_cs(1);
	spi_xfer((MCP_INSTR_WRITE << 4) | ((addr >> 8) & 0x0F));
	spi_xfer(addr & 0xFF);
	for (size_t i = 0; i < len; i++) {
		spi_xfer(data[i]);
	}
	spi_cs(0);
}

static void mcp_read_n(uint16_t addr, uint8_t *data, size_t len)
{
	spi_cs(1);
	spi_xfer((MCP_INSTR_READ << 4) | ((addr >> 8) & 0x0F));
	spi_xfer(addr & 0xFF);
	for (size_t i = 0; i < len; i++) {
		data[i] = spi_xfer(0x00);
	}
	spi_cs(0);
}

int hw_can_test(void)
{
	if (!can_fitted()) {
		printk("CAN test: no CAN hardware fitted\n");
		return -ENODEV;
	}

	printk("\n*** CAN BUS TEST ***\n");
	printk("start the host script now, sending first frame in 3s...\n");

	int err = hw_can_power_on();
	if (err) {
		printk("FAIL: hw_can_power_on: %d\n", err);
		return err;
	}

	/* Reset to get clean POR register state, then re-enable oscillator. */
	mcp_reset();
	uint32_t osc = mcp_read32(MCP_REG_OSC);
	if (!(osc & OSC_OSCRDY)) {
		osc &= ~OSC_OSCDIS;
		mcp_write32(MCP_REG_OSC, osc);
		for (int i = 0; i < 50; i++) {
			osc = mcp_read32(MCP_REG_OSC);
			if (osc & OSC_OSCRDY) break;
			k_msleep(1);
		}
	}
	mcp_config_xstby();

	/* Disable TEF and TXQ — we don't need them and they complicate the
	 * RAM layout.  Must be written while in Configuration mode. */
	uint32_t c1con = mcp_read32(MCP_REG_C1CON);
	c1con &= ~(BIT(19) | BIT(20));   /* clear STEF and TXQEN */
	mcp_write32(MCP_REG_C1CON, c1con);

	/* Bit timing for 500 kbps with 40 MHz clock.
	 * Tq = 1/40M = 25 ns.  Want bit time = 2 µs = 80 Tq.
	 * BRP=0 (prescaler 1), TSEG1=62, TSEG2=15, SJW=15.
	 * Bit time = 1 + (TSEG1+1) + (TSEG2+1) = 1 + 63 + 16 = 80 Tq.
	 * Sample point at (1 + 63) / 80 = 80%. */
	uint32_t nbtcfg = (0u << 24)     /* BRP = 0 (prescaler 1) */
			| (62u << 16)    /* TSEG1 = 62 */
			| (15u << 8)     /* TSEG2 = 15 */
			| (15u);         /* SJW = 15 */
	mcp_write32(MCP_REG_C1NBTCFG, nbtcfg);

	/* FIFO 1: TX, 4 messages deep, 8 byte payload */
	uint32_t txcon = (3u << 24)    /* FSIZE = 3 (4 messages) */
		       | (0u << 29)    /* PLSIZE = 0 (8 bytes) */
		       | BIT(7)        /* TXEN = 1 (transmit FIFO) */
		       | (0u << 5);    /* TXPRI = 0 */
	mcp_write32(MCP_REG_C1FIFOCON(1), txcon);
	mcp_write32(MCP_REG_C1FIFOCON(1), txcon | BIT(10)); /* FRESET */

	/* FIFO 2: RX, 4 messages deep, 8 byte payload */
	uint32_t rxcon = (3u << 24)    /* FSIZE = 3 (4 messages) */
		       | (0u << 29);   /* PLSIZE = 0, TXEN = 0 (receive) */
	mcp_write32(MCP_REG_C1FIFOCON(2), rxcon);
	mcp_write32(MCP_REG_C1FIFOCON(2), rxcon | BIT(10)); /* FRESET */

	/* Filter 0: accept all standard frames, route to FIFO 2 */
	mcp_write32(MCP_REG_C1FLTOBJ0, 0x00000000);
	mcp_write32(MCP_REG_C1MASK0, 0x00000000);
	uint32_t fltcon = mcp_read32(MCP_REG_C1FLTCON0);
	fltcon &= 0xFFFFFF00;
	fltcon |= BIT(7)   /* FLTEN0 = enable */
		| 2;       /* F0BP = FIFO 2 */
	mcp_write32(MCP_REG_C1FLTCON0, fltcon);

	/* Switch to CAN 2.0 Normal mode */
	err = mcp_set_mode(MCP_MODE_NORMAL_2_0);
	if (err) {
		printk("FAIL: could not enter Normal 2.0 mode\n");
		hw_can_power_off();
		return err;
	}

	k_msleep(3000);

	/* --- TX: send a PING frame (ID 0x100, data "PING") --- */
	uint32_t fifoua = mcp_read32(MCP_REG_C1FIFOUA(1));
	uint16_t ram_base = (uint16_t)(fifoua + MCP_REG_RAM);

	/* TX message object: T0, T1, then data bytes (little-endian words).
	 * MCP2518FD RAM format: T0 bits 10:0 = SID, T1 bits 3:0 = DLC. */
	uint8_t txobj[16];
	memset(txobj, 0, sizeof(txobj));
	uint32_t t0 = 0x100u;                     /* SID = 0x100 */
	uint32_t t1 = 4u;                          /* DLC = 4 */
	txobj[0] = t0 & 0xFF;
	txobj[1] = (t0 >> 8) & 0xFF;
	txobj[2] = (t0 >> 16) & 0xFF;
	txobj[3] = (t0 >> 24) & 0xFF;
	txobj[4] = t1 & 0xFF;
	txobj[5] = (t1 >> 8) & 0xFF;
	txobj[6] = (t1 >> 16) & 0xFF;
	txobj[7] = (t1 >> 24) & 0xFF;
	txobj[8] = 'P'; txobj[9] = 'I'; txobj[10] = 'N'; txobj[11] = 'G';

	mcp_write_n(ram_base, txobj, 16);

	/* UINC then TXREQ as separate writes */
	uint32_t fifocon = mcp_read32(MCP_REG_C1FIFOCON(1));
	fifocon |= BIT(8);   /* UINC */
	mcp_write32(MCP_REG_C1FIFOCON(1), fifocon);
	fifocon = mcp_read32(MCP_REG_C1FIFOCON(1));
	fifocon |= BIT(9);   /* TXREQ */
	mcp_write32(MCP_REG_C1FIFOCON(1), fifocon);

	printk("TX -> ID=0x100 [PING]\n");

	/* Wait for TXREQ to clear (TX done) or TXABT (error) */
	for (int i = 0; i < 100; i++) {
		fifocon = mcp_read32(MCP_REG_C1FIFOCON(1));
		uint32_t sta = mcp_read32(MCP_REG_C1FIFOSTA(1));
		if (!(fifocon & BIT(9)))
			break;
		if (sta & BIT(2)) {          /* TXABT */
			printk("FAIL: TX aborted (bus error)\n");
			break;
		}
		if (i == 99)
			printk("FAIL: TX timeout\n");
		k_msleep(10);
	}

	/* --- RX: wait for PONG reply (ID 0x101) --- */
	printk("waiting for reply (ID=0x101)...\n");
	bool got_reply = false;

	for (int i = 0; i < 500; i++) {
		uint32_t sta = mcp_read32(MCP_REG_C1FIFOSTA(2));
		if (sta & BIT(0)) {  /* TFNRFNIF: not empty */
			uint32_t fifoua2 = mcp_read32(MCP_REG_C1FIFOUA(2));
			uint16_t rx_addr = (uint16_t)(fifoua2 + MCP_REG_RAM);

			uint8_t rxobj[16];
			mcp_read_n(rx_addr, rxobj, 16);

			uint32_t r0 = rxobj[0] | (rxobj[1] << 8)
				    | (rxobj[2] << 16) | (rxobj[3] << 24);
			uint32_t r1 = rxobj[4] | (rxobj[5] << 8)
				    | (rxobj[6] << 16) | (rxobj[7] << 24);
			uint16_t sid = r0 & 0x7FF;
			uint8_t  dlc = r1 & 0xF;
			int dlen = dlc <= 8 ? dlc : 8;

			printk("RX <- ID=0x%03x DLC=%d\n", sid, dlc);

			/* Advance FIFO read pointer */
			uint32_t fc = mcp_read32(MCP_REG_C1FIFOCON(2));
			fc |= BIT(8);  /* UINC */
			mcp_write32(MCP_REG_C1FIFOCON(2), fc);

			if (sid == 0x101 && dlen >= 4 &&
			    rxobj[8] == 'P' && rxobj[9] == 'O' &&
			    rxobj[10] == 'N' && rxobj[11] == 'G') {
				got_reply = true;
				break;
			}
		}
		k_msleep(10);
	}

	if (got_reply) {
		printk("PASS: CAN bus test passed\n");
	} else {
		printk("FAIL: no PONG reply received\n");
	}

	printk("*** CAN BUS TEST DONE ***\n\n");

	hw_can_power_off();
	return got_reply ? 0 : -ETIMEDOUT;
}
