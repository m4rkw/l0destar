/*
 * MX25R6435F on bit-banged SPI.  See extflash.h; the commands, bit positions
 * and timings are from the Macronix datasheet and were checked against its
 * Rev. 1.2 (October 2015) on 2026-11-05.
 *
 * Bit-banged, like the CAN controller's bus and for the same reason: pins come
 * from Kconfig, no serial peripheral instance is spoken for, and nothing about
 * the bus depends on devicetree.  At a hundred kilobytes a second it moves a
 * 300-byte record in a few milliseconds, and the chip's own program time
 * (0.85 to 10 ms a page, by power mode) is longer than that.
 *
 * WHAT THIS WILL AND WILL NOT SEND THE CHIP
 * -----------------------------------------
 * Eight commands, all below: read, page program, 4 KiB sector erase, write
 * enable, the two status reads, the ID read and deep power-down.  Nothing that
 * writes the status or configuration register (its block-protect bits and
 * write-protect mode are non-volatile), nothing that touches the security
 * register or the one-time-programmable area (whose lock-down is permanent),
 * no block or chip erase.  So there is nothing in this file that can lock the
 * part, change what it protects or erase more than the one sector asked for;
 * tests/test_extflash.py keeps that list closed.
 *
 * What it does for the wear policy in flashlog.h: it never programs across a
 * page (the chip would wrap round inside it and overwrite the start of the
 * same page), checks the chip's own program and erase fail flags after every
 * operation, refuses to start if any block is protected (a program would be
 * dropped and an erase refused, one at a time, for ever), and bounds every wait,
 * so a chip that has stopped answering costs a timeout and an error, not a loop.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "extflash.h"
#include "hw_common.h"
#include "hw_domain.h"
#include "pins.h"

LOG_MODULE_REGISTER(extflash, CONFIG_APP_LOG_LEVEL);

BUILD_ASSERT(PIN_FLASH_SCK >= 0 && PIN_FLASH_SDI >= 0 &&
	     PIN_FLASH_SDO >= 0 && PIN_FLASH_CS >= 0,
	     "CONFIG_APP_FLASH_BACKLOG needs APP_PIN_FLASH_SCK, _SDI, _SDO and _CS");

/* Whether the bus is the CAN controller's.  The MCP2518FD is on a switched rail
 * and clamps its I/O to it, so the bus may only be driven while that rail is up
 * (extflash.h, SHARED BUS).  Any one of the three pins being shared is enough:
 * a shared clock alone drives the controller's input. */
#define BUS_SHARED_WITH_CAN \
	(PIN_FLASH_SCK == PIN_CAN_SCK || PIN_FLASH_SDI == PIN_CAN_SDI || \
	 PIN_FLASH_SDO == PIN_CAN_SDO)

/* The only commands this file sends (datasheet Table 5). */
#define CMD_READ    0x03
#define CMD_PP      0x02
#define CMD_SE      0x20
#define CMD_WREN    0x06
#define CMD_RDSR    0x05
#define CMD_RDSCUR  0x2B
#define CMD_RDID    0x9F
#define CMD_DP      0xB9

/* Status register: WIP bit 0, WEL bit 1, block protect BP3..BP0 bits 5..2. */
#define SR_WIP      0x01
#define SR_WEL      0x02
#define SR_BP_MASK  0x3C
/* Security register: program fail bit 5, erase fail bit 6. */
#define SCUR_P_FAIL 0x20
#define SCUR_E_FAIL 0x40

#define ID_MANUF    0xC2
#define ID_TYPE     0x28
#define ID_DENSITY  0x17        /* 64 Mbit */

#define CHIP_SECTORS 2048u
/* The last sectors of the chip, kept for log lines (not used yet).  The
 * wear counter is worked out from the sector count, so the split is fixed
 * before any unit ships. */
#define LOG_SECTORS  32u
#define POS_SECTORS  (CHIP_SECTORS - LOG_SECTORS)
#define SECTOR_BYTES FLASHLOG_SECTOR_SIZE
#define PAGE_BYTES   FLASHLOG_PAGE_SIZE
#define CHIP_BYTES   (CHIP_SECTORS * SECTOR_BYTES)

BUILD_ASSERT(CONFIG_APP_FLASH_BACKLOG_SECTORS <= POS_SECTORS,
	     "APP_FLASH_BACKLOG_SECTORS leaves out the sectors kept for log lines");

