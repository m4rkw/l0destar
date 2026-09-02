/*
 * CAN bench-test agent (CONFIG_APP_CAN_BENCH) — bench builds only.
 *
 * Turns the tracker into a CAN test target driven from the host over the bus
 * itself: the host (can_bench/*.py on the PC, via a gs_usb adapter) sends
 * 8-byte control frames on ID 0x7E0 and the agent answers on ID 0x7E8 with
 * one or more 8-byte "chunks" [op, idx|0x80 on last, 6 payload bytes].
 *
 * Everything else the agent receives counts as a data frame: it is counted,
 * its first four bytes are checked as a little-endian sequence number, and in
 * echo mode it is retransmitted on ID+1.  See can_bench/device.py for the
 * host-side encoding of every command.
 *
 * The SPI link to the MCP2518FD is bit-banged like hw_can.c (slow: Zephyr
 * GPIO API + 1 us waits) but can be switched at run time to a direct-register
 * variant (fast: nrf_gpio, no waits) to measure how much of the controller's
 * bandwidth the production driver leaves on the table.
 *
 * Never returns; enable only in a bench image.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/byteorder.h>
#include <hal/nrf_gpio.h>

#include "app.h"
#include "hw_common.h"
#include "hw_domain.h"
#include "pins.h"

/* --- MCP2518FD registers ---------------------------------------------------- */
#define R_C1CON      0x000
#define R_C1NBTCFG   0x004
#define R_C1DBTCFG   0x008
#define R_C1TDC      0x00C
#define R_C1TBC      0x010
#define R_C1TSCON    0x014
#define R_C1INT      0x01C
#define R_C1TREC     0x034
#define R_C1BDIAG0   0x038
#define R_C1BDIAG1   0x03C
#define R_FIFOCON(n) (0x05C + 12 * ((n) - 1))
#define R_FIFOSTA(n) (0x060 + 12 * ((n) - 1))
#define R_FIFOUA(n)  (0x064 + 12 * ((n) - 1))
#define R_FLTCON0    0x1D0
#define R_FLTOBJ(n)  (0x1F0 + 8 * (n))
#define R_MASK(n)    (0x1F4 + 8 * (n))
#define R_RAM        0x400
#define R_OSC        0xE00
#define R_IOCON      0xE04

#define I_RESET 0x0
#define I_READ  0x3
#define I_WRITE 0x2

#define MODE_NORMAL_FD 0
#define MODE_SLEEP     1
#define MODE_INT_LOOP  2
#define MODE_LISTEN    3
#define MODE_CONFIG    4
#define MODE_EXT_LOOP  5
#define MODE_NORMAL_20 6
#define MODE_RESTRICT  7

#define OSC_OSCDIS  BIT(2)
#define OSC_OSCRDY  BIT(10)
#define IOCON_TRIS0   BIT(0)
#define IOCON_XSTBYEN BIT(6)
#define IOCON_PM0     BIT(24)

#define CTRL_REQ_ID 0x7E0
#define CTRL_RSP_ID 0x7E8

#define TX_FIFO 1
#define RX_FIFO 2

/* control ops */
enum {
	OP_PING = 0x01, OP_STATS, OP_CLEAR, OP_SETMODE, OP_ECHO, OP_BURST,
	OP_SLEEP, OP_RAILCYCLE, OP_SPIBENCH, OP_FILTER, OP_LOOPTEST,
	OP_SETFAST, OP_REBOOT, OP_TXONE,
};

static const uint8_t dlc2len[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };

/* nominal bit timings, 40 MHz SYSCLK (BRP prescaler-1, TSEGx segment-1) */
static const struct { uint8_t brp, tseg1, tseg2, sjw; uint32_t bps; } nbt[] = {
	{ 1, 126, 31, 31,  125000 },
	{ 0, 126, 31, 31,  250000 },
	{ 0,  62, 15, 15,  500000 },
	{ 0,  30,  7,  7, 1000000 },
};
/* data bit timings (FD data phase), TDCO = TSEG1+1 as Microchip recommends */
static const struct { uint8_t brp, tseg1, tseg2, sjw, tdco; uint32_t bps; } dbt[] = {
	/* bit = 1 + (TSEG1+1) + (TSEG2+1) tq of 25 ns */
	{ 0, 30, 7, 7, 31, 1000000 },   /* 40 tq */
	{ 0, 14, 3, 3, 15, 2000000 },   /* 20 tq */
	{ 0,  6, 1, 1,  7, 4000000 },   /* 10 tq */
	{ 0,  4, 1, 1,  5, 5000000 },   /*  8 tq */
	{ 0,  2, 0, 0,  3, 8000000 },   /*  5 tq */
};

static struct {
	uint8_t mode, nom, dat, flags;   /* flags: b0 one-shot, b1 small RX FIFO, b2 no TDC */
	bool echo;
	uint8_t plsize;                  /* PLSIZE code: 0 = 8 bytes, 7 = 64 bytes */
	uint8_t tx_depth, rx_depth;
} cfg;

static struct {
	uint32_t rx_data, rx_bytes, rx_fd, rx_brs, rx_ext, rx_rtr;
	uint32_t seq_missing, seq_bad;
	int64_t  expect;
	uint32_t rxovf, echo_tx, echo_drop, tx_fail, tx_abort;
	uint32_t first_tbc, last_tbc;
	uint32_t int_checks, int_low;
	uint32_t ctrl;
	uint32_t gap_min, gap_max, gaps_over_10ms;   /* inter-arrival (RX timestamps) */
	uint32_t loop_max_us;                        /* longest pause between polls */
	uint32_t loop_last;
} st;

static bool s_fast;

/* Bursts run from the main loop (burst_service) so the RX FIFO keeps being
 * serviced while we transmit — the agent is full duplex like a real node. */
static struct {
	bool active, queued_all;
	uint16_t count, sent, failed, stalls;
	uint8_t len, flags, fill;
	uint32_t id, t0;
	int64_t t_last_progress;
} bx;


/* --- bit-banged SPI (mode 0, MSB first) ------------------------------------ */

static inline void dly(void)
{
	if (!s_fast) {
		k_busy_wait(1);
	}
}

static inline void fast_pause(void)
{
	__asm__ volatile("nop\n nop\n nop\n nop\n");
}

