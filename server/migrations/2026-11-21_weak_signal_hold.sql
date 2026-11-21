-- Weak-signal send hold tunables: weak_hold_rsrp_dbm (xwr, hold ordinary sends
-- while the serving cell's RSRP is at or below this many dBm, -140..-100;
-- 0 = off), weak_hold_snr_db (xws, or its SNR is at or below this, -20..5) and
-- weak_hold_max_s (xwm, the longest a send is held, 5-600 s).  NULL = the unit
-- keeps the value it was built with (the hold is off in a default build).
-- Apply once (safe to repeat):
--   mysql tracker < migrations/2026-11-21_weak_signal_hold.sql

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `weak_hold_rsrp_dbm` SMALLINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `weak_hold_snr_db` TINYINT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `weak_hold_max_s` SMALLINT DEFAULT NULL;
