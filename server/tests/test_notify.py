"""Outbound notifications."""

import time

from tracker import notify


def test_every_notification_is_logged(caplog):
    with caplog.at_level('INFO', logger='tracker.notify'):
        notify.send('Car ignition on, battery 12.6V', priority=2)
    assert 'Tracker, priority 2: Car ignition on, battery 12.6V' in caplog.text