static void cs(int assert)
{
	if (s_fast) {
		if (assert) {
			nrf_gpio_pin_clear(PIN_CAN_CS);
		} else {
			nrf_gpio_pin_set(PIN_CAN_CS);
		}
		fast_pause();
		return;
	}
	gpio_pin_set(hw_gpio0, PIN_CAN_CS, assert ? 0 : 1);
	dly();
}

static uint8_t xfer(uint8_t out)
{
	uint8_t in = 0;

	if (s_fast) {
		for (int i = 7; i >= 0; i--) {
			if ((out >> i) & 1) {
				nrf_gpio_pin_set(PIN_CAN_SDI);
			} else {
				nrf_gpio_pin_clear(PIN_CAN_SDI);
			}
			fast_pause();
			nrf_gpio_pin_set(PIN_CAN_SCK);
			fast_pause();
			in = (in << 1) | (nrf_gpio_pin_read(PIN_CAN_SDO) & 1);
			nrf_gpio_pin_clear(PIN_CAN_SCK);
		}
		return in;
	}
	for (int i = 7; i >= 0; i--) {
		gpio_pin_set(hw_gpio0, PIN_CAN_SDI, (out >> i) & 1);
		dly();
		gpio_pin_set(hw_gpio0, PIN_CAN_SCK, 1);
		in = (in << 1) | (gpio_pin_get(hw_gpio0, PIN_CAN_SDO) & 1);
		dly();
		gpio_pin_set(hw_gpio0, PIN_CAN_SCK, 0);
	}
	return in;
}

static void mcp_reset(void)
{
	cs(1);
	xfer(I_RESET << 4);
	xfer(0);
	cs(0);
	k_msleep(3);
}

static void rdn(uint16_t addr, uint8_t *d, size_t n)
{
	cs(1);
	xfer((I_READ << 4) | ((addr >> 8) & 0x0F));
	xfer(addr & 0xFF);
	for (size_t i = 0; i < n; i++) {
		d[i] = xfer(0);
	}
	cs(0);
}

static void wrn(uint16_t addr, const uint8_t *d, size_t n)
{
	cs(1);
	xfer((I_WRITE << 4) | ((addr >> 8) & 0x0F));
	xfer(addr & 0xFF);
	for (size_t i = 0; i < n; i++) {
		xfer(d[i]);
	}
	cs(0);
}

static uint32_t rd32(uint16_t addr)
{
	uint8_t b[4];

	rdn(addr, b, 4);
	return sys_get_le32(b);
}

static void wr32(uint16_t addr, uint32_t v)
{
	uint8_t b[4];

	sys_put_le32(v, b);
	wrn(addr, b, 4);
}

static void wr8(uint16_t addr, uint8_t v)
{
	wrn(addr, &v, 1);
}

static uint32_t tbc(void)
{
	return rd32(R_C1TBC);
}

static uint8_t opmod(void)
{
	return (rd32(R_C1CON) >> 21) & 0x7;
}

static int set_mode(uint8_t mode, int timeout_ms)
{
	uint32_t con = rd32(R_C1CON);

	con &= ~(0x7u << 24);
	con |= (uint32_t)mode << 24;
	wr32(R_C1CON, con);
	if (mode == MODE_SLEEP) {
		return 0;
	}
	for (int i = 0; i < timeout_ms; i++) {
		if (opmod() == mode) {
			return 0;
		}
		k_msleep(1);
	}
	return -ETIMEDOUT;
}

static void config_xstby(void)
{
	if (!IS_ENABLED(CONFIG_APP_BOARD_CAN_XSTBY)) {
		return;
	}
	uint32_t iocon = rd32(R_IOCON);

	iocon |= IOCON_XSTBYEN | IOCON_PM0;
	iocon &= ~IOCON_TRIS0;
	wr32(R_IOCON, iocon);
}

static int int_pin(void)
{
	if (PIN_CAN_INT < 0) {
		return -1;
	}
	return gpio_pin_get(hw_gpio0, PIN_CAN_INT);
}

static int rail_pin(void)
{
	if (PIN_CAN_RAIL_ST < 0) {
		return -1;
	}
	return gpio_pin_get(hw_gpio0, PIN_CAN_RAIL_ST);
}

/* --- configuration --------------------------------------------------------- */

static void fifo_reset(int n)
{
	wr32(R_FIFOCON(n), rd32(R_FIFOCON(n)) | BIT(10));   /* FRESET */
}

static int enter_config(void)
{
	if (set_mode(MODE_CONFIG, 50) == 0) {
		return 0;
	}
	/* a controller stuck in bus-off/transmit can refuse: hard reset it */
	mcp_reset();
	uint32_t osc = rd32(R_OSC);

	if (!(osc & OSC_OSCRDY)) {
		osc &= ~OSC_OSCDIS;
		wr32(R_OSC, osc);
		for (int i = 0; i < 50 && !(rd32(R_OSC) & OSC_OSCRDY); i++) {
			k_msleep(1);
		}
	}
	config_xstby();
	return set_mode(MODE_CONFIG, 50);
}

static void set_filters(uint16_t sid, uint16_t mask, bool exact_std)
{
	/* filter 0: data traffic (accept-all unless a filter is requested) */
	wr8(R_FLTCON0, 0);                       /* FLTEN0 = 0 while editing */
	wr32(R_FLTOBJ(0), sid);
	wr32(R_MASK(0), exact_std ? ((uint32_t)mask | BIT(30)) : 0);
	/* filter 1: control requests always reach us */
	wr8(R_FLTCON0 + 1, 0);
	wr32(R_FLTOBJ(1), CTRL_REQ_ID);
	wr32(R_MASK(1), 0x7FF | BIT(30));
	wr8(R_FLTCON0, BIT(7) | RX_FIFO);
	wr8(R_FLTCON0 + 1, BIT(7) | RX_FIFO);
}

