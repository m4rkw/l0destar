-- Per-device update state, so a bad image cannot be downloaded on a loop.
--
-- An image that boots but never confirms itself is reverted by MCUboot.
-- Without this the server keeps advertising it and the device keeps
-- fetching it: 300 KB a wake, which on a parked vehicle is a flattened
-- battery rather than a missed update.  Apply once:
--   mysql tracker < migrations/2026-09-12_fota_state.sql

ALTER TABLE `device`
  ADD COLUMN `fw_staged` VARCHAR(16) DEFAULT NULL
    COMMENT 'version the device reported staging, cleared once it is seen running',
  ADD COLUMN `fw_staged_at` DATETIME DEFAULT NULL
    COMMENT 'when that stage was reported; a different version running after the grace period is a revert',
  ADD COLUMN `fw_blocked` VARCHAR(16) DEFAULT NULL
    COMMENT 'version withheld from this device after it failed to boot here',
  ADD COLUMN `fw_fail_count` INT UNSIGNED NOT NULL DEFAULT 0
    COMMENT 'consecutive failed attempts at fw_blocked';
