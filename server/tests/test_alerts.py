"""Alert relay: a copy of an alert that was resent is dropped, not notified
twice.  No database: none of these alerts reaches the low-battery rule."""

import pytest

from tracker import telemetry


class _Log:
    def __init__(self):
        self.lines = []

    def info(self, fmt, *args):
        self.lines.append(fmt % args)


DEV = {'id': 1, 'imei': '350000000000001', 'name': 'Car'}


@pytest.fixture
def relayed(monkeypatch):
    sent = []
    monkeypatch.setattr(telemetry.notify, 'device_alert',
                        lambda name, msg, pri: sent.append((msg, pri)))
    telemetry._alert_seen.clear()
    return sent


def alert(line, device=DEV, log=None):
    telemetry._handle_alert(line, device, None, log or _Log())


def test_copy_of_a_relayed_alert_is_dropped(relayed):
    # The first datagram arrived but its reply did not, so it came again.
    log = _Log()
    alert('A,1,backup power: 9.03V,aid=8627', log=log)
    alert('A,1,backup power: 9.03V,aid=8627', log=log)
    assert relayed == [('backup power: 9.03V', 1)]
    assert log.lines[-1] == ('duplicate alert aid=8627 from '
                             '350000000000001 dropped')


def test_id_is_not_part_of_the_message(relayed):
    # Messages carry commas of their own.
    alert('A,0,movement: 38.3deg tilt, 650mg,aid=5')
    alert('A,0,google: 51.388415,-0.211253,aid=6')
    assert relayed == [('movement: 38.3deg tilt, 650mg', 0),
                       ('google: 51.388415,-0.211253', 0)]


def test_alerts_without_an_id_are_all_relayed(relayed):
    # Firmware from before alerts were held: nothing to tell a copy by,
    # and nothing resends them either.
    alert('A,0,movement: 38.3deg tilt, 650mg')
    alert('A,0,movement: 38.3deg tilt, 650mg')
    assert len(relayed) == 2


def test_reused_id_with_another_message_is_relayed(relayed):
    # Two boots can land on the same id; the message tells them apart.
    alert('A,1,backup power: 9.03V,aid=10')
    alert('A,0,car power restored: 11.51V,aid=10')
    assert len(relayed) == 2


def test_same_id_from_another_device_is_relayed(relayed):
    alert('A,1,backup power: 9.03V,aid=10')
    alert('A,1,backup power: 9.03V,aid=10', device={**DEV, 'id': 2})
    assert len(relayed) == 2


def test_forgotten_after_the_retention(relayed):
    assert telemetry._alert_copy(1, 7, 'x', now=0) is False
    assert telemetry._alert_copy(1, 7, 'x', now=1) is True
    later = telemetry._ALERT_SEEN_SECONDS + 2
    assert telemetry._alert_copy(1, 7, 'x', now=later) is False


def test_memory_is_bounded_per_device(relayed):
    for aid in range(telemetry._ALERT_SEEN_MAX + 10):
        telemetry._alert_copy(1, aid, 'x', now=aid)
    assert len(telemetry._alert_seen[1]) == telemetry._ALERT_SEEN_MAX
    # The oldest went first.
    assert telemetry._alert_copy(1, 0, 'x', now=10_000) is False