/* Bring the controller from any state to cfg.* — leaves it in cfg.mode. */
static int apply_config(void)
{
	int err = enter_config();

	if (err) {
		printk("bench: cannot enter config mode (opmod=%d)\n", opmod());
		return err;
	}
	config_xstby();

	uint32_t con = rd32(R_C1CON);

	con &= ~(BIT(19) | BIT(20));            /* STEF, TXQEN off */
	con &= ~(0xFu << 28);                   /* TXBWS = 0 */
	con |= BIT(5);                          /* ISOCRCEN */
	if (cfg.flags & BIT(0)) {
		con |= BIT(16);                 /* RTXAT: honour TXAT */
	} else {
		con &= ~BIT(16);
	}
	wr32(R_C1CON, con);

	const uint8_t n = cfg.nom < ARRAY_SIZE(nbt) ? cfg.nom : 2;
	const uint8_t d = cfg.dat < ARRAY_SIZE(dbt) ? cfg.dat : 1;

	wr32(R_C1NBTCFG, ((uint32_t)nbt[n].brp << 24) | ((uint32_t)nbt[n].tseg1 << 16) |
			 ((uint32_t)nbt[n].tseg2 << 8) | nbt[n].sjw);
	wr32(R_C1DBTCFG, ((uint32_t)dbt[d].brp << 24) | ((uint32_t)dbt[d].tseg1 << 16) |
			 ((uint32_t)dbt[d].tseg2 << 8) | dbt[d].sjw);
	if (cfg.flags & BIT(2)) {
		wr32(R_C1TDC, 0);                                 /* TDC off */
	} else {
		wr32(R_C1TDC, (2u << 16) | ((uint32_t)dbt[d].tdco << 8)); /* auto */
	}
	wr32(R_C1TSCON, BIT(16) | 39);          /* 1 us time base */

	bool fd_payload = (cfg.mode == MODE_NORMAL_FD || cfg.mode == MODE_INT_LOOP ||
			   cfg.mode == MODE_EXT_LOOP || cfg.mode == MODE_LISTEN ||
			   cfg.mode == MODE_RESTRICT);
	cfg.plsize = fd_payload ? 7 : 0;
	cfg.tx_depth = fd_payload ? 4 : 8;
	/* Message RAM is 2 KB.  RX objects carry a timestamp (RXTSEN): 8+4+64 = 76
	 * bytes in FD mode, 20 in classic; TX objects 72 / 16.  FSIZE max 32.
	 * FD: 4*72 + 22*76 = 1960 bytes.  Classic: 8*16 + 32*20 = 768 bytes. */
	cfg.rx_depth = (cfg.flags & BIT(1)) ? 4 : (fd_payload ? 22 : 32);

	uint32_t txcon = ((uint32_t)cfg.plsize << 29) | ((uint32_t)(cfg.tx_depth - 1) << 24) |
			 BIT(7) | ((cfg.flags & BIT(0)) ? 0 : (3u << 21));   /* TXAT */
	wr32(R_FIFOCON(TX_FIFO), txcon);
	wr32(R_FIFOCON(TX_FIFO), txcon | BIT(10));

	uint32_t rxcon = ((uint32_t)cfg.plsize << 29) | ((uint32_t)(cfg.rx_depth - 1) << 24) |
			 BIT(5) | BIT(3) | BIT(0);      /* RXTSEN, RXOVIE, TFNRFNIE -> INT pin */
	wr32(R_FIFOCON(RX_FIFO), rxcon);
	wr32(R_FIFOCON(RX_FIFO), rxcon | BIT(10));

	set_filters(0, 0, false);

	wr32(R_C1INT, BIT(17) | BIT(27) | BIT(30) | BIT(29)); /* RXIE RXOVIE WAKIE CERRIE */
	wr32(R_C1BDIAG0, 0);
	wr32(R_C1BDIAG1, 0);

	err = set_mode(cfg.mode, 50);
	if (err) {
		printk("bench: mode %d refused (opmod=%d)\n", cfg.mode, opmod());
	}
	return err;
}

/* --- frames ----------------------------------------------------------------- */

static uint8_t len2dlc(uint8_t len)
{
	for (uint8_t i = 0; i < 16; i++) {
		if (dlc2len[i] >= len) {
			return i;
		}
	}
	return 15;
}

/* One-shot mode (RTXAT + TXAT=0): after the single attempt fails the
 * controller sets TXATIF and leaves the message in the FIFO, so everything
 * queued behind it is stuck until the FIFO is reset.  A production driver
 * must do this too. */
static bool tx_abort_check(void)
{
	uint32_t sta = rd32(R_FIFOSTA(TX_FIFO));
	uint32_t con = rd32(R_FIFOCON(TX_FIFO));
	bool exhausted = sta & BIT(3);                          /* TXATIF */
	bool stuck = !(sta & BIT(2)) && !(con & BIT(9));        /* not empty, TXREQ clear */

	if (!exhausted && !stuck) {
		return false;
	}
	st.tx_abort++;
	wr8(R_FIFOSTA(TX_FIFO), 0);
	wr32(R_FIFOCON(TX_FIFO), con | BIT(10));                /* FRESET drops the dead message */
	return true;
}

/* Queue one frame in the TX FIFO; waits up to wait_ms for a free slot. */
static int tx_frame(uint32_t id, bool ext, bool fd, bool brs, bool rtr,
		    const uint8_t *data, uint8_t len, int wait_ms)
{
	int waited = 0;

	while (!(rd32(R_FIFOSTA(TX_FIFO)) & BIT(0))) {       /* TFNRFNIF: not full */
		if (tx_abort_check()) {
			continue;
		}
		if (waited >= wait_ms) {
			return -EBUSY;
		}
		k_msleep(1);
		waited++;
	}
	uint16_t addr = R_RAM + (uint16_t)rd32(R_FIFOUA(TX_FIFO));
	uint8_t obj[8 + 64];
	uint32_t t0, t1;
	uint8_t dlc, plen;

	if (ext) {
		t0 = ((id >> 18) & 0x7FF) | ((id & 0x3FFFF) << 11);
	} else {
		t0 = id & 0x7FF;
	}
	if (fd) {
		if (len > 64) {
			len = 64;
		}
		dlc = len2dlc(len);
		plen = dlc2len[dlc];
	} else {
		if (len > 8) {
			len = 8;
		}
		dlc = len;
		plen = rtr ? 0 : len;
	}
	t1 = dlc | (ext ? BIT(4) : 0) | (rtr ? BIT(5) : 0) | (brs ? BIT(6) : 0) |
	     (fd ? BIT(7) : 0);
	sys_put_le32(t0, obj);
	sys_put_le32(t1, obj + 4);
	memset(obj + 8, 0, plen);
	if (data && !rtr) {
		memcpy(obj + 8, data, len);
	}
	size_t total = 8 + ((plen + 3) & ~3u);

	wrn(addr, obj, total);
	wr8(R_FIFOCON(TX_FIFO) + 1, 0x03);                   /* UINC | TXREQ */
	return 0;
}

