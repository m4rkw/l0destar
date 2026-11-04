-- The last network scan a unit reported (netscan command, "F,scan," line).
--
-- Nullable, at the end of the row: instant.  Apply once, before the server
-- that writes them is started:
--   mysql tracker < migrations/2026-11-04_device_net_scan.sql

ALTER TABLE `device`
  ADD COLUMN `net_scan` VARCHAR(600) DEFAULT NULL COMMENT 'cells heard in the last scan, mcc-mnc:tac:cell:earfcn:rsrp:rsrq10 joined by |',
  ADD COLUMN `net_scan_at` DATETIME DEFAULT NULL COMMENT 'when net_scan was reported',
  ALGORITHM=INSTANT;
