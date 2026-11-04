-- The newest answer to each reporting command (selftest, status, gnssstat,
-- backlog, logdump), one row per device and name.  Apply once:
--   mysql tracker < migrations/2026-11-04_device_report.sql

CREATE TABLE IF NOT EXISTS `device_report` (
  `device_id` INT          NOT NULL,
  `name`      VARCHAR(16)  NOT NULL,
  `text`      VARCHAR(600) NOT NULL,
  `at`        DATETIME     NOT NULL,
  PRIMARY KEY (`device_id`, `name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
