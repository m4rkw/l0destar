-- What a device says it is running (ap=) and the last command it ran (ak=),
-- on its row, for the app to show.
--
--   ap_int, ap_ma, ap_backup, ap_track   the settings in force on the unit,
--                                        sent after a boot and on a change
--   ap_at                                when the server heard it
--   ack_cmd, ack_at                      the last command(s) the unit reported
--                                        having run ("locatenow", "reboot"),
--                                        and when
--
-- Nullable, at the end of the row: instant.  Apply once, before the server
-- that writes them is started:
--   mysql tracker < migrations/2026-11-04_device_applied.sql

ALTER TABLE `device`
  ADD COLUMN `ap_int` INT DEFAULT NULL COMMENT 'engine-off interval the unit reported running',
  ADD COLUMN `ap_ma` TINYINT DEFAULT NULL COMMENT 'movement alarm the unit reported running',
  ADD COLUMN `ap_backup` INT DEFAULT NULL COMMENT 'backup-power interval the unit reported running',
  ADD COLUMN `ap_track` TINYINT DEFAULT NULL COMMENT 'track mode the unit reported running',
  ADD COLUMN `ap_at` DATETIME DEFAULT NULL COMMENT 'when the settings above were reported',
  ADD COLUMN `ack_cmd` VARCHAR(48) DEFAULT NULL COMMENT 'commands the unit last reported having run, joined by |',
  ADD COLUMN `ack_at` DATETIME DEFAULT NULL COMMENT 'when ack_cmd was reported',
  ALGORITHM=INSTANT;
