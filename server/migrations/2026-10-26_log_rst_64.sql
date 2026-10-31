-- Bring log.rst up to the width schema.sql has always said it has.
--
-- The live column was VARCHAR(48) while schema.sql says VARCHAR(64), and the
-- server runs in STRICT_TRANS_TABLES: a value longer than the column is
-- refused, and the INSERT with it.  The record that carries rst= is the first
-- after a boot, the one that also carries fw=, so a reset cause that did not
-- fit lost exactly the record that says why the unit rebooted.  Nothing in the
-- server clamps it (the parsers store rst= as free text), and the firmware's
-- own assembly had no limit either: `pin+sw+wdt+dbg` plus four stall addresses
-- is 53 characters.  Firmware 0.4.84 keeps rst= to 47 when the hold breaker is
-- built in, and adds hb:<phase><s since the cut>/<s into the phase> to it after
-- a watchdog reset; older builds, and the bench, can still send more than 48.
--
-- The column is utf8mb3, so 64 characters is 192 bytes, under the 255 at which
-- the length prefix grows, and MariaDB changes it without touching a row.
-- ALGORITHM=INSTANT makes the statement fail instead of rebuilding the table
-- (1M rows, 170 MB) should the server not be able to.  Apply once:
--   mysql tracker < migrations/2026-10-26_log_rst_64.sql

ALTER TABLE `log`
  MODIFY COLUMN `rst` VARCHAR(64) DEFAULT NULL
    COMMENT 'reset cause(s), + joined, only after a reset',
  ALGORITHM=INSTANT;
