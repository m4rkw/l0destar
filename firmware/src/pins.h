#ifndef PINS_H_
#define PINS_H_

/* PCB pin assignments */
#define PIN_K1_TX      2
#define PIN_K1_RX      3
#define PIN_K2_TX      4
#define PIN_K2_RX      5
#define PIN_RLY_RST_FB 6
#define PIN_AUX_SW     7
#define PIN_RLY_SET    10
#define PIN_RLY_RST    11
#define PIN_RLY_SET_FB 12
#define PIN_TWI_SDA    21
#define PIN_TWI_SCL    22
#define PIN_INA_ALRT   23
#define PIN_ACC_INT1   24
#define PIN_ACC_INT2   30
#define PIN_IGN_SENSE  31

/* INA and accelerometer share the TWIM bus */
#define PIN_INA_SDA    PIN_TWI_SDA
#define PIN_INA_SCL    PIN_TWI_SCL
#define PIN_ACC_SDA    PIN_TWI_SDA
#define PIN_ACC_SCL    PIN_TWI_SCL

#endif
