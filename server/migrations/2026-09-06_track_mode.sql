-- Track mode: the server-side switch the device follows, and the two fields
-- a track-mode record carries.  For databases created before schema.sql
-- gained these columns; apply once:
--   mysql -u root -p tracker < migrations/2026-09-06_track_mode.sql

ALTER TABLE `device`
  ADD COLUMN `track_mode` TINYINT(1) NOT NULL DEFAULT 0
    COMMENT 'GNSS off, ECU+IMU streamed fast; sent as track=<0|1> on every response';

ALTER TABLE `log`
  ADD COLUMN `track_mode` TINYINT(1) DEFAULT NULL
    COMMENT '1 when built in track mode (tm=1)',
  ADD COLUMN `imu_burst` TEXT DEFAULT NULL
    COMMENT 'acc=: ax/ay/az/gx/gy/gz per sample at 26 Hz, samples joined by :';
