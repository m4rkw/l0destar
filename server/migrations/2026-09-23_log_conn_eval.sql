-- Why the signal is what it is, not just how strong it arrived.
--
-- The car reports RSRP -108 to -112 dBm on band 20 (800 MHz, the best
-- propagating band available to it), from two separate eNBs within 4 dB of
-- each other.  That is cell edge, and RSRP alone cannot say whether it is the
-- parking spot or the antenna: both look the same from one place.
--
--   pathloss   the modem's downlink path-loss estimate, dB.  RSRP says how
--              strong the signal arrived; this says how much was lost getting
--              here, normalised against what the cell says it transmits --
--              so unlike RSRP it is comparable across cells and distances,
--              and excess attenuation in the antenna path shows up directly.
--   rsrq       reference signal received quality, dB.  Falls with RSRP when
--              the link is noise limited, holds up when it is attenuation.
--   ce_level   LTE-M coverage enhancement level.  Above 0 means the network
--              is using repetitions -- the mechanism that makes the same
--              attach take 40 s on a cell that answers in 2 s at good signal.
--   tx_rep     estimated transmit repetitions, the uplink half of the same.
--
-- All four come from one connection evaluation and are absent whenever the
-- modem refuses it -- "radio busy" is what a drive with GNSS running usually
-- answers -- so they are sparser than rsrp/snr/band and NULL means only that
-- no evaluation ran.  None of them are carried forward: they are
-- measurements of a moment.  Apply once:
--   mysql tracker < migrations/2026-09-23_log_conn_eval.sql

ALTER TABLE `log`
  ADD COLUMN `pathloss` SMALLINT DEFAULT NULL
    COMMENT 'downlink path loss in dB, normalised against the cell transmit power',
  ADD COLUMN `rsrq` DECIMAL(4,1) DEFAULT NULL
    COMMENT 'reference signal received quality, dB',
  ADD COLUMN `ce_level` TINYINT DEFAULT NULL
    COMMENT 'LTE-M coverage enhancement level; >0 means repetitions',
  ADD COLUMN `tx_rep` SMALLINT DEFAULT NULL
    COMMENT 'estimated transmit repetitions',
  ALGORITHM=INSTANT;
