-- The map shows a device's newest record by the device's own clock rather
-- than by arrival, so a backlog flushed after an outage does not put an old
-- position on screen (tracker/web/devices.py latest_log).  That read needs
-- an index of its own, or it sorts the device's whole history on every map
-- load.  Apply once:
--   mysql tracker < migrations/2026-09-21_log_device_time.sql

ALTER TABLE `log`
  ADD KEY `device_time` (`device_id`, `gsm_timestamp`);
