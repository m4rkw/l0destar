#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_APP_BOARD_HAS_L_SENSE)
#include <nrfx_saadc.h>
#endif

#include "app.h"
#include "hw_common.h"
#include "hw_domain.h"
#include "pins.h"

LOG_MODULE_REGISTER(hw_kline, CONFIG_APP_LOG_LEVEL);

#define KLINE_BIT_US 96

/* Which switched domain the K-line transceiver sits on: v3.0 shares OBD_EN
 * with CAN, v3.1 has its own K_EN gating PP3V3_K + PP12V_K. */
static enum hw_domain kline_domain(void)
{
	return IS_ENABLED(CONFIG_APP_BOARD_SPLIT_OBD_DOMAIN) ? HW_DOMAIN_K
							     : HW_DOMAIN_OBD;
}

/* Bring up the rails the K-line circuit needs on this board, releasing the
 * K pins from their parked state.  Domain wiring per board:
 *   bench / v2.1        everything on the AUX domain
 *   v2.5K / v2.6K       shifter A-side on AUX, L9637D 5V/12V rails on K_EN —
 *                       both must be up before the pins are released
 *   v3.0                TJA1027T on the shared OBD domain; SLP_N (K_SLEEP)
 *                       must then be raised to wake the transceiver
 *   v3.1                TJA1027T on its own K_EN domain (PP3V3_K + PP12V_K);
 *                       same SLP_N handling
 */
int kline_power_on(void)
{
	if (!IS_ENABLED(CONFIG_APP_BOARD_HAS_KLINE)) {
		return -ENODEV;
	}
	if (IS_ENABLED(CONFIG_APP_BOARD_OBD_DOMAIN) ||
	    IS_ENABLED(CONFIG_APP_BOARD_SPLIT_OBD_DOMAIN)) {
		if (hw_domain_request(kline_domain(), HW_DOMAIN_USER_KLINE)) {
			/* PP12V_K feeds the TJA1027T: with it down, driving
			 * SLP_N or TXD high backfeeds the transceiver's ESD
			 * clamps.  Leave the pins parked. */
			LOG_ERR("K-line rails unavailable — staying parked");
			return -EIO;
		}
		if (PIN_K_SLEEP >= 0) {
			gpio_pin_configure(hw_gpio0, PIN_K_SLEEP,
					   GPIO_OUTPUT_HIGH);
		}
	} else {
		if (hw_domain_request(HW_DOMAIN_AUX, HW_DOMAIN_USER_KLINE)) {
			return -EIO;
		}
		if (IS_ENABLED(CONFIG_APP_BOARD_KLINE_SHIFT_ON_AUX) &&
		    hw_domain_request(HW_DOMAIN_K, HW_DOMAIN_USER_KLINE)) {
			hw_domain_release(HW_DOMAIN_AUX, HW_DOMAIN_USER_KLINE);
			return -EIO;
		}
	}
	k_msleep(500);		/* rails + K bus settle */
	return 0;
}

/* Park the K pins and drop the rails this board lets us drop.  The TJA1027
 * is put to sleep (K_SLEEP low, via the park) before its rail is cut. */
void kline_power_off(void)
{
	if (!IS_ENABLED(CONFIG_APP_BOARD_HAS_KLINE)) {
		return;
	}
	LOG_INF("K-line power off");
	if (IS_ENABLED(CONFIG_APP_BOARD_OBD_DOMAIN) ||
	    IS_ENABLED(CONFIG_APP_BOARD_SPLIT_OBD_DOMAIN)) {
		if (PIN_K_SLEEP >= 0) {
			gpio_pin_configure(hw_gpio0, PIN_K_SLEEP,
					   GPIO_OUTPUT_LOW);
		}
		hw_domain_release(kline_domain(), HW_DOMAIN_USER_KLINE);
	} else {
		if (IS_ENABLED(CONFIG_APP_BOARD_KLINE_SHIFT_ON_AUX)) {
			hw_domain_release(HW_DOMAIN_K, HW_DOMAIN_USER_KLINE);
		}
		hw_domain_release(HW_DOMAIN_AUX, HW_DOMAIN_USER_KLINE);
	}
}

int kline_init(void)
{
	if (!IS_ENABLED(CONFIG_APP_BOARD_HAS_KLINE) || PIN_K1_TX < 0) {
		return -ENODEV;
	}
	int err = kline_power_on();
	if (err) {
		return err;
	}
	gpio_pin_configure(hw_gpio0, PIN_K1_TX, GPIO_OUTPUT_HIGH);
	gpio_pin_configure(hw_gpio0, PIN_K1_RX, GPIO_INPUT | GPIO_PULL_UP);
	k_msleep(100);
	LOG_INF("K-line configured (TX=P0.%d RX=P0.%d)",
		PIN_K1_TX, PIN_K1_RX);
	return 0;
}

uint8_t kline_tx_rx_byte(uint8_t tx)
{
	uint8_t rx = 0;

	gpio_pin_set(hw_gpio0, PIN_K1_TX, 0);
	k_busy_wait(KLINE_BIT_US);

	for (int i = 0; i < 8; i++) {
		gpio_pin_set(hw_gpio0, PIN_K1_TX, (tx >> i) & 1);
		k_busy_wait(KLINE_BIT_US / 2);
		rx |= (gpio_pin_get(hw_gpio0, PIN_K1_RX) & 1) << i;
		k_busy_wait(KLINE_BIT_US / 2);
	}

	gpio_pin_set(hw_gpio0, PIN_K1_TX, 1);
	k_busy_wait(KLINE_BIT_US);
	return rx;
}

