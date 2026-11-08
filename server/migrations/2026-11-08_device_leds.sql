-- The device LEDs setting: whether the unit's status LEDs may light (1, the
-- default) or stay all dark (0).  Sent in every reply as le=<0|1>.  Apply once
-- (safe to repeat):
--   mysql tracker < migrations/2026-11-08_device_leds.sql

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `leds` TINYINT(1) NOT NULL DEFAULT 1;
