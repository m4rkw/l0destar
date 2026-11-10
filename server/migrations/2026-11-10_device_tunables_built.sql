-- The built tunable values a unit reports (tb=, once per boot): the Kconfig /
-- remote.conf values it was compiled with, <code>:<value>[;...] in the tu=
-- grammar with xoa's commas as '|'.  Raw text, NULL until the unit has
-- reported.  GET /config and /devices use it for the tunables_built and
-- tunables objects.  Apply once (safe to repeat):
--   mysql tracker < migrations/2026-11-10_device_tunables_built.sql

ALTER TABLE `device`
  ADD COLUMN IF NOT EXISTS `tunables_built` VARCHAR(400) DEFAULT NULL;
