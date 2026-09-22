-- What each engine-off wake cost the device, in milliseconds.
--
-- Telemetry with the engine off is the expensive part of a parked unit's
-- life: every wake pays for a modem bring-up, a registration, a fix and a
-- send, and one that cannot register pays its whole search window before
-- giving up.  None of that was visible from the stored row, which said what
-- was sent but never what it took to send it.
--
-- The device measures from waking to going back to sleep with the modem off,
-- and reports the figure on its NEXT record (wt=<ms>) -- it is still awake
-- while building the record being measured, and the costly part of a bad
-- wake happens after that record is built.  process_record() writes it back
-- to the row that wake produced.  NULL means none was reported for that row.
--
-- Not to be confused with `waketime`, which despite the name holds seconds of
-- uptime (the eleventh fixed CSV field).
--
-- ALGORITHM=INSTANT: `log` is ~1M rows, and a nullable column added at the
-- end of the table is a metadata-only change on MariaDB 10.5+, so telemetry
-- inserts are not blocked behind a rebuild.  Drop the clause on anything
-- older, which will copy the table instead.  Apply once:
--   mysql tracker < migrations/2026-09-22_log_wake_ms.sql

ALTER TABLE `log`
  ADD COLUMN `wake_ms` INT UNSIGNED DEFAULT NULL
    COMMENT 'ms awake to complete this send, reported on the following record',
  ALGORITHM=INSTANT;