/* Wait until the TX FIFO has drained; returns 0 when empty. */
static int tx_drain(int wait_ms)
{
	for (int i = 0; i <= wait_ms; i++) {
		if (rd32(R_FIFOSTA(TX_FIFO)) & BIT(2)) {         /* TFERFFIF: empty */
			return 0;
		}
		k_msleep(1);
	}
	return -ETIMEDOUT;
}

static void reply(uint8_t op, const uint8_t *payload, size_t n)
{
	uint8_t chunk[8];
	size_t idx = 0, off = 0;

	do {
		size_t take = n - off > 6 ? 6 : n - off;

		memset(chunk, 0, sizeof(chunk));
		chunk[0] = op;
		chunk[1] = (uint8_t)idx | ((off + take >= n) ? 0x80 : 0);
		memcpy(chunk + 2, payload + off, take);
		if (tx_frame(CTRL_RSP_ID, false, false, false, false, chunk, 8, 200)) {
			st.tx_fail++;
			return;
		}
		off += take;
		idx++;
	} while (off < n);
}

static void clear_stats(void)
{
	memset(&st, 0, sizeof(st));
	st.expect = -1;
	st.gap_min = 0xFFFFFFFF;
	wr32(R_C1BDIAG0, 0);
	wr32(R_C1BDIAG1, 0);
	wr8(R_C1INT + 1, 0);                    /* clear latched flags (bits 8-15) */
	wr8(R_FIFOSTA(TX_FIFO), 0);             /* TXABT/TXLARB/TXERR/TXATIF */
}

/* --- command handlers ------------------------------------------------------- */

static void do_stats(void)
{
	uint8_t p[6 * 9];
	uint32_t trec = rd32(R_C1TREC);
	uint32_t c1int = rd32(R_C1INT);
	uint32_t bd0 = rd32(R_C1BDIAG0);
	uint32_t bd1 = rd32(R_C1BDIAG1);
	uint32_t fsta = rd32(R_FIFOSTA(TX_FIFO));
	int ip = int_pin();

	memset(p, 0, sizeof(p));
	p[0] = (trec >> 8) & 0xFF;                     /* TEC */
	p[1] = trec & 0xFF;                            /* REC */
	p[2] = (trec >> 16) & 0x3F;                    /* EWARN..TXBO */
	p[3] = st.rxovf > 255 ? 255 : st.rxovf;
	p[4] = (c1int >> 8) & 0xFF;                    /* ECCIF..IVMIF */
	p[5] = (ip < 0 ? 0x80 : (ip & 1)) | (st.int_low ? 0x02 : 0) |
	       ((st.int_checks && st.int_low == st.int_checks) ? 0x04 : 0);
	sys_put_le32(st.rx_data, p + 6);
	sys_put_le16(st.seq_missing > 0xFFFF ? 0xFFFF : st.seq_missing, p + 10);
	sys_put_le16(st.seq_bad > 0xFFFF ? 0xFFFF : st.seq_bad, p + 12);
	sys_put_le32(bd0, p + 14);
	sys_put_le16(bd1 >> 16, p + 18);
	sys_put_le16(bd1 & 0xFFFF, p + 20);
	sys_put_le16(st.echo_tx > 0xFFFF ? 0xFFFF : st.echo_tx, p + 22);
	sys_put_le16(st.echo_drop > 0xFFFF ? 0xFFFF : st.echo_drop, p + 24);
	sys_put_le16(st.tx_fail > 0xFFFF ? 0xFFFF : st.tx_fail, p + 26);
	p[28] = st.rx_fd > 255 ? 255 : st.rx_fd;
	p[29] = st.rx_brs > 255 ? 255 : st.rx_brs;
	sys_put_le32(st.last_tbc - st.first_tbc, p + 30);
	p[34] = st.rx_ext > 255 ? 255 : st.rx_ext;
	p[35] = st.rx_rtr > 255 ? 255 : st.rx_rtr;
	sys_put_le32(st.rx_bytes, p + 36);
	p[40] = opmod();
	p[41] = (s_fast ? 1 : 0) | ((fsta >> 4) & 0x7) << 1;   /* TXERR/TXLARB/TXABT */
	sys_put_le16(st.gap_min == 0xFFFFFFFF ? 0 : (st.gap_min > 0xFFFF ? 0xFFFF : st.gap_min), p + 42);
	sys_put_le16(st.gap_max > 0xFFFF ? 0xFFFF : st.gap_max, p + 44);
	p[46] = st.gaps_over_10ms > 255 ? 255 : st.gaps_over_10ms;
	p[47] = st.loop_max_us / 1000 > 255 ? 255 : st.loop_max_us / 1000;   /* ms */
	sys_put_le16(st.tx_abort > 0xFFFF ? 0xFFFF : st.tx_abort, p + 48);

	printk("bench: stats rx=%u miss=%u bad=%u ovf=%u echo=%u/%u txfail=%u "
	       "TEC=%u REC=%u trec=0x%02x bdiag0=0x%08x bdiag1=0x%08x int=0x%02x opmod=%d "
	       "gap=%u..%u us (>10ms: %u) loop_max=%u us\n",
	       st.rx_data, st.seq_missing, st.seq_bad, st.rxovf, st.echo_tx,
	       st.echo_drop, st.tx_fail, p[0], p[1], p[2], bd0, bd1, p[4], p[40],
	       st.gap_min == 0xFFFFFFFF ? 0 : st.gap_min, st.gap_max, st.gaps_over_10ms,
	       st.loop_max_us);
	st.loop_max_us = 0;
	reply(OP_STATS, p, sizeof(p));
}

static void do_setmode(const uint8_t *a)
{
	uint8_t ack[6] = { 1, cfg.mode, cfg.nom, cfg.dat, cfg.flags, 0 };

	reply(OP_SETMODE, ack, 6);
	tx_drain(200);
	k_msleep(20);
	cfg.mode = a[0];
	cfg.nom = a[1];
	cfg.dat = a[2];
	cfg.flags = a[3];
	bx.active = false;
	int err = apply_config();

	printk("bench: setmode mode=%d nom=%u bps dat=%u bps flags=0x%02x -> %s (opmod=%d)\n",
	       cfg.mode, nbt[cfg.nom < ARRAY_SIZE(nbt) ? cfg.nom : 2].bps,
	       dbt[cfg.dat < ARRAY_SIZE(dbt) ? cfg.dat : 1].bps, cfg.flags,
	       err ? "FAILED" : "ok", opmod());
}