int kline_self_test(void)
{
	int rx_idle = gpio_pin_get(hw_gpio0, PIN_K1_RX);
	LOG_INF("K-line diag: TX=P0.%d RX=P0.%d", PIN_K1_TX, PIN_K1_RX);
	LOG_INF("  idle: RX=%d (expect 1)", rx_idle);

	gpio_pin_set(hw_gpio0, PIN_K1_TX, 0);
	k_busy_wait(200);
	int rx_low = gpio_pin_get(hw_gpio0, PIN_K1_RX);
	gpio_pin_set(hw_gpio0, PIN_K1_TX, 1);
	k_busy_wait(200);
	int rx_high = gpio_pin_get(hw_gpio0, PIN_K1_RX);
	LOG_INF("  TX=0 -> RX=%d (expect 0), TX=1 -> RX=%d (expect 1)",
		rx_low, rx_high);

	uint8_t rx = kline_tx_rx_byte(0x55);
	LOG_INF("  TX=0x55 RX=0x%02X (expect 0x55)", rx);

	if (rx != 0x55) {
		LOG_ERR("K-line self-test failed");
		return -EIO;
	}
	LOG_INF("K-line self-test passed");
	return 0;
}

/* Drive one 8N1 byte out of `tx_pin` at the K-line bit rate while sampling
 * `rx_pin` mid-bit.  The L9637D is non-inverting (TxD low -> K low -> RxD low)
 * and all transceivers share the K-line, so the byte appears on the far RxD. */
static uint8_t kline_xfer_byte(uint8_t tx_pin, uint8_t rx_pin, uint8_t tx)
{
	uint8_t rx = 0;

	gpio_pin_set(hw_gpio0, tx_pin, 0);		/* start bit */
	k_busy_wait(KLINE_BIT_US);

	for (int i = 0; i < 8; i++) {			/* 8 data bits, LSB first */
		gpio_pin_set(hw_gpio0, tx_pin, (tx >> i) & 1);
		k_busy_wait(KLINE_BIT_US / 2);
		rx |= (gpio_pin_get(hw_gpio0, rx_pin) & 1) << i;
		k_busy_wait(KLINE_BIT_US / 2);
	}

	gpio_pin_set(hw_gpio0, tx_pin, 1);		/* stop bit / idle */
	k_busy_wait(KLINE_BIT_US);
	return rx;
}

/* Stream every byte value 0x00..0xFF from one transceiver's TxD to the other's
 * RxD over the shared K-line and count how many arrive intact.  The first few
 * mismatches are logged; the rest are summarised. */
static int kline_stream(const char *dir, uint8_t tx_pin, uint8_t rx_pin)
{
	const int n = 256;
	int ok = 0, shown = 0;

	for (int i = 0; i < n; i++) {
		uint8_t tx = (uint8_t)i;
		uint8_t rx = kline_xfer_byte(tx_pin, rx_pin, tx);
		if (rx == tx) {
			ok++;
		} else if (shown++ < 8) {
			LOG_ERR("  %s [%d]: sent 0x%02X got 0x%02X", dir, i, tx, rx);
		}
		k_busy_wait(KLINE_BIT_US * 2);		/* inter-byte idle */
	}

	LOG_INF("%s: %d/%d bytes OK", dir, ok, n);
	return (ok == n) ? 0 : -EIO;
}

/* -- ISO 9141-2 / ISO 14230-4 init -------------------------------------------
 *
 * Two ways to open a K-line session, tried in turn; neither sends a
 * diagnostic request beyond what the init itself consists of.
 *
 * Slow (5-baud) init — ISO 9141-2 and ISO 14230-4: the tester sends the
 * functional address 0x33 as one 8N1 byte at 5 baud (200 ms per bit, 2 s
 * in all) on K, and on L where the board lets it.  The ECU answers at
 * 10.4 kbaud with the sync byte 0x55 and two key bytes; the tester echoes
 * the inverted second key byte and the ECU acknowledges with the inverted
 * address (0xCC).
 *
 * Fast init — ISO 14230-4 only, and the only one most KWP2000 ECUs from
 * about 2000 on (Toyota included) respond to: a 25 ms low / 25 ms high
 * wake-up pulse on K, immediately followed by the StartCommunication
 * request (C1 33 F1 81 66).  The ECU's positive response carries its
 * address and the same two key bytes.  That one frame is unavoidable —
 * it *is* the init — and nothing follows it: the session is left to time
 * out on the ECU's side (P3max, 5 s) once the rails drop.
 *
 * Timing windows (ISO 9141-2 figure 2 / ISO 14230-2 tables 3 and 4):
 *   W5    bus idle before the address            >= 300 ms
 *   W1    end of address -> sync byte            60-300 ms
 *   W2    sync -> KW1                            5-20 ms
 *   W3    KW1 -> KW2                             0-20 ms
 *   W4    KW2 -> ~KW2, and ~KW2 -> ~addr         25-50 ms
 *   Tidle bus idle before a fast init            >= 300 ms (after a failed init)
 *   Tinil wake-up low                            25 ms
 *   Twup  wake-up start -> first request byte    50 ms
 *   P4    tester inter-byte gap                  5-20 ms
 *   P2    request end -> response start          25-50 ms
 *   P1    ECU inter-byte gap                     0-20 ms
 *
 * Bit-banged like the rest of this file.  The TJA1027T (v3.0+) has no TXD
 * dominant time-out, only an initial-TXD-low check on leaving sleep, so it
 * will hold the bus down for the 200 ms address bits; the L9637D always did.
 * RX mirrors the bus on both parts, so every bit we drive is read back and a
 * transmitter that is not actually driving the wire shows up as an echo
 * failure rather than as a silent ECU.  (The mirror is of the transceiver's
 * own bus pin, though, which idles high off its internal pull-up — an open
 * K wire is indistinguishable from an idle vehicle bus here.)
 */

