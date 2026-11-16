-- Wear of the unit's external flash log (firmware fc=).
--
-- The most erase cycles any sector of the log has had (the chip is rated for
-- 100,000), as last reported, and when it last changed.  NULL until a unit
-- with the flash backlog has opened its log.  Apply once (safe to repeat):
--   mysql tracker < migrations/2026-11-12_device_flash_cycles.sql

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `flash_cycles` INT UNSIGNED DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `flash_cycles_at` DATETIME DEFAULT NULL;
