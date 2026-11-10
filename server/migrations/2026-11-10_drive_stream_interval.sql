-- The drive stream limit changes meaning: drive_stream_hz (xdh, records a
-- second, 0-10) is replaced by drive_stream_interval_s (xdi, seconds between
-- records, 0-900, 0 = as fast as possible).  Any old drive_stream_hz override
-- is dropped with the column, and the unit goes back to the built value.
-- Apply once (safe to repeat):
--   mysql tracker < migrations/2026-11-10_drive_stream_interval.sql

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `drive_stream_interval_s` SMALLINT UNSIGNED DEFAULT NULL;

ALTER TABLE `device`
  DROP COLUMN IF EXISTS `drive_stream_hz`;
