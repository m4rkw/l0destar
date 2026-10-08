-- The motion through a gap between a device's GNSS fixes (firmware 0.4.68).
--
-- GNSS gets nothing while LTE holds the radio, which every send does, so a
-- track is runs of one-second fixes with gaps between them: 3-4 s on most
-- cells, 12 s on the ones that hold the connection for 10 s after a reply,
-- 25 s or more through a rejected tracking-area update.  The record that ends
-- a gap now carries mv=<gap>:<v0>:<steps>:<end>, the ECU's road speed and the
-- gyro's heading change for each second of it, and the journey replay fits
-- that path between the two fixes (tracker/deadreckon.py).  Stored as sent:
-- the path is worked out when a journey is drawn, not on arrival.
--
-- Nullable, at the end: instant.  Apply once:
--   mysql tracker < migrations/2026-10-04_log_motion.sql

ALTER TABLE `log`
  ADD COLUMN `motion` VARCHAR(255) DEFAULT NULL
    COMMENT 'mv=: <gap ds>:<v0 km/h>:<steps>:<end>, see deadreckon.py',
  ALGORITHM=INSTANT;
