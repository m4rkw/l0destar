-- Replay protection that survives a restart.
--
-- The UDP listener kept each device's last 1024 nonces in memory and only the
-- most recent in device.last_nonce, so after a restart any earlier captured
-- datagram could be replayed once, and one older than the last 1024 at any
-- time.  Every nonce a device uses is now kept here for 30 days.  Apply once:
--   mysql tracker < migrations/2026-09-14_device_nonce.sql

CREATE TABLE IF NOT EXISTS `device_nonce` (
  `device_id`     INT UNSIGNED NOT NULL,
  `nonce`         BINARY(12)   NOT NULL,
  `seen_at`       INT UNSIGNED NOT NULL COMMENT 'unix time',
  PRIMARY KEY (`device_id`, `nonce`),
  KEY `seen_at` (`seen_at`),
  CONSTRAINT `device_nonce_device` FOREIGN KEY (`device_id`) REFERENCES `device` (`id`)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
