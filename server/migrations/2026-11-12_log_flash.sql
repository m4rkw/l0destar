-- Records that were buffered in the unit's external flash (fl=1).
--
-- The flash backlog moves the oldest records out of the RAM ring when it is
-- full.  Each record that comes back out of the flash is sent with fl=1, so
-- how often the tier is used can be counted over time:
--   SELECT DATE(`timestamp`), COUNT(*) FROM log WHERE flash = 1 GROUP BY 1;
--
-- Nullable, at the end: instant.  Apply once:
--   mysql tracker < migrations/2026-11-12_log_flash.sql

ALTER TABLE `log`
  ADD COLUMN `flash` TINYINT(1) DEFAULT NULL
    COMMENT 'fl=1: the record was buffered in the external flash',
  ALGORITHM=INSTANT;