static void do_burst(const uint8_t *a)
{
	bx.count = sys_get_le16(a);
	bx.len = a[2];
	bx.flags = a[3];
	bx.id = sys_get_le16(a + 4);
	bx.fill = a[6];
	if (bx.flags & BIT(2)) {
		bx.id |= 0x10000;               /* make it a real 29-bit ID */
	}
	bx.sent = bx.failed = bx.stalls = 0;
	bx.queued_all = false;
	wr8(R_FIFOSTA(TX_FIFO), 0);
	bx.t0 = tbc();
	bx.t_last_progress = k_uptime_get();
	bx.active = true;
}

static void burst_service(void)
{
	if (!bx.active) {
		return;
	}
	bool fd = bx.flags & BIT(0), brs = bx.flags & BIT(1), ext = bx.flags & BIT(2),
	     rtr = bx.flags & BIT(3);
	uint8_t data[64];
	int64_t now = k_uptime_get();

	while (!bx.queued_all) {
		uint16_t i = bx.sent + bx.failed;

		if (i >= bx.count) {
			bx.queued_all = true;
			break;
		}
		if (tx_abort_check()) {
			bx.failed++;
			continue;
		}
		if (!(rd32(R_FIFOSTA(TX_FIFO)) & BIT(0))) {      /* full */
			if (now - bx.t_last_progress > 2000) {
				bx.failed++;                             /* nothing left in 2 s */
				bx.stalls++;
				bx.t_last_progress = now;
				if (bx.stalls > 4) {
					bx.queued_all = true;
				}
			}
			return;
		}
		memset(data, bx.fill, sizeof(data));
		sys_put_le32(i, data);
		if (tx_frame(bx.id, ext, fd, brs, rtr, data, bx.len, 0)) {
			bx.failed++;
		} else {
			bx.sent++;
		}
		bx.t_last_progress = now;
	}
	/* everything queued: wait (non-blocking) for the FIFO to drain */
	if (tx_abort_check()) {
		bx.failed++;
		return;
	}
	bool empty = rd32(R_FIFOSTA(TX_FIFO)) & BIT(2);

	if (!empty && now - bx.t_last_progress < 2000) {
		return;
	}
	uint32_t t1 = tbc();
	uint32_t fsta = rd32(R_FIFOSTA(TX_FIFO));
	uint32_t trec = rd32(R_C1TREC);
	uint8_t p[12];

	bx.active = false;
	sys_put_le16(bx.sent, p);
	sys_put_le16(bx.failed, p + 2);
	sys_put_le32(t1 - bx.t0, p + 4);
	p[8] = (trec >> 8) & 0xFF;
	p[9] = trec & 0xFF;
	p[10] = (fsta >> 4) & 0x7;              /* TXERR(1) TXLARB(2) TXABT(4) */
	p[11] = empty ? 1 : 0;
	printk("bench: burst id=0x%x n=%u len=%u fd=%d brs=%d ext=%d -> sent=%u failed=%u "
	       "%u us drained=%d TEC=%u REC=%u fifo_flags=0x%x\n",
	       bx.id, bx.count, bx.len, fd, brs, ext, bx.sent, bx.failed, t1 - bx.t0, empty,
	       p[8], p[9], p[10]);
	reply(OP_BURST, p, sizeof(p));
	st.loop_last = k_cycle_get_32();
}

static void do_sleep(const uint8_t *a)
{
	uint16_t ms = sys_get_le16(a);
	bool no_xstby = a[2] & BIT(0);
	uint8_t ack[6] = { 1, 0, 0, 0, 0, 0 };

	reply(OP_SLEEP, ack, 6);
	tx_drain(200);
	k_msleep(20);

	uint32_t iocon = rd32(R_IOCON);

	if (no_xstby) {
		wr32(R_IOCON, iocon & ~IOCON_XSTBYEN);
	}
	wr8(R_C1INT + 1, 0);                    /* clear WAKIF etc. */
	int int_before = int_pin();

	enter_config();
	set_mode(MODE_SLEEP, 0);
	int64_t t_sleep = k_uptime_get();
	int32_t int_low_at = -1;

	for (int64_t now = 0; now < ms; now = k_uptime_get() - t_sleep) {
		if (int_low_at < 0 && int_pin() == 0) {
			int_low_at = (int32_t)now;
		}
		k_msleep(2);
	}
	int int_end = int_pin();

	/* wake: dummy CS, re-enable oscillator, wait ready */
	cs(1);
	dly();
	cs(0);
	k_msleep(1);
	uint32_t osc = rd32(R_OSC);
	uint32_t t_w0 = k_cycle_get_32();
	uint32_t osc_before = osc;

	if (osc & OSC_OSCDIS) {
		wr32(R_OSC, osc & ~OSC_OSCDIS);
	}
	for (int i = 0; i < 100 && !((osc = rd32(R_OSC)) & OSC_OSCRDY); i++) {
		k_msleep(1);
	}
	uint32_t wake_us = k_cyc_to_us_floor32(k_cycle_get_32() - t_w0);
	uint32_t c1int = rd32(R_C1INT);
	uint8_t op_after = opmod();

	if (no_xstby) {
		wr32(R_IOCON, iocon);
	}
	int err = apply_config();
	uint8_t p[12];

	p[0] = (c1int >> 14) & 1;               /* WAKIF */
	p[1] = int_low_at >= 0;
	sys_put_le16(int_low_at < 0 ? 0xFFFF : int_low_at, p + 2);
	sys_put_le16(wake_us > 0xFFFF ? 0xFFFF : wake_us, p + 4);
	p[6] = op_after;
	p[7] = (int_before < 0 ? 0x80 : int_before) | (int_end << 1);
	sys_put_le16(osc_before & 0xFFFF, p + 8);
	p[10] = (c1int >> 8) & 0xFF;
	p[11] = err ? 1 : 0;
	printk("bench: sleep %u ms xstby=%d -> WAKIF=%d INT low@%d ms (before=%d end=%d) "
	       "osc_before=0x%04x wake=%u us opmod_after=%d c1int=0x%02x reconfig=%s\n",
	       ms, !no_xstby, p[0], int_low_at, int_before, int_end, osc_before & 0xFFFF,
	       wake_us, op_after, p[10], err ? "FAILED" : "ok");
	reply(OP_SLEEP, p, sizeof(p));
}

