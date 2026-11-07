-- The GNSS backup off window: a daily span of whole hours (server local time,
-- may wrap midnight) in which the unit drops the GNSS backup supply.  Disabled
-- when gnss_off_window = 0 or the two hours are equal.  Apply once (safe to
-- repeat):
--   mysql tracker < migrations/2026-11-07_gnss_off_window.sql

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `gnss_off_window`    TINYINT(1)       NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS `gnss_off_hour_from` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS `gnss_off_hour_to`   TINYINT UNSIGNED NOT NULL DEFAULT 7;