#define KLINE_INIT_ADDR     0x33	/* OBD functional address */
#define KLINE_TESTER_ADDR   0xF1
#define KLINE_SLOW_BIT_MS   200
#define KLINE_SYNC_BYTE     0x55
#define KLINE_SID_START_COMM 0x81
#define KLINE_FRAME_MAX     16		/* header + up to 12 data + checksum */

/* Wait up to timeout_ms for a start bit, then clock in one 8N1 byte at the
 * K-line bit rate.  -ETIMEDOUT if nothing arrived, -EBADMSG on a framing
 * error (stop bit low).  The byte itself is written in either success case. */
static int kline_rx_byte(uint32_t timeout_ms, uint8_t *out)
{
	int64_t deadline = k_uptime_get() + timeout_ms;

	while (gpio_pin_get(hw_gpio0, PIN_K1_RX)) {
		if (k_uptime_get() >= deadline) {
			return -ETIMEDOUT;
		}
	}

	unsigned int key = irq_lock();
	uint8_t v = 0;

	k_busy_wait(KLINE_BIT_US + KLINE_BIT_US / 2);	/* middle of bit 0 */
	for (int i = 0; i < 8; i++) {
		v |= (gpio_pin_get(hw_gpio0, PIN_K1_RX) & 1) << i;
		k_busy_wait(KLINE_BIT_US);
	}
	int stop = gpio_pin_get(hw_gpio0, PIN_K1_RX);

	irq_unlock(key);
	*out = v;
	return stop ? 0 : -EBADMSG;
}

/* Send one 8N1 byte at the K-line bit rate with interrupts held off for the
 * ~1 ms it takes, and return what the bus echoed back. */
static uint8_t kline_tx_byte(uint8_t tx)
{
	unsigned int key = irq_lock();
	uint8_t rx = kline_tx_rx_byte(tx);

	irq_unlock(key);
	return rx;
}

/* Hold the line at `level` for one 5-baud bit and count the milliseconds
 * the bus disagreed with it (after a 2 ms settle).  The L pulldown FET is
 * asserted for a low bit when use_l is set. */
static int kline_slow_bit(int level, bool use_l)
{
	int bad = 0;

	gpio_pin_set(hw_gpio0, PIN_K1_TX, level);
	if (use_l) {
		kline_l_send(!level);
	}
	int64_t t0 = k_uptime_get();

	for (int64_t t = 0; t < KLINE_SLOW_BIT_MS; t = k_uptime_get() - t0) {
		k_msleep(1);
		if (t >= 2 && (gpio_pin_get(hw_gpio0, PIN_K1_RX) & 1) != level) {
			bad++;
		}
	}
	return bad;
}

/* Send `addr` at 5 baud, LSB first, framed with start and stop bits.
 * bad_low/bad_high receive the milliseconds where the bus did not follow
 * TX, split by driven level. */
static void kline_slow_addr(uint8_t addr, bool use_l, int *bad_low, int *bad_high)
{
	int lo = 0, hi = 0;

	for (int i = 0; i < 10; i++) {
		int level = (i == 0) ? 0 : (i == 9) ? 1 : (addr >> (i - 1)) & 1;
		int bad = kline_slow_bit(level, use_l);

		if (level) {
			hi += bad;
		} else {
			lo += bad;
		}
	}
	if (use_l) {
		kline_l_send(false);
	}
	*bad_low = lo;
	*bad_high = hi;
}

/* Key bytes as the ECU sends them: KB1 then KB2.  ISO 14230-4 fixes KB2 at
 * 0x8F with KB1 describing the header formats supported; ISO 9141-2 uses
 * 08 08 or 94 94. */
static const char *kline_kw_name(uint8_t kb1, uint8_t kb2)
{
	if (kb1 == 0x08 && kb2 == 0x08) {
		return "ISO 9141-2";
	}
	if (kb1 == 0x94 && kb2 == 0x94) {
		return "ISO 9141-2 (KW 94 94)";
	}
	if (kb2 == 0x8F) {
		return "ISO 14230-4 KWP2000";
	}
	return "unrecognised key bytes";
}

/* Confirm the bus sits high for `ms` and report how long it did not. */
static int kline_wait_idle(int ms)
{
	int busy = 0;

	for (int i = 0; i < ms; i++) {
		if (!(gpio_pin_get(hw_gpio0, PIN_K1_RX) & 1)) {
			busy++;
		}
		k_msleep(1);
	}
	return busy;
}

/* Whether the L line can be driven for the 5-baud address: the pulldown
 * gate must be permitted on this board and, where the line can be sensed,
 * must not be shorted. */
static bool kline_can_use_l(void)
{
	if (PIN_L_SEND < 0 || kline_l_send(false) != 0) {
		return false;
	}
	int p = kline_l_line_probe(NULL, NULL);

	return p == 0 || p == -ENODEV;
}

/* 5-baud init to `addr`.  0 with the key bytes filled in, -ETIMEDOUT if
 * nothing answered at all (the "try something else" case), other negative
 * errno on a malformed handshake.  quiet suppresses everything but replies,
 * for the address sweep. */
