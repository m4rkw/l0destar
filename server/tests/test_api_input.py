"""What the API does with input it cannot use.

Skipped unless ``TRACKER_TEST_DB`` is set; see conftest.py.
"""

import pytest

from conftest import needs_db, record

pytestmark = needs_db

IMEI = '350000000000000'


@pytest.mark.parametrize('body, message', [
    ({'int': 3.7}, 'int must be a whole number from 0 to 4294967295'),
    ({'int': -1}, 'int must be a whole number from 0 to 4294967295'),
    ({'al': True}, 'al must be a whole number from 0 to 1'),
    ({'oaf': 24}, 'oaf must be a whole number from 0 to 23'),
])
def test_config_refuses_values_it_cannot_store(client, bearer, device, database, body, message):
    response = client.post('/api/1.0/config', headers=bearer, json=dict(body, imei=IMEI))
    assert response.status_code == 400
    assert response.get_json() == {'status': 'error', 'message': message}
    row = database.one('SELECT `int`, `alarm`, `overnight_alarm_hour_from` FROM `device` '
                       'WHERE `imei` = %s', (IMEI,))
    assert row == {'int': 3600, 'alarm': 0, 'overnight_alarm_hour_from': 23}


def test_config_takes_a_whole_number_given_as_a_string(client, bearer, device, database):
    response = client.post('/api/1.0/config', headers=bearer, json={'imei': IMEI, 'int': '900'})
    assert response.status_code == 200
    assert database.one('SELECT `int` FROM `device` WHERE `imei` = %s', (IMEI,))['int'] == 900


def test_server_side_settings_in_a_command_are_checked(client, bearer, device):
    response = client.post('/api/1.0/command', headers=bearer,
                           json={'imei': IMEI, 'command': 'garage=2'})
    assert response.status_code == 400
    assert response.get_json()['message'] == 'garage must be a whole number from 0 to 1'


@pytest.mark.parametrize('query', ['page=x', 'per_page=0', 'page=-1'])
def test_journey_paging_must_be_sensible(client, logged_in, device, query):
    response = client.get('/api/1.0/journeys?imei=%s&%s' % (IMEI, query))
    assert response.status_code == 400
    assert response.get_json()['status'] == 'error'


def test_zero_leaves_a_flag_off(client, bearer, device):
    from tracker import db, logs, telemetry

    telemetry.process_lines(device, [record(0, 51.5, -0.1, 0)], '203.0.113.7', db.web, logs.udp)
    url = '/api/1.0/track?imei=%s' % IMEI
    assert client.get(url + '&google=0&return=1', headers=bearer).get_json()['url'].startswith('maps:')
    assert client.get(url + '&google=1&return=1', headers=bearer).get_json()['url'].startswith('https://maps.google.com/')
    assert client.get(url + '&return=0', headers=bearer).status_code == 302
