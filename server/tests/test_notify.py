"""Outbound notifications."""

import time

from tracker import notify


def test_every_notification_is_logged(caplog):
    with caplog.at_level('INFO', logger='tracker.notify'):
        notify.send('Car ignition on, battery 12.6V', priority=2)
    assert 'Tracker, priority 2: Car ignition on, battery 12.6V' in caplog.text


def test_a_slow_backend_does_not_hold_up_the_caller(monkeypatch):
    delivered = []

    def slow(message, title, priority, url, url_title):
        time.sleep(0.5)
        delivered.append(message)

    monkeypatch.setattr(notify, '_backend', 'slow')
    monkeypatch.setitem(notify._BACKENDS, 'slow', slow)
    started = time.monotonic()
    notify.send('hello')
    assert time.monotonic() - started < 0.2
    notify.wait_idle()
    assert delivered == ['hello']


def test_google_positions_link_to_a_universal_url(monkeypatch):
    sent = {}
    monkeypatch.setattr(notify, 'send', lambda message, **kw: sent.update(kw, message=message))
    notify.device_alert('Car', 'google: 51.500000,-0.100000', 0)
    assert sent['url'] == 'https://www.google.com/maps/search/?api=1&query=51.500000,-0.100000'
    assert sent['message'] == 'Car: google: 51.500000,-0.100000'