/* Waits.  The datasheet's maxima are 10 ms for a page program (ultra-low-power
 * mode; 4 ms in high-performance mode) and 240 ms for a sector erase (both);
 * these allow a generous multiple. */
#define PROG_TIMEOUT_MS   40
#define ERASE_TIMEOUT_MS  1000
#define BUSY_TIMEOUT_MS   1000

/* tDP 10 us to enter deep power-down, then CS must stay high tDPDD 30 us; a CS
 * pulse wakes it and it is ready tRDP (35 to 45 us) after.  Doubled. */
#define DP_ENTER_US   60
#define DP_WAKE_US    100

static inline void sck(int v) { gpio_pin_set_raw(hw_gpio0, PIN_FLASH_SCK, v); }
static inline void sdi(int v) { gpio_pin_set_raw(hw_gpio0, PIN_FLASH_SDI, v); }
static inline void cs(int v)  { gpio_pin_set_raw(hw_gpio0, PIN_FLASH_CS, v); }

/* Mode 0, MSB first, as hw_can.c. */
static void spi_write_byte(uint8_t b)
{
	for (int i = 7; i >= 0; i--) {
		sdi((b >> i) & 1);
		sck(1);
		sck(0);
	}
}

static uint8_t spi_read_byte(void)
{
	uint8_t v = 0;

	for (int i = 0; i < 8; i++) {
		sck(1);
		v = (uint8_t)((v << 1) | (gpio_pin_get_raw(hw_gpio0, PIN_FLASH_SDO) & 1));
		sck(0);
	}
	return v;
}

static void spi_write_cmd_addr(uint8_t cmd, uint32_t addr)
{
	spi_write_byte(cmd);
	spi_write_byte((uint8_t)(addr >> 16));
	spi_write_byte((uint8_t)(addr >> 8));
	spi_write_byte((uint8_t)addr);
}

static uint8_t read_reg(uint8_t cmd)
{
	uint8_t v;

	cs(0);
	spi_write_byte(cmd);
	sdi(0);
	v = spi_read_byte();
	cs(1);
	return v;
}

static uint8_t rdsr(void)   { return read_reg(CMD_RDSR); }
static uint8_t rdscur(void) { return read_reg(CMD_RDSCUR); }

/* Until the chip has finished what it is doing.  Bounded: a chip that is not
 * there reads as all ones (busy for ever) or all zeros (never busy), and the
 * caller's own checks catch the second. */
static int wait_ready(int timeout_ms)
{
	const int64_t end = k_uptime_get() + timeout_ms;

	for (;;) {
		if (!(rdsr() & SR_WIP)) {
			return 0;
		}
		if (k_uptime_get() >= end) {
			return -ETIMEDOUT;
		}
		k_msleep(1);
	}
}

static int write_enable(void)
{
	cs(0);
	spi_write_byte(CMD_WREN);
	cs(1);
	/* WEL stays clear if the chip is absent, protected or still busy. */
	return (rdsr() & SR_WEL) ? 0 : -EACCES;
}

static int dev_read(void *ctx, uint32_t addr, void *buf, size_t len)
{
	uint8_t *p = buf;

	ARG_UNUSED(ctx);
	if (len == 0 || addr >= CHIP_BYTES || len > CHIP_BYTES - addr) {
		return -EINVAL;
	}
	cs(0);
	spi_write_cmd_addr(CMD_READ, addr);
	sdi(0);
	for (size_t i = 0; i < len; i++) {
		p[i] = spi_read_byte();
	}
	cs(1);
	return 0;
}

static int dev_prog(void *ctx, uint32_t addr, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	int err;

	ARG_UNUSED(ctx);
	/* A program that runs past the end of a page wraps round inside it and
	 * overwrites the start of the same page.  flashlog never asks for one. */
	if (len == 0 || addr >= CHIP_BYTES || (addr % PAGE_BYTES) + len > PAGE_BYTES) {
		return -EINVAL;
	}
	err = write_enable();
	if (err) {
		return err;
	}
	cs(0);
	spi_write_cmd_addr(CMD_PP, addr);
	for (size_t i = 0; i < len; i++) {
		spi_write_byte(p[i]);
	}
	cs(1);
	err = wait_ready(PROG_TIMEOUT_MS);
	if (err) {
		return err;
	}
	return (rdscur() & SCUR_P_FAIL) ? -EIO : 0;
}

