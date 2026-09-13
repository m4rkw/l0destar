"""A session is only as good as the account behind it.

Skipped unless ``TRACKER_TEST_DB`` is set; see conftest.py.
"""

from conftest import needs_db

pytestmark = needs_db


def refused(response):
    return (response.status_code == 302
            and response.headers['Location'].endswith('/login'))


def test_a_locked_account_loses_its_session(client, logged_in, database, device):
    assert client.get('/api/1.0/devices').status_code == 200

    database.query("UPDATE `user` SET `locked` = 1 WHERE `username` = 'tester'")
    assert refused(client.get('/api/1.0/devices'))

    # Cleared rather than refused once: unlocking does not revive it.
    database.query("UPDATE `user` SET `locked` = 0 WHERE `username` = 'tester'")
    assert refused(client.get('/api/1.0/devices'))
    with client.session_transaction() as session:
        assert 'username' not in session


def test_a_removed_account_loses_its_session(client, logged_in, database, device):
    database.query("DELETE FROM `user` WHERE `username` = 'tester'")
    assert refused(client.get('/devices'))
    assert refused(client.get('/'))