static void do_railcycle(const uint8_t *a)
{
	uint16_t off_ms = sys_get_le16(a);
	uint8_t ack[6] = { 1, 0, 0, 0, 0, 0 };

	reply(OP_RAILCYCLE, ack, 6);
	tx_drain(200);
	k_msleep(20);

	int rail_on = rail_pin();

	hw_can_power_off();
	/* sample the rail-sense pin through the off period: the lightly loaded
	 * PP3V3_CAN rail takes a while to decay below the sense threshold */
	int64_t t_off = k_uptime_get();
	int rail_off = rail_pin();
	int32_t rail_low_at = rail_off == 0 ? 0 : -1;

	while (k_uptime_get() - t_off < off_ms) {
		int r = rail_pin();

		if (r == 0 && rail_low_at < 0) {
			rail_low_at = (int32_t)(k_uptime_get() - t_off);
		}
		rail_off = r;
		k_msleep(1);
	}
	int64_t t0 = k_uptime_get();
	int err = hw_can_power_on();
	int64_t t_on = k_uptime_get() - t0;
	int rail_back = rail_pin();
	uint32_t osc = err ? 0xFFFFFFFF : rd32(R_OSC);
	int err2 = err ? -1 : apply_config();
	uint8_t p[12];

	p[0] = (uint8_t)(int8_t)err;
	p[1] = (uint8_t)(int8_t)err2;
	p[2] = (uint8_t)rail_on;
	p[3] = (uint8_t)rail_off;
	p[4] = (uint8_t)rail_back;
	sys_put_le16(t_on > 0xFFFF ? 0xFFFF : (uint16_t)t_on, p + 5);
	sys_put_le16(rail_low_at < 0 ? 0xFFFF : (uint16_t)rail_low_at, p + 7);
	sys_put_le16(osc & 0xFFFF, p + 9);
	p[11] = opmod();
	printk("bench: railcycle off=%u ms -> rail on/off/back=%d/%d/%d (sense low after %d ms) "
	       "power_on=%d (%lld ms) osc=0x%08x reconfig=%d opmod=%d\n",
	       off_ms, rail_on, rail_off, rail_back, rail_low_at, err, t_on, osc, err2, p[11]);
	reply(OP_RAILCYCLE, p, sizeof(p));
}

static void do_spibench(const uint8_t *a)
{
	uint16_t n = sys_get_le16(a);
	uint8_t buf[32], chk[32];
	uint32_t ram_err = 0, rw_err = 0;

	if (n == 0) {
		n = 200;
	}
	uint8_t saved = cfg.mode;

	enter_config();
	/* RAM sweep: 2 KB, four patterns */
	static const uint8_t pats[] = { 0x00, 0xFF, 0x55, 0xAA };

	for (int pi = 0; pi < 5; pi++) {
		for (uint16_t off = 0; off < 2048; off += 32) {
			for (int i = 0; i < 32; i++) {
				buf[i] = pi < 4 ? pats[pi] : (uint8_t)((off + i) * 7 + 3);
			}
			wrn(R_RAM + off, buf, 32);
		}
		for (uint16_t off = 0; off < 2048; off += 32) {
			for (int i = 0; i < 32; i++) {
				buf[i] = pi < 4 ? pats[pi] : (uint8_t)((off + i) * 7 + 3);
			}
			rdn(R_RAM + off, chk, 32);
			for (int i = 0; i < 32; i++) {
				if (chk[i] != buf[i]) {
					ram_err++;
				}
			}
		}
	}
	/* timed 16-byte transactions */
	for (int i = 0; i < 16; i++) {
		buf[i] = (uint8_t)(i * 17 + 1);
	}
	uint32_t c0 = k_cycle_get_32();

	for (uint16_t i = 0; i < n; i++) {
		wrn(R_RAM + 64, buf, 16);
	}
	uint32_t c1 = k_cycle_get_32();

	for (uint16_t i = 0; i < n; i++) {
		rdn(R_RAM + 64, chk, 16);
		if (memcmp(chk, buf, 16)) {
			rw_err++;
		}
	}
	uint32_t c2 = k_cycle_get_32();
	uint32_t wr_us = k_cyc_to_us_floor32(c1 - c0) / n;
	uint32_t rd_us = k_cyc_to_us_floor32(c2 - c1) / n;
	/* 18 bytes on the wire per transaction */
	uint32_t kbps = rd_us ? (18u * 8u * 1000u) / rd_us : 0;
	uint8_t p[12];

	sys_put_le16(ram_err > 0xFFFF ? 0xFFFF : ram_err, p);
	sys_put_le16(rw_err, p + 2);
	sys_put_le16(wr_us > 0xFFFF ? 0xFFFF : wr_us, p + 4);
	sys_put_le16(rd_us > 0xFFFF ? 0xFFFF : rd_us, p + 6);
	sys_put_le16(kbps > 0xFFFF ? 0xFFFF : kbps, p + 8);
	p[10] = s_fast;
	p[11] = 0;
	cfg.mode = saved;
	apply_config();
	printk("bench: spibench fast=%d n=%u ram_err=%u rw_err=%u write16=%u us read16=%u us "
	       "(~%u kbit/s SPI)\n", s_fast, n, ram_err, rw_err, wr_us, rd_us, kbps);
	reply(OP_SPIBENCH, p, sizeof(p));
}

static void do_filter(const uint8_t *a)
{
	uint16_t sid = sys_get_le16(a);
	uint16_t mask = sys_get_le16(a + 2);
	bool all = a[4] & BIT(0);

	set_filters(sid, mask, !all);
	uint8_t p[6] = { 1, all, (uint8_t)sid, (uint8_t)(sid >> 8), (uint8_t)mask, (uint8_t)(mask >> 8) };

	printk("bench: filter %s sid=0x%03x mask=0x%03x\n", all ? "accept-all" : "std", sid, mask);
	reply(OP_FILTER, p, 6);
}