static int kline_slow_init(uint8_t addr, bool use_l, bool quiet,
			   uint8_t *kb1, uint8_t *kb2)
{
	if (!quiet) {
		printk("slow init: address 0x%02X on %s\n",
		       addr, use_l ? "K+L" : "K only");
	}

	/* The address, 2 s at 5 baud, watching the echo. */
	int bad_low = 0, bad_high = 0;

	kline_slow_addr(addr, use_l, &bad_low, &bad_high);
	if (bad_low && !quiet) {
		printk("  echo: bus stayed high for %d ms of the low bits — "
		       "transceiver not driving K\n", bad_low);
	}
	if (bad_high && !quiet) {
		printk("  echo: bus low for %d ms of the high bits — "
		       "K held down externally\n", bad_high);
	}

	/* W1: sync byte.  The spec allows 300 ms; give it a little slack. */
	uint8_t sync = 0, ack = 0;
	int64_t t_addr = k_uptime_get();
	int r = kline_rx_byte(400, &sync);
	int64_t w1 = k_uptime_get() - t_addr;

	if (r == -ETIMEDOUT) {
		if (!quiet) {
			printk("  no sync byte within %lld ms\n", w1);
		}
		return -ETIMEDOUT;
	}
	printk("  0x%02X: sync 0x%02X after %lld ms%s\n", addr, sync, w1,
	       r == -EBADMSG ? " (framing error)" : "");
	if (sync != KLINE_SYNC_BYTE) {
		printk("  expected 0x%02X — baud rate mismatch or noise\n",
		       KLINE_SYNC_BYTE);
		return -EPROTO;
	}

	/* W2/W3: key bytes, each due within 20 ms. */
	if (kline_rx_byte(50, kb1) == -ETIMEDOUT) {
		printk("  no KB1 after sync\n");
		return -EPROTO;
	}
	if (kline_rx_byte(50, kb2) == -ETIMEDOUT) {
		printk("  no KB2 (KB1=0x%02X)\n", *kb1);
		return -EPROTO;
	}
	printk("  key bytes: KB1=0x%02X KB2=0x%02X — %s\n",
	       *kb1, *kb2, kline_kw_name(*kb1, *kb2));

	/* W4: send back ~KB2 after 25-50 ms; the ECU answers with ~address. */
	k_msleep(30);
	uint8_t inv = (uint8_t)~*kb2;
	uint8_t echo = kline_tx_byte(inv);

	if (echo != inv) {
		printk("  warning: ~KB2 0x%02X echoed as 0x%02X\n", inv, echo);
	}
	if (kline_rx_byte(60, &ack) == -ETIMEDOUT) {
		printk("  no address acknowledge after ~KB2\n");
		return -EPROTO;
	}
	printk("  ack: 0x%02X (expect 0x%02X)\n", ack, (uint8_t)~addr);
	return ack == (uint8_t)~addr ? 0 : -EPROTO;
}

/* 5-baud sweep: the slow init to every address 0x01..0xFE bar our own and
 * the functional one already tried, stopping at the first ECU that completes
 * the handshake.  ~3 s per address, so up to 13 minutes.  Any reply at all
 * is reported and the sweep carries on past a broken handshake. */
static int kline_slow_init_scan(bool use_l, uint8_t *ecu, uint8_t *kb1,
				uint8_t *kb2)
{
	int replies = 0;

	printk("scan: 5-baud init 0x01..0xFE on %s\n", use_l ? "K+L" : "K only");
	for (int t = 0x01; t <= 0xFE; t++) {
		if (t == KLINE_INIT_ADDR || t == KLINE_TESTER_ADDR) {
			continue;
		}
		if ((t & 0x0F) == 0) {
			printk("  ...0x%02X\n", t);
		}
		/* W5 before each address. */
		if (kline_wait_idle(300)) {
			printk("  0x%02X: bus busy before init — waiting\n", t);
			kline_wait_idle(300);
		}

		int r = kline_slow_init((uint8_t)t, use_l, true, kb1, kb2);

		if (r == 0) {
			*ecu = (uint8_t)t;
			return 0;
		}
		if (r != -ETIMEDOUT) {
			replies++;
		}
	}
	printk("scan: no session opened (%d address%s replied)\n",
	       replies, replies == 1 ? "" : "es");
	return replies ? -EPROTO : -ETIMEDOUT;
}

/* Receive one KWP2000 frame: format byte, optional target/source, optional
 * length byte, data, checksum.  Returns the number of bytes stored (header
 * through checksum) or a negative errno.  first_ms bounds the wait for the
 * first byte; later bytes must follow within P1max (20 ms, plus slack). */
static int kline_rx_frame(uint8_t *buf, int max, uint32_t first_ms)
{
	int n = 0;

	if (kline_rx_byte(first_ms, &buf[0]) == -ETIMEDOUT) {
		return -ETIMEDOUT;
	}
	n = 1;

	int hdr = 1 + ((buf[0] & 0xC0) ? 2 : 0);	/* target + source present? */
	int len = buf[0] & 0x3F;

	if (len == 0) {
		hdr++;					/* explicit length byte */
	}
	while (n < hdr) {
		if (n >= max) {
			return -EMSGSIZE;
		}
		if (kline_rx_byte(40, &buf[n]) == -ETIMEDOUT) {
			return -EPROTO;			/* frame stopped short */
		}
		n++;
	}
	if (len == 0) {
		len = buf[hdr - 1];
	}

	int total = hdr + len + 1;			/* + checksum */

	if (total > max) {
		return -EMSGSIZE;
	}
	while (n < total) {
		if (kline_rx_byte(40, &buf[n]) == -ETIMEDOUT) {
			return -EPROTO;
		}
		n++;
	}

	uint8_t cs = 0;

	for (int i = 0; i < total - 1; i++) {
		cs += buf[i];
	}
	if (cs != buf[total - 1]) {
		printk("  checksum 0x%02X, computed 0x%02X\n", buf[total - 1], cs);
		return -EBADMSG;
	}
	return total;
}

