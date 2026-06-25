#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "hw_common.h"
#include "pins.h"

LOG_MODULE_REGISTER(hw_kline, CONFIG_APP_LOG_LEVEL);

#define KLINE_BIT_US 96

void kline_power_on(void)
{
	k_msleep(500);
}

void kline_power_off(void)
{
	LOG_INF("K-line power off");
}

int kline_init(void)
{
	kline_power_on();
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

/* K-wire transmission check: stream a run of bytes K1 -> K2 and then K2 -> K1
 * over the shared K-line, confirming the two L9637Ds actually talk to each
 * other across the wire (the loopback test only proves each chip's own path). */
int kline_test(void)
{
	gpio_pin_configure(hw_gpio0, PIN_K1_TX, GPIO_OUTPUT_HIGH);
	gpio_pin_configure(hw_gpio0, PIN_K2_TX, GPIO_OUTPUT_HIGH);
	gpio_pin_configure(hw_gpio0, PIN_K1_RX, GPIO_INPUT | GPIO_PULL_UP);
	gpio_pin_configure(hw_gpio0, PIN_K2_RX, GPIO_INPUT | GPIO_PULL_UP);
	k_msleep(10);					/* let the bus settle high */

	LOG_INF("K-wire test: K1(TX=P0.%d RX=P0.%d) <-> K2(TX=P0.%d RX=P0.%d)",
		PIN_K1_TX, PIN_K1_RX, PIN_K2_TX, PIN_K2_RX);

	int e1 = kline_stream("K1->K2", PIN_K1_TX, PIN_K2_RX);
	int e2 = kline_stream("K2->K1", PIN_K2_TX, PIN_K1_RX);

	if (e1 || e2) {
		LOG_ERR("K-wire test FAILED");
		return -EIO;
	}
	LOG_INF("K-wire test passed (256 bytes each way)");
	return 0;
}
