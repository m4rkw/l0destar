-- Device-side record ids, and the wake figures that refer to them.
--
-- `log.rec_id` is the device's own id for a record (rid= on the wire):
-- consecutive within a boot, seeded at random at each one, so a gap in the
-- sequence is a record that never arrived and no two boots reuse the same
-- ids.  It exists because a figure that can only be worked out after a record
-- has gone -- how long the wake that sent it lasted, `wake_ms` -- has to be
-- able to name the record it belongs to.  Arrival order cannot: a wake whose
-- own send failed leaves its record in the device's backlog, which is flushed
-- BEHIND the live record carrying the figure, so "the row before this one" is
-- the wrong row in exactly the case the figure is most worth having.
--
-- `device.wake_pending_*` holds a figure whose record has not arrived yet --
-- that same backlog case -- until the record lands and process_record()
-- applies it.  At most one per device is ever outstanding, because the device
-- clears its own pending figure as it emits it.
--
-- Index (`device_id`, `rec_id`) so the write-back is a lookup rather than a
-- scan of the device's history on every wake.
--
-- ALGORITHM=INSTANT on the column; an index cannot be instant, but INPLACE
-- with LOCK=NONE builds it without blocking telemetry inserts.  Apply once,
-- after 2026-09-22_log_wake_ms.sql:
--   mysql tracker < migrations/2026-09-22_log_rec_id.sql

ALTER TABLE `log`
  ADD COLUMN `rec_id` INT UNSIGNED DEFAULT NULL
    COMMENT 'the device''s own id for this record (rid=), consecutive within a boot',
  ALGORITHM=INSTANT;

ALTER TABLE `log`
  ADD KEY `device_rec` (`device_id`, `rec_id`),
  ALGORITHM=INPLACE, LOCK=NONE;

ALTER TABLE `device`
  ADD COLUMN `wake_pending_rec_id` INT UNSIGNED DEFAULT NULL
    COMMENT 'log.rec_id the held figure belongs to',
  ADD COLUMN `wake_pending_ms` INT UNSIGNED DEFAULT NULL
    COMMENT 'that figure, applied when the record arrives';
