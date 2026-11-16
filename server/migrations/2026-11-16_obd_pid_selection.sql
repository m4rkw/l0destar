-- OBD PID selection and the supported-PID scan.
--
-- device.obd_pids      the PIDs captured (xop), concatenated hex; NULL = default
-- device.obd_supported the car's supported-PID bitmaps (osp=), NULL = never scanned
-- device.obd_supported_at, device.obd_scan   when, and whether a scan is wanted (os=)
-- log.obd_extra        JSON of the PIDs that have no column of their own
-- Apply once (safe to repeat):
--   mysql tracker < migrations/2026-11-16_obd_pid_selection.sql

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `obd_pids` VARCHAR(24) DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `obd_supported` VARCHAR(64) DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `obd_supported_at` DATETIME DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `obd_scan` TINYINT(1) NOT NULL DEFAULT 0;

ALTER TABLE `log`
  ADD COLUMN IF NOT EXISTS `obd_extra` TEXT DEFAULT NULL;
