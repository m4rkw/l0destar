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
