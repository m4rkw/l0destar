"""The gw= reply token: the GNSS backup off window as UTC whole hours.

No database needed.  The server's local zone is pinned to Europe/London so
the BST and GMT cases are deterministic.
"""

import datetime
import time

import pytest

from tracker import telemetry

SUMMER = datetime.datetime(2026, 7, 1, 12, 0)    # BST, UTC+1
WINTER = datetime.datetime(2026, 12, 1, 12, 0)   # GMT, UTC+0


@pytest.fixture(autouse=True)
def london(monkeypatch):
    monkeypatch.setenv('TZ', 'Europe/London')
    time.tzset()
    yield
    monkeypatch.undo()
    time.tzset()


def token(window, hour_from, hour_to, now):
    return telemetry.gnss_off_window_token(
        {'gnss_off_window': window, 'gnss_off_hour_from': hour_from,
         'gnss_off_hour_to': hour_to}, now)


@pytest.mark.parametrize('window, hour_from, hour_to, now, expected', [
    (1, 0, 7, SUMMER, '23-6'),          # local 00-07 in BST
    (1, 0, 7, WINTER, '0-7'),           # GMT: unchanged
    (1, 22, 6, SUMMER, '21-5'),         # wraps midnight locally
    (1, 22, 6, WINTER, '22-6'),
    (1, 1, 5, SUMMER, '0-4'),
    (1, 23, 0, SUMMER, '22-23'),
    (0, 0, 7, SUMMER, 'off'),           # switched off
    (1, 5, 5, SUMMER, 'off'),           # from == to
    (1, 0, 0, WINTER, 'off'),
])
def test_token(window, hour_from, hour_to, now, expected):
    assert token(window, hour_from, hour_to, now) == expected


def test_a_device_without_the_columns_is_off():
    assert telemetry.gnss_off_window_token({}) == 'off'


def test_defaults_when_hours_are_null():
    assert token(1, None, None, WINTER) == '0-7'


def test_the_offset_is_taken_at_reply_time(monkeypatch):
    # The same local window is a different UTC span either side of the change.
    assert token(1, 0, 7, datetime.datetime(2026, 3, 28, 12)) == '0-7'
    assert token(1, 0, 7, datetime.datetime(2026, 3, 30, 12)) == '23-6'
