"""A session is only as good as the account behind it.

Skipped unless ``TRACKER_TEST_DB`` is set; see conftest.py.
"""

import time

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


def test_re_enrolment_ends_the_old_passkeys_session(client, logged_in, database, device):
    """Registering again is how a lost phone is recovered, so it has to cut
    the lost phone off."""
    assert client.get('/api/1.0/devices').status_code == 200

    # What registration does: the row is replaced, with the new credential.
    database.query("DELETE FROM `user` WHERE `username` = 'tester'")
    database.query(
        "INSERT INTO `user` (`username`, `user_id`, `credential`) "
        "VALUES ('tester', 'new-credential', '{}')"
    )
    assert refused(client.get('/api/1.0/devices'))


def test_a_session_without_a_credential_is_refused(client, database, device):
    """Sessions issued before they recorded the credential log in again."""
    database.query(
        "INSERT INTO `user` (`username`, `user_id`, `credential`) "
        "VALUES ('tester', 'tester-credential', '{}')"
    )
    with client.session_transaction() as session:
        session['username'] = 'tester'
    assert refused(client.get('/api/1.0/devices'))


def test_a_login_expires_after_its_lifetime(client, logged_in, database, device):
    """However much it is used: the cookie is re-issued on every request, so
    only the recorded login time can end it."""
    from tracker import config

    assert client.get('/api/1.0/devices').status_code == 200
    with client.session_transaction() as session:
        session['login_at'] = int(time.time()) - config.SESSION_LIFETIME_DAYS * 86400 - 1
    assert refused(client.get('/api/1.0/devices'))
