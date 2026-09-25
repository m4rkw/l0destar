-- Why a wake took as long as it did.
--
-- An engine-off wake is dominated by the LTE attach.  Measured across six
-- wakes on 2026-09-23 the send was uniformly fast -- 0.51 to 0.72 s from the
-- device building the record to the server storing it -- while the wakes
-- themselves ran 3.9 s to 44.6 s on the same cell, same RAT, with no GPS
-- involved.  All of the variance is before the record exists, which is the
-- attach.  `wake_ms` alone could not show that; these columns can.
--
--   attach_ms  how much of wake_ms went on registering, measured on the
--              device from CFUN=1 to the registration event (not from the
--              1 Hz poll, which rounds every attach up to a whole second).
--              NULL when no attach was measured; 0 when the modem was
--              already registered and the wake paid none.
--
--   rsrp/snr/band  the serving cell's signal quality at the time
--              (AT%XMONITOR), in dBm, dB and LTE band.  LTE-M raises its
--              repetition count as coverage worsens, so the same procedure
--              on the same cell takes several times longer at a lower RSRP
--              -- this is what makes that testable rather than assumed.
--              NOT carried forward between rows: signal moves continuously,
--              and a copied value would read as a measurement never taken.
--              NULL means "not measured on this record", which is every row
--              but the first after a wake or a cell change.
--
-- attach_ms arrives with wake_ms, on a later record, and is written back the
-- same way -- see 2026-09-22_log_rec_id.sql.  Apply once, after that one:
--   mysql tracker < migrations/2026-09-23_log_attach_signal.sql

ALTER TABLE `log`
  ADD COLUMN `attach_ms` INT UNSIGNED DEFAULT NULL
    COMMENT 'ms of wake_ms spent on the LTE attach; 0 = already registered',
  ADD COLUMN `rsrp` SMALLINT DEFAULT NULL
    COMMENT 'serving cell RSRP in dBm, -140..-44',
  ADD COLUMN `snr` SMALLINT DEFAULT NULL
    COMMENT 'serving cell SNR in dB, -24..+24',
  ADD COLUMN `band` TINYINT UNSIGNED DEFAULT NULL
    COMMENT 'LTE band in use',
  ALGORITHM=INSTANT;

ALTER TABLE `device`
  ADD COLUMN `wake_pending_attach_ms` INT UNSIGNED DEFAULT NULL
    COMMENT 'attach part of the held wake figure, applied with it';
