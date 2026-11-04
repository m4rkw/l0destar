/*
 * The MX25R6435F SPI NOR flash (Macronix, 64 Mbit, 8 MiB in 4 KiB sectors) on
 * bit-banged SPI, for the backlog in databuf.c (CONFIG_APP_FLASH_BACKLOG).
 *
 * Every use is a session: extflash_begin(), whatever reads and writes, then
 * extflash_end().  Between sessions the flash is in deep power-down (0.007 uA
 * typical, against 5 to 24 uA in standby), the SPI pins are idle and the chip
 * select is high.  extflash_init() puts it there at boot.
 *
 * SHARED BUS
 * ----------
 * The v4.1.x designs wire the flash to the CAN controller's SCK, SDI and SDO.
 * The MCP2518FD is on a switched rail (hw_domain.c), and with that rail down
 * its pins are clamp diodes into a dead supply: anything that drives the bus
 * pushes current into it, and on v4.1.2 SDO goes to the controller's output pin
 * with no resistor in the way.  So when the flash's pins are the CAN
 * controller's, a session is refused (-EBUSY) and the bus is not touched while
 * the CAN rail is down.  Nothing in production raises that rail after boot, so
 * on such a board the flash is reachable only in the few milliseconds it is up
 * at boot, which is enough to put the chip into deep power-down and no more.
 * Give the flash pins of its own (or set APP_PIN_CAN_* to -1 where no CAN
 * controller is fitted) and the restriction goes away.
 *
 * Not run on hardware.  Written from the datasheet (Rev. 1.6) for the board
 * designs that carry it (hardware/l0destar_v4.1.x, not in the repository yet),
 * and exercised on the host against a model of the chip (tests/test_extflash.py).
 */

#ifndef APP_EXTFLASH_H_
#define APP_EXTFLASH_H_

#include <stdint.h>

#include "flashlog.h"

/* Once, early in boot, after hw_domain_init(): puts the chip into deep
 * power-down.  On a shared bus that has to wait for the CAN rail, so it is done
 * the first time that comes up (the boot-time self-test and CAN init do). */
void extflash_init(void);

/* Opens a session.  0, or -EBUSY when the flash shares its bus with the CAN
 * controller and the CAN rail is down.  Nothing is touched in that case. */
int  extflash_begin(void);

/* Ends it: deep power-down, pins idle.  Always call it after a begin that
 * returned 0, whatever happened in between. */
void extflash_end(void);

/* A session with nothing in it: the chip is woken and put back to sleep. */
void extflash_park(void);

/* Inside a session: checks the chip is the one this was written for and
 * writable, and says how many sectors the backlog may use.  0, -ENODEV for
 * another chip, -EACCES if its block protection is set, -ETIMEDOUT if it is
 * stuck busy. */
int  extflash_probe(uint32_t *sectors);

/* The operations for flashlog, valid inside a session. */
const struct flashlog_dev *extflash_dev(uint32_t sectors);

#endif
