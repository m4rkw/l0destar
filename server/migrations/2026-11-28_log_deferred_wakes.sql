-- Parked wakes that put their timed report off before this record (pd=,
-- intelligent power saving).
--
-- The server reads pd= (parse_csv_line's deferred_wakes) but does not store
-- it: after this, the column still has to be added to schema.sql and to
-- LOG_COLUMNS / the log entry in tracker/telemetry.py.
--
-- Nullable, at the end: instant.  Apply once (safe to repeat):
--   mysql tracker < migrations/2026-11-28_log_deferred_wakes.sql

ALTER TABLE `log`
  ADD COLUMN IF NOT EXISTS `deferred_wakes` INT UNSIGNED DEFAULT NULL
    COMMENT 'pd=: parked reports put off before this record',
  ALGORITHM=INSTANT;