/* Fast init: wake-up pulse plus StartCommunication to `target`, addressed
 * functionally (fmt 0xC1, the OBD way) or physically (fmt 0x81, how Toyota's
 * own tester talks to each ECU on the SIL line).  0 with the key bytes and
 * the responding ECU's address filled in on a positive response.  quiet
 * suppresses everything but replies, for the address scan. */
static int kline_fast_init(uint8_t fmt, uint8_t target, uint32_t first_ms,
			   bool quiet, uint8_t *ecu, uint8_t *kb1, uint8_t *kb2)
{
	const uint8_t req[] = {
		fmt | 1,			/* 1 data byte */
		target, KLINE_TESTER_ADDR,
		KLINE_SID_START_COMM,
	};
	uint8_t cs = 0;

	for (size_t i = 0; i < sizeof(req); i++) {
		cs += req[i];
	}
	if (!quiet) {
		printk("fast init: 25 ms wake-up then StartCommunication "
		       "%02X %02X %02X %02X %02X (%s address 0x%02X)\n",
		       req[0], req[1], req[2], req[3], cs,
		       (fmt & 0xC0) == 0xC0 ? "functional" : "physical", target);
	}

	/* Tinil / Twup, then the request straight after the high half. */
	int echo_bad = 0;

	gpio_pin_set(hw_gpio0, PIN_K1_TX, 0);
	k_busy_wait(25000);
	echo_bad += gpio_pin_get(hw_gpio0, PIN_K1_RX) ? 1 : 0;
	gpio_pin_set(hw_gpio0, PIN_K1_TX, 1);
	k_busy_wait(25000);
	echo_bad += gpio_pin_get(hw_gpio0, PIN_K1_RX) ? 0 : 1;
	if (echo_bad && !quiet) {
		printk("  echo: bus did not follow the wake-up pulse\n");
	}

	for (size_t i = 0; i <= sizeof(req); i++) {
		uint8_t b = (i < sizeof(req)) ? req[i] : cs;
		uint8_t e = kline_tx_byte(b);

		if (e != b && !quiet) {
			printk("  echo: sent 0x%02X read back 0x%02X\n", b, e);
		}
		if (i < sizeof(req)) {
			k_msleep(5);				/* P4 */
		}
	}

	/* P2: response starts within 50 ms.  Positive: 8x F1 <ecu> C1 KB1 KB2 CS. */
	uint8_t rsp[KLINE_FRAME_MAX];
	int64_t t_req = k_uptime_get();
	int n = kline_rx_frame(rsp, sizeof(rsp), first_ms);
	int64_t p2 = k_uptime_get() - t_req;

	if (n == -ETIMEDOUT) {
		if (!quiet) {
			printk("  no response within %lld ms\n", p2);
		}
		return -ETIMEDOUT;
	}
	if (n < 0) {
		printk("  0x%02X: malformed response (%d) after %lld ms\n",
		       target, n, p2);
		return n;
	}

	printk("  0x%02X: response after %lld ms:", target, p2);
	for (int i = 0; i < n; i++) {
		printk(" %02X", rsp[i]);
	}
	printk("\n");

	int hdr = 1 + ((rsp[0] & 0xC0) ? 2 : 0) + ((rsp[0] & 0x3F) ? 0 : 1);
	const uint8_t *data = &rsp[hdr];
	int dlen = n - hdr - 1;

	*ecu = (rsp[0] & 0xC0) ? rsp[2] : target;
	if (dlen >= 1 && data[0] == 0x7F) {
		printk("  negative response: service 0x%02X code 0x%02X\n",
		       dlen >= 2 ? data[1] : 0, dlen >= 3 ? data[2] : 0);
		return -EPROTO;
	}
	if (dlen < 3 || data[0] != (KLINE_SID_START_COMM | 0x40)) {
		printk("  not a StartCommunication positive response\n");
		return -EPROTO;
	}
	*kb1 = data[1];
	*kb2 = data[2];
	printk("  ECU address 0x%02X, key bytes: KB1=0x%02X KB2=0x%02X — %s\n",
	       *ecu, *kb1, *kb2, kline_kw_name(*kb1, *kb2));
	return 0;
}

/* Physical-address sweep: a fast init to every target 0x01..0xFE (bar our
 * own address and the functional one already tried), stopping at the first
 * ECU that opens a session.  ~0.5 s per address, so up to two minutes.
 * Any reply at all — even a negative one — also proves the wire reaches
 * an ECU, so those are reported and the sweep carries on past them. */
static int kline_fast_init_scan(uint8_t *ecu, uint8_t *kb1, uint8_t *kb2)
{
	int replies = 0;

	printk("scan: physical-address fast init 0x01..0xFE\n");
	for (int t = 0x01; t <= 0xFE; t++) {
		if (t == KLINE_INIT_ADDR || t == KLINE_TESTER_ADDR) {
			continue;
		}
		if ((t & 0x0F) == 0) {
			printk("  ...0x%02X\n", t);
		}
		/* Tidle after a failed init before the next wake-up pulse. */
		if (kline_wait_idle(300)) {
			printk("  0x%02X: bus busy before init — waiting\n", t);
			kline_wait_idle(300);
		}

		int r = kline_fast_init(0x80, (uint8_t)t, 100, true,
					ecu, kb1, kb2);

		if (r == 0) {
			return 0;
		}
		if (r != -ETIMEDOUT) {
			replies++;
		}
	}
	printk("scan: no session opened (%d address%s replied)\n",
	       replies, replies == 1 ? "" : "es");
	return replies ? -EPROTO : -ETIMEDOUT;
}