static int dev_erase(void *ctx, uint32_t addr)
{
	int err;

	ARG_UNUSED(ctx);
	if ((addr % SECTOR_BYTES) != 0 || addr >= CHIP_BYTES) {
		return -EINVAL;
	}
	err = write_enable();
	if (err) {
		return err;
	}
	cs(0);
	spi_write_cmd_addr(CMD_SE, addr);
	cs(1);
	err = wait_ready(ERASE_TIMEOUT_MS);
	if (err) {
		return err;
	}
	return (rdscur() & SCUR_E_FAIL) ? -EIO : 0;
}

static struct flashlog_dev s_dev = {
	.read = dev_read,
	.prog = dev_prog,
	.erase = dev_erase,
};

const struct flashlog_dev *extflash_dev(uint32_t sectors)
{
	s_dev.sectors = sectors;
	return &s_dev;
}

int extflash_begin(void)
{
	if (BUS_SHARED_WITH_CAN && !hw_domain_is_on(hw_can_domain())) {
		return -EBUSY;
	}

	gpio_pin_configure(hw_gpio0, PIN_FLASH_CS, GPIO_OUTPUT_HIGH);
	gpio_pin_configure(hw_gpio0, PIN_FLASH_SCK, GPIO_OUTPUT_LOW);
	gpio_pin_configure(hw_gpio0, PIN_FLASH_SDI, GPIO_OUTPUT_LOW);
	gpio_pin_configure(hw_gpio0, PIN_FLASH_SDO, GPIO_INPUT);

	/* Out of deep power-down: a chip select pulse, then tRDP.  Always, not
	 * only after our own DP: the chip keeps that state across a reset of the
	 * nRF, and a command sent to it asleep is ignored without a word. */
	cs(0);
	cs(1);
	k_busy_wait(DP_WAKE_US);
	return 0;
}

void extflash_end(void)
{
	cs(0);
	spi_write_byte(CMD_DP);
	cs(1);
	k_busy_wait(DP_ENTER_US);

	/* The idle state hw_domain.c leaves the CAN bus in: clocks low, data
	 * low, the chip's output an input.  The chip select stays high. */
	gpio_pin_configure(hw_gpio0, PIN_FLASH_SCK, GPIO_OUTPUT_LOW);
	gpio_pin_configure(hw_gpio0, PIN_FLASH_SDI, GPIO_OUTPUT_LOW);
	gpio_pin_configure(hw_gpio0, PIN_FLASH_SDO, GPIO_INPUT);
	gpio_pin_configure(hw_gpio0, PIN_FLASH_CS, GPIO_OUTPUT_HIGH);
}

void extflash_park(void)
{
	if (extflash_begin() == 0) {
		extflash_end();
	}
}

/* The CAN rail is up (first user, pins released): the flash can be reached. */
static void can_rail_notify(bool up)
{
	if (up) {
		extflash_park();
	}
}

void extflash_init(void)
{
	if (BUS_SHARED_WITH_CAN) {
		/* Called at once if the rail is already up. */
		hw_domain_notify_set(hw_can_domain(), can_rail_notify);
		return;
	}
	extflash_park();
}

/* RDID, and whether the three bytes are this chip's. */
static bool read_id(uint8_t id[3])
{
	cs(0);
	spi_write_byte(CMD_RDID);
	sdi(0);
	for (int i = 0; i < 3; i++) {
		id[i] = spi_read_byte();
	}
	cs(1);
	return id[0] == ID_MANUF && id[1] == ID_TYPE && id[2] == ID_DENSITY;
}

int extflash_check(void)
{
	uint8_t id[3];

	return read_id(id) ? 0 : -ENODEV;
}

int extflash_probe(uint32_t *sectors)
{
	uint8_t id[3];
	uint8_t sr;
	int err;

	if (!read_id(id)) {
		LOG_ERR("flash id %02x %02x %02x, expected %02x %02x %02x "
			"(MX25R6435F)", id[0], id[1], id[2],
			ID_MANUF, ID_TYPE, ID_DENSITY);
		return -ENODEV;
	}

	/* A write or erase the last boot did not wait for: it finishes by itself. */
	err = wait_ready(BUSY_TIMEOUT_MS);
	if (err) {
		return err;
	}
	sr = rdsr();
	if (sr & SR_BP_MASK) {
		LOG_ERR("flash block-protected (status %02x): no writes", sr);
		return -EACCES;
	}

	*sectors = MIN((uint32_t)CONFIG_APP_FLASH_BACKLOG_SECTORS, POS_SECTORS);
	return 0;
}
