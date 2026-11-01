-- The board a build is for, stored beside the firmware version.
--
-- fw= says which build a row came from, not which board that build was made
-- for, and a version number cannot say: one sequence of versions carried a
-- unit through six board profiles, and the update manifest names a board only
-- for the update that fetched the image (a bridge release is offered under
-- the board the unit is leaving).  The first record after every boot now
-- carries bd=<board id>, for example v4.0p1+kline, and the server stores it
-- on every row the way it stores fw, carrying it forward while the version
-- stays the same.
--
-- Nullable, at the end of the row: instant.  ALGORITHM=INSTANT makes the
-- statement fail instead of rebuilding the table should the server not be
-- able to.  Apply once, before the server that stores it is started:
--   mysql tracker < migrations/2026-11-01_log_board.sql
-- Rows from before are filled from the published images by
--   python tools/backfill_board.py

ALTER TABLE `log`
  ADD COLUMN `board` VARCHAR(32) DEFAULT NULL
    COMMENT 'board the running build is for (bd=), carried forward while fw is the same',
  ALGORITHM=INSTANT;
