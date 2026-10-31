-- u-blox AssistNow orbit assistance for devices with an external GNSS receiver.
--
-- The server registers each receiver with u-blox's service, downloads its
-- orbits once a day and serves the part a device can use over the TLS listener
-- (/assist/data.bin), so the device holds nothing that a reboot or a firmware
-- update could lose and u-blox's per-device request limit is spent by the
-- server on a schedule of its own.  One row per device that has registered:
-- the receiver's chip id it registered with, the credential u-blox issued for
-- that receiver (a secret, like `device`.`psk`), and the last download as
-- received.  Nothing changes for a device without this, or with the
-- `assistnow` section absent from config.yaml.
--
-- Safe to run more than once, and on a database whose `device`.`id` is not
-- the INT UNSIGNED of schema.sql: the live one, made before it, has a signed
-- int(11), and a foreign key will not be made between columns that differ in
-- signedness (error 150).  So the table is made first and `device_id` is then
-- given exactly the type `device`.`id` has, with the key.
--
-- Apply once, before restarting the server on code that reads it:
--   mysql tracker < migrations/2026-10-30_device_assist.sql

CREATE TABLE IF NOT EXISTS `device_assist` (
  `device_id`     INT UNSIGNED NOT NULL,
  `uniqid`        CHAR(12)     DEFAULT NULL COMMENT 'the receiver chip id (6 bytes, hex) this registered with',
  `chipcode`      VARCHAR(255) DEFAULT NULL COMMENT 'u-blox credential for that receiver; a secret',
  `service_url`   VARCHAR(255) DEFAULT NULL COMMENT 'where u-blox said to fetch orbits from',
  `registered_at` DATETIME     DEFAULT NULL,
  `fetched_at`    DATETIME     DEFAULT NULL COMMENT 'when `blob` was downloaded',
  `blob_until`    DATE         DEFAULT NULL COMMENT 'last day the orbits in `blob` are stamped for',
  `blob_days`     TINYINT UNSIGNED DEFAULT NULL COMMENT 'days of orbits asked for',
  `next_try_at`   DATETIME     DEFAULT NULL COMMENT 'no download attempts before this (back-off, usage limit)',
  `last_error`    VARCHAR(255) DEFAULT NULL,
  `blob`          MEDIUMBLOB   DEFAULT NULL COMMENT 'the service response as received: UBX-MGA messages',
  PRIMARY KEY (`device_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

SET @assist_fk = IF(
  (SELECT COUNT(*) FROM `information_schema`.`table_constraints`
    WHERE `table_schema` = DATABASE() AND `table_name` = 'device_assist'
      AND `constraint_name` = 'device_assist_device') > 0,
  'DO 0',
  CONCAT('ALTER TABLE `device_assist` MODIFY `device_id` ',
         (SELECT `column_type` FROM `information_schema`.`columns`
           WHERE `table_schema` = DATABASE() AND `table_name` = 'device' AND `column_name` = 'id'),
         ' NOT NULL, ADD CONSTRAINT `device_assist_device` FOREIGN KEY (`device_id`) ',
         'REFERENCES `device` (`id`) ON DELETE CASCADE'));
PREPARE `assist_fk` FROM @assist_fk;
EXECUTE `assist_fk`;
DEALLOCATE PREPARE `assist_fk`;
