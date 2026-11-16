-- GNSS backup supply while on backup power (firmware gx=): 0 keep on (the
-- default), 1 its own window, 2 follow the regular window.  Apply once
-- (safe to repeat):
--   mysql tracker < migrations/2026-11-15_gnss_backup_power_window.sql

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `gnss_bk_window` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS `gnss_bk_hour_from` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS `gnss_bk_hour_to` TINYINT UNSIGNED NOT NULL DEFAULT 7;
