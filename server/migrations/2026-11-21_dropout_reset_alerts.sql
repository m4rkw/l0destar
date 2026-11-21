-- Per-device settings for the dropout alert (records stop for a while in a
-- drive) and the watchdog-reset alert.  Server-side only: never sent to the
-- unit.  alert_dropout_s 0 = dropout alerts off, else 5-3600 s.  Apply once
-- (safe to repeat):
--   mysql tracker < migrations/2026-11-21_dropout_reset_alerts.sql
--
-- Note: `device`.`id` is a signed int(11) on the live database, so nothing
-- here references it.

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `alert_dropout_s` SMALLINT UNSIGNED NOT NULL DEFAULT 30,
  ADD COLUMN IF NOT EXISTS `alert_dropout_priority` TINYINT NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS `alert_reset` TINYINT(1) NOT NULL DEFAULT 1,
  ADD COLUMN IF NOT EXISTS `alert_reset_priority` TINYINT NOT NULL DEFAULT 0;
