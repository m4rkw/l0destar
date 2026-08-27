"""Test configuration.

The pure-logic tests need no database.  They still import ``tracker.config``,
which insists on a config file, so one is written to a temp directory and
``TRACKER_CONFIG`` is pointed at it before the package is imported.

The integration tests need a real MySQL or MariaDB.  Point ``TRACKER_TEST_DB``
at a throwaway one and they run; leave it unset and they skip.  They are
destructive — every test truncates the tables it uses — so it must not be a
database with anything in it.

    TRACKER_TEST_DB=root:@127.0.0.1:3306/tracker_test pytest
"""

import os
import tempfile

import pytest
import yaml

_TMP = tempfile.mkdtemp(prefix='tracker-tests-')


def _parse_dsn(dsn):
    """user:password@host:port/database"""
    credentials, _, location = dsn.rpartition('@')
    user, _, password = credentials.partition(':')
    hostport, _, database = location.partition('/')
    host, _, port = hostport.partition(':')
    return {
        'host': host or '127.0.0.1',
        'port': int(port or 3306),
        'database': database,
        'user': user,
        'password': password,
    }


TEST_DSN = os.environ.get('TRACKER_TEST_DB')

_config = {
    'session_secret': 'tests-only-not-a-real-secret',
    'default_device': '350000000000000',
    'secure_cookies': False,
    'log_dir': os.path.join(_TMP, 'logs'),
    'fw_dir': os.path.join(_TMP, 'fw'),
    'notify': {'backend': 'none'},
    'database': _parse_dsn(TEST_DSN) if TEST_DSN else {
        'host': '127.0.0.1', 'port': 3306, 'database': 'tracker_test',
        'user': 'nobody', 'password': '',
    },
}

_config_path = os.path.join(_TMP, 'config.yaml')
with open(_config_path, 'w') as f:
    yaml.safe_dump(_config, f)

os.environ['TRACKER_CONFIG'] = _config_path

needs_db = pytest.mark.skipif(
    TEST_DSN is None,
    reason='set TRACKER_TEST_DB=user:pass@host:port/database to run',
)


@pytest.fixture
def fw_dir():
    """An empty firmware directory."""
    from tracker import config, firmware

    os.makedirs(config.FW_DIR, exist_ok=True)
    for name in os.listdir(config.FW_DIR):
        os.unlink(os.path.join(config.FW_DIR, name))
    firmware._manifest_cache.clear()
    return config.FW_DIR


@pytest.fixture
def database():
    """A database with the schema loaded and every table empty."""
    from tracker import db

    handle = db.web
    handle.query('SET FOREIGN_KEY_CHECKS = 0')
    for table in ('journey', 'command', 'log', 'device', 'plmn', 'api_token',
                  'user', 'registration', 'regoptions', 'authoptions',
                  'authoptions_ip'):
        handle.query('DELETE FROM `%s`' % table)
    handle.query('SET FOREIGN_KEY_CHECKS = 1')
    return handle


@pytest.fixture
def device(database):
    """One enrolled device, reporting hourly."""
    from tracker import db

    imei = '350000000000000'
    database.query(
        "INSERT INTO `device` (`imei`, `name`, `registration`, `psk`, `int`, "
        "`movement_alarm`) VALUES (%s, 'Car', 'AB12CDE', %s, 3600, 1)",
        (imei, 'aa' * 32),
    )
    database.query(
        "INSERT INTO `plmn` (`mcc`, `mnc`, `operator`) VALUES ('234', '10', 'Test Network')"
    )
    return db.lookup_device(imei=imei)


@pytest.fixture
def client(database):
    from tracker.web import create_app

    app = create_app()
    app.config['TESTING'] = True
    return app.test_client()


@pytest.fixture
def bearer(database):
    """Authorization header for the automation API."""
    database.query(
        "INSERT INTO `api_token` (`name`, `token`, `created_at`) "
        "VALUES ('tests', 'test-token', 0)"
    )
    return {'Authorization': 'Bearer test-token'}


@pytest.fixture
def logged_in(client):
    """A session cookie standing in for a completed passkey login."""
    with client.session_transaction() as session:
        session['username'] = 'tester'
    return client


@pytest.fixture
def published_for_device(fw_dir, device):
    """A newer firmware version published for the test device."""
    from tracker import firmware

    with open(os.path.join(fw_dir, 'manifest-%s.txt' % device['imei']), 'w') as f:
        f.write('version=0.4.12\nfile=l0destar-0.4.12-%s.bin\nboard=test\n'
                % device['imei'])
    firmware._manifest_cache.clear()
    return device


def record(minute, lat, lon, ignition, speed=0.0, extras=''):
    """Build a telemetry line at a given minute of the day."""
    hours, minutes = divmod(minute, 60)
    return ('12/08/26,%02d:%02d:00+01,%.6f,%.6f,%.1f,30.0,90.0,0.9,10,12.40,%d,5,0%s'
            % (hours, minutes, lat, lon, speed, ignition, extras))
