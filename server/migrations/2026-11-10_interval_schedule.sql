-- The engine-off reporting interval schedule: a JSON list of up to 8 rules
-- {"days": [0-6, Monday is 0], "from": "HH:MM", "to": "HH:MM", "int": seconds}
-- (server local time; first match wins; 0 = no timed reports, else 10-86400).
-- The interval sent in every reply is the first rule in force now, else the
-- plain `int`.  NULL = no schedule.  Apply once (safe to repeat):
--   mysql tracker < migrations/2026-11-10_interval_schedule.sql

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `interval_schedule` TEXT DEFAULT NULL;
