-- A reporting interval for a tracker running on the inline backup module.
--
-- With the module fitted, a cut to the car's supply leaves the tracker on a
-- cell of its own, and `int` -- written for a parked car on its own battery --
-- is not necessarily the cadence wanted then: a longer one spares the cell, a
-- shorter one follows a car whose power has been cut more closely.  This is
-- that cadence.  0, which is what every existing device gets, means `int`
-- applies on backup power too, exactly as before.
--
-- It travels like track_mode: every response carries backup=<seconds>, so a
-- device that rebooted or missed a reply converges on it, and the firmware
-- (0.4.79 and later) uses it only while the module is carrying the tracker.
-- Older firmware ignores the field.  Nothing on the device sets it, so unlike
-- `int` it is never reported back.  Apply once, before restarting the server
-- on code that reads it:
--   mysql tracker < migrations/2026-10-18_device_backup_int.sql

ALTER TABLE `device`
  ADD COLUMN `backup_int` INT UNSIGNED NOT NULL DEFAULT 0
    COMMENT 'reporting interval while the backup module carries the supply, seconds; 0 = the same as int; sent as backup=<n> on every response';