int kline_vehicle_init_ex(struct kline_session *out)
{
	if (!IS_ENABLED(CONFIG_APP_BOARD_HAS_KLINE) || PIN_K1_TX < 0) {
		printk("K-line init skipped (no K-line on this board)\n");
		return -ENODEV;
	}

	printk("\n*** K-LINE INIT ***\n");

	int err = kline_init();

	if (err) {
		printk("K-line init aborted: rails did not come up (%d)\n", err);
		return err;
	}
	printk("TX=P0.%d RX=P0.%d\n", PIN_K1_TX, PIN_K1_RX);

	/* W5: the bus must sit idle (high) for at least 300 ms first.  A line
	 * that is low here is either unpowered on the far side or held down
	 * by something — either way there is no point sending anything. */
	int idle = gpio_pin_get(hw_gpio0, PIN_K1_RX);
	int busy = kline_wait_idle(300);

	printk("bus idle: RX=%d, low for %d/300 ms\n", idle, busy);
	if (busy > 0) {
		printk("FAIL: K line not idle (held low / traffic present)\n");
		err = -EBUSY;
		goto out;
	}

	uint8_t ecu = KLINE_INIT_ADDR, kb1 = 0, kb2 = 0;
	const char *how = "slow init";
	bool use_l = kline_can_use_l();

	err = kline_slow_init(KLINE_INIT_ADDR, use_l, false, &kb1, &kb2);
	if (err == -ETIMEDOUT) {
		/* Silence, not a bad handshake: try the KWP2000 fast init after
		 * the bus has idled again. */
		busy = kline_wait_idle(300);
		if (busy > 0) {
			printk("FAIL: K line not idle before fast init (%d ms low)\n",
			       busy);
			err = -EBUSY;
			goto out;
		}
		how = "fast init";
		err = kline_fast_init(0xC0, KLINE_INIT_ADDR, 300, false,
				      &ecu, &kb1, &kb2);
	}
	if (err == -ETIMEDOUT) {
		/* Still nothing on the OBD functional address.  An ECU that
		 * only speaks to the maker's tester wants its own physical
		 * address instead — find it. */
		how = "physical-address fast init";
		err = kline_fast_init_scan(&ecu, &kb1, &kb2);
	}
	if (err == -ETIMEDOUT) {
		/* Last standard combination: the slow init also carries an
		 * address, and some makers' ECUs only wake to their own. */
		how = "physical-address 5-baud init";
		err = kline_slow_init_scan(use_l, &ecu, &kb1, &kb2);
	}

out:
	if (err) {
		printk("FAIL: K-line init (%d)\n", err);
	} else {
		printk("PASS: K-line communication established via %s, "
		       "ECU 0x%02X (%s)\n", how, ecu, kline_kw_name(kb1, kb2));
		if (out) {
			out->how = how;
			out->protocol = kline_kw_name(kb1, kb2);
			out->ecu = ecu;
			out->kb1 = kb1;
			out->kb2 = kb2;
		}
	}
	printk("*** K-LINE INIT DONE ***\n\n");

	/* Nothing more is sent; drop the rails and let the ECU time the
	 * session out. */
	kline_power_off();
	return err;
}

int kline_vehicle_init(void)
{
	return kline_vehicle_init_ex(NULL);
}

/* -- L line: pulldown gate + sense (v3.3+) -----------------------------------
 *
 * L_SEND gates a 2N7002 that pulls the L wire to ground for the 5-baud
 * address init.  On every board before v3.3 that FET sits directly across
 * the wire: if L is shorted to battery it saturates into the short and dies
 * inside the first address bit, sometimes taking the nRF with it through a
 * drain-gate short.  CONFIG_APP_L_SEND_ENABLED is therefore off on those
 * boards and kline_l_send() refuses, so nothing in firmware can assert it.
 *
 * v3.3 puts an AL5809-90 (90 mA, thermal shutdown) in series with the wire
 * and taps it for L_SENSE: pin -> 47K -> 1N4148 anode, cathode on the L
 * node.  The diode blocks the vehicle's 12 V from ever reaching the pin, so
 * the only way to read the line is to source current into it, and the SAADC
 * does that with its internal pull-up resistor ladder (~400K to VDD):
 *
 *   L pulled low   diode conducts, the pin sits a forward drop plus
 *                  47K x ladder current above the wire: ~0.7-0.9 V
 *   L high or open diode reverse biased, the ladder takes the pin to VDD
 *                  and the reading runs into the 3.6 V full scale
 *
 * A plain GPIO input cannot do this — the nRF's ~13K internal pull-up
 * against the 47K leaves even a grounded line at ~2.7 V, above VIH — and
 * Zephyr's ADC driver hard-codes the ladder to bypass (adc_nrfx_saadc.c),
 * hence nrfx here.
 */

#if IS_ENABLED(CONFIG_APP_BOARD_HAS_L_SENSE)

#define L_SENSE_CH      0                          /* SAADC channel index */
#define L_SENSE_LOW_MV  CONFIG_APP_L_SENSE_LOW_MV
#define L_SENSE_FS_MV   3600                       /* gain 1/6 x 0.6 V ref */

