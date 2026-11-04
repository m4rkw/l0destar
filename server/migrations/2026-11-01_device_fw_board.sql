-- The firmware version and board a device is running, on its own row.
--
-- Every log row carries fw and board, carried forward between boots, so the
-- newest row says what a unit runs - but only to a query of a table that
-- grows without limit.  The server now keeps the two figures on the device row
-- as well, taken from its live records and written when either changes (a
-- restart into an update, mostly).  The device list, the API and
-- tools/device.py read them from here.
--
-- Nullable, at the end of the row: instant.  ALGORITHM=INSTANT makes the
-- statement fail instead of rebuilding the table should the server not be
-- able to.  Apply once, before the server that writes them is started:
--   mysql tracker < migrations/2026-11-01_device_fw_board.sql
-- A device keeps no version until it sends a record, so one that is switched
-- off is filled from its newest row by
--   python tools/backfill_device_versions.py

ALTER TABLE `device`
  ADD COLUMN `fw` VARCHAR(16) DEFAULT NULL
    COMMENT 'firmware version the newest live record reported',
  ADD COLUMN `board` VARCHAR(32) DEFAULT NULL
    COMMENT 'board that build is for (bd=); empty when not known',
  ALGORITHM=INSTANT;
