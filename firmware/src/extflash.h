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
 * The v4.0p2 and v4.1.x designs wire the flash to the CAN controller's SCK, SDI
 * and SDO.
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
 * Run on hardware only as far as the ID read at boot (a v4.0p2 bench unit,
 * 10 Nov 2026, after a pin reset and after a cold start: C2 28 17); programming
 * and erasing have not.  Written from the datasheet (checked against Rev. 1.2)
 * for the board designs that carry it (hardware/l0destar_v4.0p2, and the
 * v4.1.x ones, not in the repository yet), and exercised on the host against a
 * model of the chip (tests/test_extflash.py).
 */

#ifndef APP_EXTFLASH_H_
#define APP_EXTFLASH_H_

#include <stdint.h>

#include "flashlog.h"

/* Once, early in boot, after hw_domain_init(): wakes the chip, reads its ID and
 * puts it into deep power-down.  On a shared bus that has to wait for the CAN
 * rail, so it is done the first time that comes up (the boot-time self-test and
 * CAN init do).  What the ID read found is extflash_boot_check()'s. */
void extflash_init(void);

/* What the ID read at boot found, for the caller that says so (databuf.c does,
 * from the main thread's startup): 0 if it is the chip this was written for,
 * -ENODEV if not, with id[] what came back (FF FF FF for no chip, or one that
 * does not answer; 00 00 00 for a line held low), -EAGAIN if it has not been
 * read yet because the bus is shared with the CAN controller and its rail has
 * not come up.  Asked once per boot: a chip that goes wrong later is the
 * session check's to find (extflash_check()). */
int  extflash_boot_check(uint8_t id[3]);

/* Opens a session.  0, or -EBUSY when the flash shares its bus with the CAN
 * controller and the CAN rail is down.  Nothing is touched in that case. */
int  extflash_begin(void);

/* Ends it: deep power-down, pins idle.  Always call it after a begin that
 * returned 0, whatever happened in between. */
void extflash_end(void);

/* A session with nothing in it: the chip is woken and put back to sleep.  The
 * first one of a boot also reads its ID, for extflash_boot_check(). */
void extflash_park(void);

/* Inside a session: checks the chip is the one this was written for and
 * writable, and says how many sectors the backlog may use (at most the chip's
 * first 2016: the last 32 are kept for log lines).  0, -ENODEV for
 * another chip, -EACCES if its block protection is set, -ETIMEDOUT if it is
 * stuck busy. */
int  extflash_probe(uint32_t *sectors);

/* Inside a session: reads the chip's ID and nothing else (about 0.2 ms; no
 * status read, no write).  0 if it is the expected chip, -ENODEV if not.  For
 * the start of every session after the first, to notice a chip that has gone
 * away; the caller says what it found. */
int  extflash_check(void);

/* The operations for flashlog, valid inside a session. */
const struct flashlog_dev *extflash_dev(uint32_t sectors);

#endif
