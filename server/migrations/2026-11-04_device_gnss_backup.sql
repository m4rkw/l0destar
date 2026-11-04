-- The GNSS backup supply state moves onto the device row; the history table
-- (2026-11-03_gnss_backup.sql) goes.
--
--   gnss_backup     1 = V_BCKP meant to be on, 0 = dropped, NULL = unknown
--   gnss_backup_at  when that state was set (unit's change time, or the API)
--
-- Apply once, before the server that writes them is started:
--   mysql tracker < migrations/2026-11-04_device_gnss_backup.sql

ALTER TABLE `device`
  ADD COLUMN `gnss_backup` TINYINT DEFAULT NULL COMMENT 'GNSS backup supply: 1 on, 0 dropped',
  ADD COLUMN `gnss_backup_at` DATETIME DEFAULT NULL COMMENT 'when gnss_backup was set',
  ALGORITHM=INSTANT;

-- Seed from the newest history row, then drop the table.
UPDATE `device` d JOIN `gnss_backup` g
  ON g.`device_id` = d.`id`
 AND g.`at` = (SELECT MAX(`at`) FROM `gnss_backup` WHERE `device_id` = d.`id`)
SET d.`gnss_backup` = g.`on`, d.`gnss_backup_at` = g.`at`;

DROP TABLE `gnss_backup`;