static void do_looptest(const uint8_t *a)
{
	uint8_t mode = a[0];
	uint8_t flags = a[1];
	uint8_t len = a[2];
	uint8_t count = a[3] ? a[3] : 8;
	uint16_t delay_ms = sys_get_le16(a + 4);
	bool fd = flags & BIT(0), brs = flags & BIT(1), ext = flags & BIT(2);
	uint8_t saved = cfg.mode;
	uint8_t txd[64], rxd[64];
	uint8_t sent = 0, got = 0, mism = 0;

	if (delay_ms) {
		/* ack now, run later: lets the host drop to listen-only (or stop)
		 * so it cannot ACK or error-flag the loopback frames */
		uint8_t ack[6] = { 1, 0, 0, 0, 0, 0 };

		reply(OP_LOOPTEST, ack, 6);
		tx_drain(200);
		k_msleep(delay_ms);
	}
	cfg.mode = (mode == MODE_INT_LOOP) ? MODE_INT_LOOP : MODE_EXT_LOOP;
	if (apply_config()) {
		cfg.mode = saved;
		apply_config();
		uint8_t p[12] = { 0 };

		p[11] = 1;
		reply(OP_LOOPTEST, p, 12);
		return;
	}
	uint32_t t0 = tbc();

	for (uint8_t i = 0; i < count; i++) {
		uint32_t id = (ext ? 0x1ABCD00 : 0x300) + i;

		for (int k = 0; k < len; k++) {
			txd[k] = (uint8_t)(i * 31 + k * 7 + 1);
		}
		if (tx_frame(id, ext, fd, brs, false, txd, len, 200)) {
			continue;
		}
		sent++;
		bool ok = false;

		for (int w = 0; w < 200; w++) {
			uint32_t sta = rd32(R_FIFOSTA(RX_FIFO));

			if (!(sta & BIT(0))) {
				k_msleep(1);
				continue;
			}
			uint16_t addr = R_RAM + (uint16_t)rd32(R_FIFOUA(RX_FIFO));
			uint8_t hdr[8];

			rdn(addr, hdr, 8);
			uint32_t r0 = sys_get_le32(hdr), r1 = sys_get_le32(hdr + 4);
			bool rext = r1 & BIT(4), rfd = r1 & BIT(7), rbrs = r1 & BIT(6);
			uint32_t rid = rext ? (((r0 & 0x7FF) << 18) | ((r0 >> 11) & 0x3FFFF)) : (r0 & 0x7FF);
			uint8_t rlen = rfd ? dlc2len[r1 & 0xF] : ((r1 & 0xF) > 8 ? 8 : (r1 & 0xF));

			rdn(addr + 12, rxd, rlen);          /* skip R0 R1 TS */
			wr8(R_FIFOCON(RX_FIFO) + 1, 0x01);
			got++;
			if (rid != id || rfd != fd || rbrs != brs || rext != ext ||
			    rlen != (fd ? dlc2len[len2dlc(len)] : len) ||
			    memcmp(rxd, txd, len)) {
				mism++;
			}
			ok = true;
			break;
		}
		if (!ok) {
			break;
		}
	}
	tx_drain(500);
	uint32_t dur = tbc() - t0;
	uint32_t trec = rd32(R_C1TREC);
	uint32_t bd1 = rd32(R_C1BDIAG1);
	uint32_t fsta = rd32(R_FIFOSTA(TX_FIFO));
	uint8_t p[12];

	p[0] = sent;
	p[1] = got;
	p[2] = mism;
	p[3] = (trec >> 8) & 0xFF;
	p[4] = trec & 0xFF;
	p[5] = (fsta >> 4) & 0x7;
	sys_put_le16(bd1 >> 16, p + 6);
	sys_put_le32(dur, p + 8);
	printk("bench: looptest %s fd=%d brs=%d ext=%d len=%u n=%u -> sent=%u got=%u mismatch=%u "
	       "%u us TEC=%u REC=%u bdiag1=0x%08x fifo=0x%x\n",
	       cfg.mode == MODE_INT_LOOP ? "internal" : "external", fd, brs, ext, len, count,
	       sent, got, mism, dur, p[3], p[4], bd1, p[5]);
	cfg.mode = saved;
	apply_config();
	reply(OP_LOOPTEST, p, sizeof(p));
}

static void do_txone(const uint8_t *a)
{
	/* single frame with explicit shape: [len, flags(fd,brs,ext,rtr), id16, fill] */
	uint8_t len = a[0], flags = a[1], fill = a[4];
	uint32_t id = sys_get_le16(a + 2);
	uint8_t data[64];

	if (flags & BIT(2)) {
		id |= 0x10000;
	}
	for (int i = 0; i < 64; i++) {
		data[i] = (uint8_t)(fill + i);
	}
	int err = tx_frame(id, flags & BIT(2), flags & BIT(0), flags & BIT(1), flags & BIT(3),
			   data, len, 200);
	uint8_t p[6] = { (uint8_t)(int8_t)err, 0, 0, 0, 0, 0 };

	reply(OP_TXONE, p, 6);
}

static void handle_ctrl(const uint8_t *p, uint8_t len)
{
	uint8_t a[8] = { 0 };

	memcpy(a, p, len > 8 ? 8 : len);
	st.ctrl++;
	switch (a[0]) {
	case OP_PING: {
		uint8_t r[6] = { 1, opmod(), s_fast, cfg.mode, cfg.nom, cfg.dat };

		reply(OP_PING, r, 6);
		break;
	}
	case OP_STATS:
		do_stats();
		break;
	case OP_CLEAR: {
		clear_stats();
		uint8_t r[6] = { 1, 0, 0, 0, 0, 0 };

		reply(OP_CLEAR, r, 6);
		break;
	}
	case OP_SETMODE:
		do_setmode(a + 1);
		break;
	case OP_ECHO: {
		cfg.echo = a[1] & 1;
		uint8_t r[6] = { cfg.echo, 0, 0, 0, 0, 0 };

		printk("bench: echo %s\n", cfg.echo ? "on" : "off");
		reply(OP_ECHO, r, 6);
		break;
	}
	case OP_BURST:
		do_burst(a + 1);
		break;
	case OP_SLEEP:
		do_sleep(a + 1);
		break;
	case OP_RAILCYCLE:
		do_railcycle(a + 1);
		break;
	case OP_SPIBENCH:
		do_spibench(a + 1);
		break;
	case OP_FILTER:
		do_filter(a + 1);
		break;
	case OP_LOOPTEST:
		do_looptest(a + 1);
		break;
	case OP_SETFAST: {
		s_fast = a[1] & 1;
		uint8_t r[6] = { s_fast, 0, 0, 0, 0, 0 };

		printk("bench: SPI %s\n", s_fast ? "fast (direct GPIO)" : "slow (Zephyr GPIO API)");
		reply(OP_SETFAST, r, 6);
		break;
	}
	case OP_TXONE:
		do_txone(a + 1);
		break;
	case OP_REBOOT: {
		uint8_t r[6] = { 1, 0, 0, 0, 0, 0 };

		reply(OP_REBOOT, r, 6);
		tx_drain(200);
		printk("bench: reboot\n");
		k_msleep(50);
		sys_reboot(SYS_REBOOT_COLD);
		break;
	}
	default: {
		uint8_t r[6] = { 0xFF, a[0], 0, 0, 0, 0 };

		reply(a[0], r, 6);
		break;
	}
	}
}

