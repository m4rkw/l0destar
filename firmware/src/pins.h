#ifndef PINS_H_
#define PINS_H_

/* PCB pin assignments */
#define PIN_K1_TX      2
#define PIN_K1_RX      3
#define PIN_K2_TX      4
#define PIN_K2_RX      5
#define PIN_AUX_SW     7
#define PIN_TWI_SDA    12
#define PIN_TWI_SCL    11
#define PIN_INA_ALRT   10
#define PIN_ACC_INT1   31
#define PIN_ACC_INT2   30
/* Ignition sense is hard-coded ON in ignition_read() for testing; the real
 * sense line isn't wired on this board, so park the pin on a free GPIO (P0.6)
 * to keep P0.31 exclusively for ACC_INT1. */
#define PIN_IGN_SENSE  6

/* INA and accelerometer share the TWIM bus */
#define PIN_INA_SDA    PIN_TWI_SDA
#define PIN_INA_SCL    PIN_TWI_SCL
#define PIN_ACC_SDA    PIN_TWI_SDA
#define PIN_ACC_SCL    PIN_TWI_SCL

#endif