static bool s_l_sense_ok;
/* EasyDMA target: keep it out of the stack and word aligned. */
static nrf_saadc_value_t s_l_sample __aligned(4);

/* AIN index for a P0.x pin, or -1 if that pin has no ADC channel.
 * nRF9151: AIN0..AIN7 are P0.13..P0.20 and nothing else is sampleable. */
static int l_sense_ain(void)
{
	if (PIN_L_SENSE < 13 || PIN_L_SENSE > 20) {
		return -1;
	}
	return PIN_L_SENSE - 13;
}

int kline_l_sense_init(void)
{
	if (PIN_L_SENSE < 0) {
		return -ENODEV;
	}

	int ain = l_sense_ain();

	if (ain < 0) {
		LOG_ERR("L_SENSE on P0.%d has no SAADC channel (AIN0-7 are "
			"P0.13-P0.20) — L-line fault detection unavailable",
			PIN_L_SENSE);
		return -ENOTSUP;
	}

	int err = nrfx_saadc_init(NRFX_SAADC_DEFAULT_CONFIG_IRQ_PRIORITY);

	if (err && err != -EALREADY) {
		LOG_ERR("SAADC init failed (%d)", err);
		return err;
	}

	nrfx_saadc_channel_t ch =
		NRFX_SAADC_DEFAULT_CHANNEL_SE(NRFX_ANALOG_EXTERNAL_AIN0 + ain,
					      L_SENSE_CH);

	/* The whole point: pull the node up through the ladder so a line held
	 * low by the FET (or by an ECU) drags it back down through the diode. */
	ch.channel_config.resistor_p = NRF_SAADC_RESISTOR_PULLUP;
	ch.channel_config.gain       = NRF_SAADC_GAIN1_6;   /* 0 - 3.6 V */
	ch.channel_config.acq_time   = NRF_SAADC_ACQTIME_40US;  /* 47K source */

	err = nrfx_saadc_channel_config(&ch);
	if (err) {
		LOG_ERR("SAADC channel config failed (%d)", err);
		return err;
	}

	s_l_sense_ok = true;
	LOG_INF("L sense ready (P0.%d = AIN%d, pull-up ladder)",
		PIN_L_SENSE, ain);
	return 0;
}

bool kline_l_sense_available(void)
{
	return s_l_sense_ok;
}

int kline_l_sense_mv(void)
{
	if (!s_l_sense_ok) {
		return -ENODEV;
	}

	int err = nrfx_saadc_simple_mode_set(BIT(L_SENSE_CH),
					     NRF_SAADC_RESOLUTION_12BIT,
					     NRF_SAADC_OVERSAMPLE_DISABLED,
					     NULL);   /* NULL = blocking */
	if (!err) {
		err = nrfx_saadc_buffer_set(&s_l_sample, 1);
	}
	if (!err) {
		err = nrfx_saadc_mode_trigger();
	}
	if (err) {
		LOG_ERR("L sense conversion failed (%d)", err);
		return err;
	}

	int raw = s_l_sample < 0 ? 0 : s_l_sample;   /* offset error near 0 V */

	return raw * L_SENSE_FS_MV / 4096;
}

#else  /* no L sense on this board */

#define L_SENSE_LOW_MV 0

int  kline_l_sense_init(void)      { return -ENODEV; }
bool kline_l_sense_available(void) { return false; }
int  kline_l_sense_mv(void)        { return -ENODEV; }

#endif

/* Drive (or release) the L pulldown FET.  -EPERM on boards where L_SEND is
 * unsafe: that gate is the whole point, so nothing should poke PIN_L_SEND
 * directly. */
int kline_l_send(bool on)
{
	if (PIN_L_SEND < 0) {
		return -ENODEV;
	}
	if (!IS_ENABLED(CONFIG_APP_L_SEND_ENABLED)) {
		return -EPERM;
	}
	gpio_pin_configure(hw_gpio0, PIN_L_SEND,
			   on ? GPIO_OUTPUT_HIGH : GPIO_OUTPUT_LOW);
	return 0;
}

/* Pulse the pulldown and watch the sense follow it.  Cheap enough to run
 * before every 5-baud init: the wire is only held down for a few ms, and on
 * v3.3 the AL5809-90 caps the current at 90 mA even into a dead short.
 *
 *   0        the line went low when pulled — L wiring healthy
 *   -EIO     it stayed high: something low-impedance is holding it up,
 *            i.e. the L wire is shorted to battery.  Do not run the init.
 *   -ENODEV  no L sense / no L_SEND on this board
 *   -EPERM   L_SEND disabled (the pre-v3.3 defect)
 *
 * idle_mv/pulled_mv are optional and only written when the probe ran.
 */
int kline_l_line_probe(int *idle_mv, int *pulled_mv)
{
	if (!kline_l_sense_available() || PIN_L_SEND < 0) {
		return -ENODEV;
	}

	int idle = kline_l_sense_mv();

	if (idle < 0) {
		return idle;
	}

	int err = kline_l_send(true);

	if (err) {
		return err;
	}
	k_msleep(5);

	int pulled = kline_l_sense_mv();

	kline_l_send(false);

	if (pulled < 0) {
		return pulled;
	}
	if (idle_mv) {
		*idle_mv = idle;
	}
	if (pulled_mv) {
		*pulled_mv = pulled;
	}
	if (pulled >= L_SENSE_LOW_MV) {
		LOG_ERR("L line stuck high under the pulldown (%d mV idle, "
			"%d mV pulled) — shorted to battery?", idle, pulled);
		return -EIO;
	}
	LOG_INF("L line OK (%d mV idle -> %d mV pulled)", idle, pulled);
	return 0;
}