/* --- receive path ------------------------------------------------------------ */

static void rx_poll(void)
{
	for (int guard = 0; guard < 256; guard++) {
		uint32_t sta = rd32(R_FIFOSTA(RX_FIFO));

		if (sta & BIT(3)) {                            /* RXOVIF */
			st.rxovf++;
			wr8(R_FIFOSTA(RX_FIFO), 0);
		}
		if (!(sta & BIT(0))) {
			return;
		}
		int ip = int_pin();

		if (ip >= 0) {
			st.int_checks++;
			if (ip == 0) {
				st.int_low++;
			}
		}
		uint16_t addr = R_RAM + (uint16_t)rd32(R_FIFOUA(RX_FIFO));
		uint8_t hdr[16];

		rdn(addr, hdr, 16);                    /* R0 R1 TS + first 4 data bytes */
		uint32_t r0 = sys_get_le32(hdr), r1 = sys_get_le32(hdr + 4);
		uint32_t ts = sys_get_le32(hdr + 8);
		bool ext = r1 & BIT(4), rtr = r1 & BIT(5), brs = r1 & BIT(6), fd = r1 & BIT(7);
		uint32_t id = ext ? (((r0 & 0x7FF) << 18) | ((r0 >> 11) & 0x3FFFF)) : (r0 & 0x7FF);
		uint8_t dlc = r1 & 0xF;
		uint8_t len = fd ? dlc2len[dlc] : (dlc > 8 ? 8 : dlc);
		const uint16_t data_off = 12;

		if (rtr) {
			len = 0;
		}
		if (!ext && id == CTRL_REQ_ID && !rtr) {
			uint8_t p[8] = { 0 };
			uint8_t n = len > 8 ? 8 : len;

			memcpy(p, hdr + data_off, n > 4 ? 4 : n);
			if (n > 4) {
				rdn(addr + data_off + 4, p + 4, n - 4);
			}
			wr8(R_FIFOCON(RX_FIFO) + 1, 0x01);     /* UINC */
			handle_ctrl(p, n);
			st.loop_last = k_cycle_get_32();       /* command time is not a stall */
			continue;
		}
		uint32_t now = ts;

		if (st.rx_data == 0) {
			st.first_tbc = now;
		} else {
			uint32_t gap = now - st.last_tbc;

			if (gap < st.gap_min) {
				st.gap_min = gap;
			}
			if (gap > st.gap_max) {
				st.gap_max = gap;
			}
			if (gap > 10000) {
				st.gaps_over_10ms++;
			}
		}
		st.last_tbc = now;
		st.rx_data++;
		st.rx_bytes += len;
		st.rx_fd += fd;
		st.rx_brs += brs;
		st.rx_ext += ext;
		st.rx_rtr += rtr;
		if (len >= 4) {
			uint32_t seq = sys_get_le32(hdr + data_off);

			if (st.expect >= 0) {
				if (seq > (uint32_t)st.expect) {
					st.seq_missing += seq - (uint32_t)st.expect;
				} else if (seq < (uint32_t)st.expect) {
					st.seq_bad++;
				}
			}
			st.expect = (int64_t)seq + 1;
		}
		if (cfg.echo) {
			uint8_t buf[64];

			memcpy(buf, hdr + data_off, len > 4 ? 4 : len);
			if (len > 4) {
				rdn(addr + data_off + 4, buf + 4, len - 4);
			}
			wr8(R_FIFOCON(RX_FIFO) + 1, 0x01);
			if (tx_frame(id + 1, ext, fd, brs, false, buf, len, 100)) {
				st.echo_drop++;
			} else {
				st.echo_tx++;
			}
		} else {
			wr8(R_FIFOCON(RX_FIFO) + 1, 0x01);
		}
	}
}

/* --- entry ------------------------------------------------------------------- */

void can_bench_run(void)
{
	printk("\n*** CAN BENCH AGENT ***\n");

	/* transceiver self-check first: internal loopback (digital core only)
	 * then external loopback (TXD -> MAX33041E -> RXD) — needs no partner */
	hw_can_selftest();

	int err = hw_can_power_on();

	if (err) {
		printk("bench: hw_can_power_on failed (%d) — halting\n", err);
		for (;;) {
			k_msleep(10000);
		}
	}
	if (PIN_CAN_INT >= 0) {
		gpio_pin_configure(hw_gpio0, PIN_CAN_INT, GPIO_INPUT);
	}
	cfg.mode = MODE_NORMAL_20;
	cfg.nom = 2;            /* 500 kbps */
	cfg.dat = 1;            /* 2 Mbps */
	cfg.flags = 0;
	cfg.echo = false;
	s_fast = false;
	clear_stats();
	err = apply_config();
	printk("bench: %s — CAN 2.0 500 kbps, control 0x%03x -> 0x%03x, OSC=0x%08x rail=%d int=%d\n",
	       err ? "CONFIG FAILED" : "ready", CTRL_REQ_ID, CTRL_RSP_ID, rd32(R_OSC),
	       rail_pin(), int_pin());

	st.loop_last = k_cycle_get_32();
	uint32_t iter = 0;

	for (;;) {
		rx_poll();
		burst_service();
		if ((++iter & 63) == 0 && (cfg.flags & BIT(0))) {
			tx_abort_check();
		}
		uint32_t c = k_cycle_get_32();
		uint32_t us = k_cyc_to_us_floor32(c - st.loop_last);

		if (us > st.loop_max_us) {
			st.loop_max_us = us;
		}
		st.loop_last = c;
		k_yield();
	}
}
