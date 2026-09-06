-- l0destar tracking server schema (MySQL / MariaDB).
--
--   mysql -u root -p -e "CREATE DATABASE tracker CHARACTER SET utf8mb4"
--   mysql -u root -p tracker < schema.sql
--
-- Storage note: `log` is the only table that grows without bound.  A device
-- reporting once a minute writes about half a million rows a year, so the
-- indexes below are the ones the read paths actually use and nothing else —
-- every extra index is paid for on every insert, and inserts happen while a
-- device is holding its radio open waiting for the response.

SET NAMES utf8mb4;


-- One row per tracker.
CREATE TABLE `device` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `imei`          VARCHAR(16)  NOT NULL,
  `name`          VARCHAR(64)  NOT NULL DEFAULT '',
  `registration`  VARCHAR(32)  DEFAULT NULL COMMENT 'vehicle plate, shown in the UI',

  -- Pre-shared key for the UDP transport: 32 bytes as 64 hex characters.
  -- NULL disables the UDP transport for this device.
  `psk`           CHAR(64)     DEFAULT NULL,
  -- Most recent accepted nonce, so a restart cannot reopen a replay window
  -- for the single most-recently captured datagram.
  `last_nonce`    VARBINARY(12) DEFAULT NULL,

  -- Settings the device syncs and applies.
  `int`           INT UNSIGNED NOT NULL DEFAULT 0  COMMENT 'reporting interval, seconds',
  `movement_alarm` TINYINT(1)  NOT NULL DEFAULT 1  COMMENT 'wake and report on accelerometer trigger',

  -- Settings the server acts on; these never reach the device.
  `alarm`         TINYINT(1)   NOT NULL DEFAULT 0  COMMENT 'notify on ignition on',
  `garage`        TINYINT(1)   NOT NULL DEFAULT 0  COMMENT 'expected to be moved; downgrades alert priority',
  `overnight_alarm` TINYINT(1) NOT NULL DEFAULT 0,
  `overnight_alarm_hour_from` TINYINT UNSIGNED DEFAULT 23,
  `overnight_alarm_hour_to`   TINYINT UNSIGNED DEFAULT 6,

  `created_at`    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `imei` (`imei`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- Telemetry.  One row per record the device sent.
CREATE TABLE `log` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `device_id`     INT UNSIGNED NOT NULL,
  `ip`            VARCHAR(45)  DEFAULT NULL,

  -- `timestamp` is server arrival time; `gsm_timestamp` is the modem's own
  -- clock with its UTC offset in a separate column.  Both are kept because
  -- they disagree in the cases that matter: a device buffering records while
  -- out of coverage, and a cold boot before the network has supplied a time.
  `timestamp`     DATETIME(6)  NOT NULL,
  `gsm_timestamp` DATETIME(6)  DEFAULT NULL,
  `gsm_timestamp_offset` SMALLINT DEFAULT NULL COMMENT 'quarter-hours east of UTC',

  `latitude`      DECIMAL(10,7) DEFAULT NULL,
  `longitude`     DECIMAL(10,7) DEFAULT NULL,
  `altitude`      DECIMAL(8,2)  DEFAULT NULL COMMENT 'metres',
  `speed`         DECIMAL(6,2)  DEFAULT NULL COMMENT 'mph',
  `heading`       DECIMAL(6,2)  DEFAULT NULL COMMENT 'degrees true',
  `hdop`          DECIMAL(5,2)  DEFAULT NULL,
  `satellites`    TINYINT UNSIGNED DEFAULT NULL,

  `ignition_state` TINYINT(1)  NOT NULL DEFAULT 0,
  `powered_on`    TINYINT(1)   NOT NULL DEFAULT 0 COMMENT 'this row is the ignition-on transition',
  `battery_level` DECIMAL(5,2) DEFAULT NULL COMMENT 'vehicle battery, volts',
  `vsys`          DECIMAL(4,2) DEFAULT NULL COMMENT 'SiP supply rail, volts',

  -- Serving cell.  Sent only when it changes or on the first record after a
  -- wake; the server carries the last known value forward onto every row.
  `mcc`           VARCHAR(4)   DEFAULT NULL,
  `mnc`           VARCHAR(4)   DEFAULT NULL,
  `lac`           VARCHAR(8)   DEFAULT NULL,
  `cid`           VARCHAR(12)  DEFAULT NULL,
  `rat`           VARCHAR(8)   DEFAULT NULL COMMENT 'CATM1 | NBIOT',
  `cell_location` TINYINT(1)   DEFAULT 0 COMMENT 'position is cell-derived, not GNSS',

  -- IMU, raw LSB as the sensor reports them.  Stored unscaled so a change of
  -- full-scale range in firmware does not silently reinterpret old rows.
  `accel_x`       SMALLINT     DEFAULT NULL,
  `accel_y`       SMALLINT     DEFAULT NULL,
  `accel_z`       SMALLINT     DEFAULT NULL,
  `gyro_x`        SMALLINT     DEFAULT NULL,
  `gyro_y`        SMALLINT     DEFAULT NULL,
  `gyro_z`        SMALLINT     DEFAULT NULL,
  `imu_temp`      DECIMAL(5,2) DEFAULT NULL COMMENT 'degrees C',
  `mcu_temp`      DECIMAL(5,2) DEFAULT NULL COMMENT 'degrees C',

  `waketime`      INT UNSIGNED DEFAULT NULL COMMENT 'seconds awake for this send',
  `uptime`        INT UNSIGNED DEFAULT NULL COMMENT 'seconds since boot',
  `dead_reckoning` TINYINT(1)  DEFAULT NULL,

  `fw`            VARCHAR(16)  DEFAULT NULL COMMENT 'running firmware version, carried forward',
  `dbg`           VARCHAR(255) DEFAULT NULL COMMENT 'debug counters, only after a fault',
  `rst`           VARCHAR(64)  DEFAULT NULL COMMENT 'reset cause, only after a reset',

  -- OBD-II over the K wire.  Only on vehicles with a K interface and only
  -- for the PIDs the ECU supports, so every column is independently NULL and
  -- nothing is carried forward.  Scaled on receipt; see OBD_FIELDS.
  `obd_rpm`         SMALLINT UNSIGNED DEFAULT NULL,
  `obd_rpm_min`     SMALLINT UNSIGNED DEFAULT NULL COMMENT 'over the reporting cycle',
  `obd_rpm_max`     SMALLINT UNSIGNED DEFAULT NULL,
  `obd_rpm_avg`     SMALLINT UNSIGNED DEFAULT NULL,
  `obd_speed`       DECIMAL(6,2) DEFAULT NULL COMMENT 'mph, from the ECU',
  `obd_coolant`     SMALLINT     DEFAULT NULL COMMENT 'degrees C',
  `obd_intake`      SMALLINT     DEFAULT NULL COMMENT 'degrees C',
  `obd_load`        DECIMAL(5,1) DEFAULT NULL COMMENT 'percent',
  `obd_throttle`    DECIMAL(5,1) DEFAULT NULL COMMENT 'percent',
  `obd_maf`         DECIMAL(7,2) DEFAULT NULL COMMENT 'g/s',
  `obd_timing`      DECIMAL(5,1) DEFAULT NULL COMMENT 'degrees advance',
  `obd_stft`        DECIMAL(5,1) DEFAULT NULL COMMENT 'short-term fuel trim, percent',
  `obd_ltft`        DECIMAL(5,1) DEFAULT NULL COMMENT 'long-term fuel trim, percent',
  `obd_fuel_status` SMALLINT UNSIGNED DEFAULT NULL COMMENT 'raw PID 03 bitmap',
  `obd_mil`         TINYINT(1)   DEFAULT NULL,
  `obd_dtc_count`   TINYINT UNSIGNED DEFAULT NULL COMMENT 'stored codes',

  -- What the interface shows: the ECU's road speed when reported, else GNSS.
  `combined_speed`  DECIMAL(6,2) DEFAULT NULL COMMENT 'mph',

  PRIMARY KEY (`id`),
  -- The read paths are "latest row for a device" and "rows between two ids
  -- for a device", both served by this one index.
  KEY `device_id` (`device_id`, `id`),
  CONSTRAINT `log_device` FOREIGN KEY (`device_id`) REFERENCES `device` (`id`)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- Stored fault codes as the ECU reports them over the K wire.  Rows are never
-- deleted: a code that goes away is marked inactive with the time it cleared,
-- so the table is a history of when each fault appeared and disappeared.
CREATE TABLE `dtc` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `device_id`     INT UNSIGNED NOT NULL,
  `code`          CHAR(5)      NOT NULL COMMENT 'P0133 etc.',
  `raised_at`     DATETIME(6)  NOT NULL,
  `cleared_at`    DATETIME(6)  DEFAULT NULL,
  `active`        TINYINT(1)   NOT NULL DEFAULT 1,
  PRIMARY KEY (`id`),
  KEY `device_active` (`device_id`, `active`),
  CONSTRAINT `dtc_device` FOREIGN KEY (`device_id`) REFERENCES `device` (`id`)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- Ignition-on to ignition-off, with the log id range it covers.
CREATE TABLE `journey` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `device_id`     INT UNSIGNED NOT NULL,
  `start_time`    DATETIME(6)  NOT NULL,
  `end_time`      DATETIME(6)  DEFAULT NULL COMMENT 'NULL while the journey is in progress',
  `start_log_id`  BIGINT UNSIGNED DEFAULT NULL,
  `end_log_id`    BIGINT UNSIGNED DEFAULT NULL,
  `from_latitude`  DECIMAL(10,7) DEFAULT NULL,
  `from_longitude` DECIMAL(10,7) DEFAULT NULL,
  `to_latitude`    DECIMAL(10,7) DEFAULT NULL,
  `to_longitude`   DECIMAL(10,7) DEFAULT NULL,
  `miles`         DECIMAL(8,2) DEFAULT NULL,
  -- Optional human place names.  Nothing in this repository fills them in;
  -- they are here for a deployment that wants to run its own geocoder.
  `from_place`    VARCHAR(128) DEFAULT NULL,
  `to_place`      VARCHAR(128) DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `device_start` (`device_id`, `start_time`),
  CONSTRAINT `journey_device` FOREIGN KEY (`device_id`) REFERENCES `device` (`id`)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- Commands waiting for a device to check in.  Rows are deleted as they are
-- handed over, so delivery is at-most-once: a command lost to a dropped reply
-- is re-queued by whoever issued it, which is safer than replaying a
-- `poweroff` after the operator has changed their mind.
CREATE TABLE `command` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `device_id`     INT UNSIGNED NOT NULL,
  `timestamp`     DATETIME(6)  NOT NULL,
  `command`       VARCHAR(255) NOT NULL,
  PRIMARY KEY (`id`),
  KEY `device_id` (`device_id`),
  CONSTRAINT `command_device` FOREIGN KEY (`device_id`) REFERENCES `device` (`id`)
    ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- MCC/MNC to carrier name, for showing which network a device was on.
-- Optional: an empty table just means no operator name in the UI.
CREATE TABLE `plmn` (
  `mcc`           VARCHAR(4)   NOT NULL,
  `mnc`           VARCHAR(4)   NOT NULL,
  `operator`      VARCHAR(64)  NOT NULL,
  `country`       VARCHAR(64)  DEFAULT NULL,
  PRIMARY KEY (`mcc`, `mnc`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- -- accounts ---------------------------------------------------------------
-- Passkeys only; there is no password column anywhere by design.

CREATE TABLE `user` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `username`      VARCHAR(64)  NOT NULL,
  `user_id`       VARCHAR(255) NOT NULL COMMENT 'WebAuthn credential id',
  `credential`    TEXT         NOT NULL COMMENT 'JSON: credential_id, public_key, sign_count',
  `failed_login_count` INT UNSIGNED NOT NULL DEFAULT 0,
  `locked`        TINYINT(1)   NOT NULL DEFAULT 0,
  `created_at`    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `username` (`username`),
  KEY `user_id` (`user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- Single-use enrolment invitations minted by tools/regtoken.py.
CREATE TABLE `registration` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `username`      VARCHAR(64)  NOT NULL,
  `token`         CHAR(64)     NOT NULL,
  `timestamp`     INT UNSIGNED NOT NULL,
  PRIMARY KEY (`id`),
  KEY `username_token` (`username`, `token`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- In-flight WebAuthn challenges.  Both tables are scratch space; nothing is
-- lost by truncating them.
CREATE TABLE `regoptions` (
  `session_id`    CHAR(36)     NOT NULL,
  `regoptions`    TEXT         NOT NULL,
  `timestamp`     INT UNSIGNED NOT NULL,
  PRIMARY KEY (`session_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE `authoptions` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `user_id`       VARCHAR(255) NOT NULL,
  `session_id`    CHAR(36)     NOT NULL,
  `authoptions`   TEXT         NOT NULL COMMENT 'base64 challenge',
  `timestamp`     INT UNSIGNED NOT NULL,
  -- The challenge is bound to the user agent and IP that requested it, so one
  -- intercepted in flight cannot be completed from somewhere else.
  `useragent`     VARCHAR(255) NOT NULL DEFAULT '',
  `ipaddr`        VARCHAR(45)  NOT NULL DEFAULT '',
  PRIMARY KEY (`id`),
  UNIQUE KEY `session_id` (`session_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE `authoptions_ip` (
  `ip`            VARCHAR(45)  NOT NULL,
  `count`         INT UNSIGNED NOT NULL DEFAULT 0,
  `last_request_timestamp` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`ip`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- Bearer tokens for the automation API.  Opaque and unscoped: anything
-- holding one can queue a command, so treat one as console-equivalent.
CREATE TABLE `api_token` (
  `id`            INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `name`          VARCHAR(64)  NOT NULL,
  `token`         VARCHAR(128) NOT NULL,
  `created_at`    INT UNSIGNED NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `token` (`token`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
