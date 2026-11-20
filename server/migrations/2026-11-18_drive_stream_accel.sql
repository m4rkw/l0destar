-- Drive-stream accelerometer tunables: xim (longest gap between accel/gyro
-- readings in streamed records, ms, 0 = every record), xia (accel/gyro in the
-- drive stream at all, 0/1) and xic (impact capture while xia is 0, 0/1).
-- NULL = the unit keeps the value it was built with.  Apply once (safe to
-- repeat):
--   mysql tracker < migrations/2026-11-18_drive_stream_accel.sql
--
-- Track mode was removed at the same time.  Nothing here drops anything: the
-- device columns `track_mode`, `ap_track` and `track_period_ms` and the log
-- columns `track_mode` and `imu_burst` stay, unused (the log ones still take
-- what a unit on an old image sends), so no database needs a destructive
-- migration and old rows keep their data.

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `drive_stream_accel_ms` SMALLINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `drive_stream_accel` TINYINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `drive_stream_impact` TINYINT DEFAULT NULL;
