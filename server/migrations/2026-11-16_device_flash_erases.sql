-- Sector erases of the unit's external flash log (firmware fe=).
--
-- Sectors erased since the chip was new, as last reported; fc= (flash_cycles)
-- is this divided by the ring's 2016 sectors, so it stays 0 for the first lap.
-- Apply once (safe to repeat):
--   mysql tracker < migrations/2026-11-16_device_flash_erases.sql

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `flash_erases` INT UNSIGNED DEFAULT NULL;
