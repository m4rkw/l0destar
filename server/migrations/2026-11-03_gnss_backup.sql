-- Changes of the GNSS receiver's backup supply, reported by the device (bk=)
-- and filed at the time they happened.  A new table: nothing existing is
-- altered.  Apply once, before the server that writes it is started:
--   mysql tracker < migrations/2026-11-03_gnss_backup.sql
-- The foreign key needs device_id to match device.id.  The live database
-- declares it signed (INT, not INT UNSIGNED) and was created that way; check
-- information_schema first and drop UNSIGNED here if yours is the same.

CREATE TABLE IF NOT EXISTS `gnss_backup` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `device_id`     INT UNSIGNED NOT NULL,
  `at`            DATETIME(6)  NOT NULL,
  `on`            TINYINT(1)   NOT NULL COMMENT '1 = V_BCKP supplied, 0 = dropped',
  PRIMARY KEY (`id`),
  UNIQUE KEY `device_at` (`device_id`, `at`),
  CONSTRAINT `gnss_backup_device` FOREIGN KEY (`device_id`) REFERENCES `device` (`id`)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
