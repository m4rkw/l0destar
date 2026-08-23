#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

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

/* ISO-9141 interface test.  On boards with two transceivers (bench), stream
 * bytes K1->K2 and K2->K1 across the wire.  On single-transceiver boards
 * (v2.5K/v2.6K), verify the L9637D loopback path (TX drives the K-line,
 * same chip's RX reads it back) with a static toggle check and a full
 * 256-byte stream.  If L-line pins are fitted, verify the L pulldown FET
 * toggles the L-line. */
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

	/* L-line: verify the L pulldown FET toggles the L-line */
	if (PIN_L_SEND >= 0 && PIN_L_RECV >= 0) {
		gpio_pin_configure(hw_gpio0, PIN_L_SEND, GPIO_OUTPUT_LOW);
		gpio_pin_configure(hw_gpio0, PIN_L_RECV,
				   GPIO_INPUT | GPIO_PULL_UP);
		k_msleep(5);

		int l_idle = gpio_pin_get(hw_gpio0, PIN_L_RECV);
		gpio_pin_set(hw_gpio0, PIN_L_SEND, 1);
		k_msleep(1);
		int l_active = gpio_pin_get(hw_gpio0, PIN_L_RECV);
		gpio_pin_set(hw_gpio0, PIN_L_SEND, 0);
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
