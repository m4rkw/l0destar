-- The runtime tunables: per-device overrides of values the firmware is built
-- with (Kconfig).  NULL, the default, means no override: the unit keeps the
-- value it was built with.  Every reply carries the non-NULL ones as
-- tu=<code>:<value>[;...] (tu=- when all are NULL).  Apply once (safe to
-- repeat):
--   mysql tracker < migrations/2026-11-10_device_tunables.sql

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `tow_tilt_deg` TINYINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `parked_impact_mg` SMALLINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `impact_immediate_mg` SMALLINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `accel_alert_priority` TINYINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `accel_alert_backoff_s` INT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `obd_alerts` VARCHAR(120) DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `ignition_on_sleep_s` SMALLINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `battery_warning_mv` SMALLINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `battery_poweroff_mv` SMALLINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `drive_stream` TINYINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `drive_stream_hz` TINYINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `track_period_ms` SMALLINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `databuf_drain_s` SMALLINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `network_search_s` INT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `resp_poll_s` SMALLINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `led_accel_wake` TINYINT DEFAULT NULL;
