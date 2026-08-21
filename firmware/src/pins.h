#ifndef PINS_H_
#define PINS_H_

/* PCB pin assignments — P0.x GPIO numbers, -1 = not fitted on this board.
 *
 * All values come from Kconfig (APP_PIN_*).  The per-board defaults live in
 * Kconfig.boards, selected by the APP_BOARD choice; a bench or board variant
 * can still remap any signal from local.conf without touching code. */
#define PIN_K1_TX      CONFIG_APP_PIN_K1_TX
#define PIN_K1_RX      CONFIG_APP_PIN_K1_RX
#define PIN_K2_TX      CONFIG_APP_PIN_K2_TX
#define PIN_K2_RX      CONFIG_APP_PIN_K2_RX
#define PIN_L_SEND     CONFIG_APP_PIN_L_SEND
#define PIN_L_RECV     CONFIG_APP_PIN_L_RECV
#define PIN_K_EN       CONFIG_APP_PIN_K_EN
#define PIN_K_SLEEP    CONFIG_APP_PIN_K_SLEEP
#define PIN_AUX_SW     CONFIG_APP_PIN_AUX_SW
#define PIN_OBD_EN     CONFIG_APP_PIN_OBD_EN
#define PIN_CAN_EN     CONFIG_APP_PIN_CAN_EN
#define PIN_TWI_SDA    CONFIG_APP_PIN_TWI_SDA
#define PIN_TWI_SCL    CONFIG_APP_PIN_TWI_SCL
#define PIN_INA_ALRT   CONFIG_APP_PIN_INA_ALRT
#define PIN_ACC_INT1   CONFIG_APP_PIN_ACC_INT1
#define PIN_ACC_INT2   CONFIG_APP_PIN_ACC_INT2
#define PIN_IGN_SENSE  CONFIG_APP_PIN_IGN_SENSE

/* MCP2518FD CAN controller (SPI bit-banged; -1 on boards without CAN) */
#define PIN_CAN_SCK    CONFIG_APP_PIN_CAN_SCK
#define PIN_CAN_SDI    CONFIG_APP_PIN_CAN_SDI
#define PIN_CAN_SDO    CONFIG_APP_PIN_CAN_SDO
#define PIN_CAN_CS     CONFIG_APP_PIN_CAN_CS
#define PIN_CAN_INT    CONFIG_APP_PIN_CAN_INT

/* Latching relay (no-ops unless APP_RELAY_CONNECTED) */
#define PIN_RLY_SET    CONFIG_APP_PIN_RLY_SET
#define PIN_RLY_RST    CONFIG_APP_PIN_RLY_RST
#define PIN_RLY_SET_FB CONFIG_APP_PIN_RLY_SET_FB
#define PIN_RLY_RST_FB CONFIG_APP_PIN_RLY_RST_FB

/* Status LEDs (-1 = not fitted; LED4/5 exist only on v2.1 mini) */
#define PIN_LED1       CONFIG_APP_PIN_LED1
#define PIN_LED2       CONFIG_APP_PIN_LED2
#define PIN_LED3       CONFIG_APP_PIN_LED3
#define PIN_LED4       CONFIG_APP_PIN_LED4
#define PIN_LED5       CONFIG_APP_PIN_LED5

/* Switched-rail status sense inputs (v3.1+).  The 3.3V senses read high
 * while their rail is up; the 12V sense is inverted by a 2N7002 and reads
 * low while PP12V_K is up (CONFIG_APP_BOARD_RAIL_ST_12V_ACTIVE_LOW). */
#define PIN_GPS_RAIL_ST    CONFIG_APP_PIN_GPS_RAIL_ST
#define PIN_CAN_RAIL_ST    CONFIG_APP_PIN_CAN_RAIL_ST
#define PIN_K3V3_RAIL_ST   CONFIG_APP_PIN_K3V3_RAIL_ST
#define PIN_K12V_RAIL_ST   CONFIG_APP_PIN_K12V_RAIL_ST

/* 0-30V AIO inputs (v2.1 only) */
#define PIN_AIO1       CONFIG_APP_PIN_AIO1
#define PIN_AIO2       CONFIG_APP_PIN_AIO2
#define PIN_AIO3       CONFIG_APP_PIN_AIO3
#define PIN_AIO4       CONFIG_APP_PIN_AIO4
#define PIN_AIO5       CONFIG_APP_PIN_AIO5
#define PIN_AIO6       CONFIG_APP_PIN_AIO6

/* INA and accelerometer share the TWIM bus */
#define PIN_INA_SDA    PIN_TWI_SDA
#define PIN_INA_SCL    PIN_TWI_SCL
#define PIN_ACC_SDA    PIN_TWI_SDA
#define PIN_ACC_SCL    PIN_TWI_SCL

#endif
