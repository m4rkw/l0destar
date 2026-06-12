#ifndef PINS_H_
#define PINS_H_

/* PCB pin assignments — P0.x GPIO numbers.
 *
 * All values come from Kconfig (APP_PIN_*) so a bench or board variant can
 * remap any signal from local.conf without touching code.  Defaults are the
 * production PCB; signals for hardware that isn't fitted are parked on free
 * GPIOs so every pin always has a defined assignment. */
#define PIN_K1_TX      CONFIG_APP_PIN_K1_TX
#define PIN_K1_RX      CONFIG_APP_PIN_K1_RX
#define PIN_K2_TX      CONFIG_APP_PIN_K2_TX
#define PIN_K2_RX      CONFIG_APP_PIN_K2_RX
#define PIN_AUX_SW     CONFIG_APP_PIN_AUX_SW
#define PIN_TWI_SDA    CONFIG_APP_PIN_TWI_SDA
#define PIN_TWI_SCL    CONFIG_APP_PIN_TWI_SCL
#define PIN_INA_ALRT   CONFIG_APP_PIN_INA_ALRT
#define PIN_ACC_INT1   CONFIG_APP_PIN_ACC_INT1
#define PIN_ACC_INT2   CONFIG_APP_PIN_ACC_INT2
#define PIN_IGN_SENSE  CONFIG_APP_PIN_IGN_SENSE

/* Latching relay (no-ops unless APP_RELAY_CONNECTED) */
#define PIN_RLY_SET    CONFIG_APP_PIN_RLY_SET
#define PIN_RLY_RST    CONFIG_APP_PIN_RLY_RST
#define PIN_RLY_SET_FB CONFIG_APP_PIN_RLY_SET_FB
#define PIN_RLY_RST_FB CONFIG_APP_PIN_RLY_RST_FB

/* INA and accelerometer share the TWIM bus */
#define PIN_INA_SDA    PIN_TWI_SDA
#define PIN_INA_SCL    PIN_TWI_SCL
#define PIN_ACC_SDA    PIN_TWI_SDA
#define PIN_ACC_SCL    PIN_TWI_SCL

#endif
