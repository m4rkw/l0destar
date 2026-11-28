-- Intelligent power saving and log hold tunables: smart_power (xsp, 0/1),
-- smart_power_rsrp_dbm (xsr, -140..-90), smart_power_battery_mv (xsb,
-- 11000-13000), smart_power_battery_drop_mv (xsd, 0-1000 mV, 0 = off),
-- smart_power_max_defer_s (xsm, 0-86400) and log_hold_rsrp_dbm
-- (xlh, -140..-90 dBm, 0 = off).  NULL = the unit keeps the value it was built
-- with.  Apply once (safe to repeat):
--   mysql tracker < migrations/2026-11-28_smart_power.sql

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `smart_power` TINYINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `smart_power_rsrp_dbm` SMALLINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `smart_power_battery_mv` SMALLINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `smart_power_battery_drop_mv` SMALLINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `smart_power_max_defer_s` INT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `log_hold_rsrp_dbm` SMALLINT DEFAULT NULL;
