-- The device event log: a line for each thing that happened to a unit
-- (ignition, firmware, commands, reboots, settings, results), read back by
-- GET /api/1.0/devices/events.  Apply once:
--   mysql tracker < migrations/2026-11-06_device_event.sql
-- then fill it from the existing log with tools/backfill_device_events.py.

CREATE TABLE IF NOT EXISTS `device_event` (
  `id`        INT          NOT NULL AUTO_INCREMENT,
  `device_id` INT          NOT NULL,
  `at`        DATETIME     NOT NULL,
  `type`      VARCHAR(16)  NOT NULL,
  `text`      VARCHAR(200) NOT NULL,
  `ref`       VARCHAR(96)  DEFAULT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `device_ref` (`device_id`, `ref`),
  KEY `device_at` (`device_id`, `at`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
