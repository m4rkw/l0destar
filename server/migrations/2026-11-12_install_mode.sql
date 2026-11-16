-- Installation mode (firmware im=, ir=, ie=): a unit being fitted.
--
-- install_mode is set by POST /config; the unit answers instreport /
-- instenable commands with a check (install_report) and an enable outcome
-- (install_enable), both JSON.  Apply once (safe to repeat):
--   mysql tracker < migrations/2026-11-12_install_mode.sql

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `install_mode` TINYINT(1) NOT NULL DEFAULT 0,
  ADD COLUMN IF NOT EXISTS `install_report` TEXT DEFAULT NULL,
  ADD COLUMN IF NOT EXISTS `install_enable` TEXT DEFAULT NULL;