/* ISO-9141 interface test.  On boards with two transceivers (bench), stream
 * bytes K1->K2 and K2->K1 across the wire.  On single-transceiver boards
 * (v2.5K/v2.6K), verify the L9637D loopback path (TX drives the K-line,
 * same chip's RX reads it back) with a static toggle check and a full
 * 256-byte stream.  Where the L line can safely be driven, pulse the
 * pulldown FET and verify the wire follows — on L_SENSE (v3.3+) or on a
 * wired L_RECV. */
int kline_test(void)
{
	if (PIN_K1_TX < 0) {
		printk("K-line test skipped (no K-line on this board)\n");
		return 0;
	}

	printk("\n*** ISO-9141 TEST ***\n");

	if (kline_power_on()) {
		printk("K-line test aborted: rails did not come up\n");
		return -EIO;
	}

	gpio_pin_configure(hw_gpio0, PIN_K1_TX, GPIO_OUTPUT_HIGH);
	gpio_pin_configure(hw_gpio0, PIN_K1_RX, GPIO_INPUT | GPIO_PULL_UP);
	if (PIN_K2_TX >= 0) {
		gpio_pin_configure(hw_gpio0, PIN_K2_TX, GPIO_OUTPUT_HIGH);
		gpio_pin_configure(hw_gpio0, PIN_K2_RX, GPIO_INPUT | GPIO_PULL_UP);
	}
	k_msleep(10);

	/* Static loopback: verify TX drives RX through the L9637D */
	int rx_idle = gpio_pin_get(hw_gpio0, PIN_K1_RX);
	gpio_pin_set(hw_gpio0, PIN_K1_TX, 0);
	k_msleep(1);
	int rx_dom = gpio_pin_get(hw_gpio0, PIN_K1_RX);
	gpio_pin_set(hw_gpio0, PIN_K1_TX, 1);
	k_msleep(1);
	int rx_rec = gpio_pin_get(hw_gpio0, PIN_K1_RX);

	printk("K1 static: idle=%d TX=0->RX=%d TX=1->RX=%d",
	       rx_idle, rx_dom, rx_rec);

	int err = 0;

	if (rx_idle != 1 || rx_dom != 0 || rx_rec != 1) {
		printk(" FAIL\n");
		err = -EIO;
	} else {
		printk(" OK\n");
	}

	if (!err && PIN_K2_TX >= 0) {
		printk("K-wire: K1(P0.%d/P0.%d) <-> K2(P0.%d/P0.%d)\n",
		       PIN_K1_TX, PIN_K1_RX, PIN_K2_TX, PIN_K2_RX);
		int e1 = kline_stream("K1->K2", PIN_K1_TX, PIN_K2_RX);
		int e2 = kline_stream("K2->K1", PIN_K2_TX, PIN_K1_RX);
		if (e1 || e2)
			err = -EIO;
	} else if (!err) {
		printk("K1 loopback: TX=P0.%d RX=P0.%d\n",
		       PIN_K1_TX, PIN_K1_RX);
		err = kline_stream("K1 loop", PIN_K1_TX, PIN_K1_RX);
	}

	/* L-line.  v3.3+ reads the wire back on the L_SENSE ADC input; older
	 * boards can only check it with a wired L_RECV.  Where L_SEND is
	 * disabled there is nothing safe to drive, so say so and move on. */
	if (PIN_L_SEND >= 0 && !IS_ENABLED(CONFIG_APP_L_SEND_ENABLED)) {
		printk("L-line: SKIP (L_SEND disabled — the pulldown FET on "
		       "this board dies into a short to battery)\n");
	} else if (kline_l_sense_available()) {
		int idle = 0, pulled = 0;
		int res = kline_l_line_probe(&idle, &pulled);

		printk("L-line: idle=%d mV send=1->%d mV (low below %d mV)",
		       idle, pulled, L_SENSE_LOW_MV);
		if (res) {
			printk(" FAIL\n");
			err = -EIO;
		} else {
			printk(" OK\n");
		}
	} else if (PIN_L_SEND >= 0 && PIN_L_RECV >= 0) {
		kline_l_send(false);
		gpio_pin_configure(hw_gpio0, PIN_L_RECV,
				   GPIO_INPUT | GPIO_PULL_UP);
		k_msleep(5);

		int l_idle = gpio_pin_get(hw_gpio0, PIN_L_RECV);
		kline_l_send(true);
		k_msleep(1);
		int l_active = gpio_pin_get(hw_gpio0, PIN_L_RECV);
		kline_l_send(false);
		k_msleep(1);
		int l_release = gpio_pin_get(hw_gpio0, PIN_L_RECV);

		printk("L-line: idle=%d send=1->%d send=0->%d",
		       l_idle, l_active, l_release);

		if (l_idle != 1 || l_active != 0 || l_release != 1) {
			printk(" FAIL\n");
			err = -EIO;
		} else {
			printk(" OK\n");
		}

		gpio_pin_configure(hw_gpio0, PIN_L_SEND,
				   GPIO_INPUT | GPIO_PULL_DOWN);
	}

	if (err)
		printk("FAIL: ISO-9141 test\n");
	else
		printk("PASS: ISO-9141 test\n");

	printk("*** ISO-9141 TEST DONE ***\n\n");

	kline_power_off();
	return err;
}
