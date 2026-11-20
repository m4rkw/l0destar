"""Short replies (tracker/reply_policy.py and build_response's gate): the
settings go back in full when they are news, and the reply is just the
interval, the movement alarm and any commands the rest of the time."""

import pytest

from tracker import reply_policy, telemetry

DEVICE = {'id': 1, 'imei': 'x', 'int': 3600, 'movement_alarm': 1, 'backup_int': 900,
          'gnss_backup': 1}
SHORT = '1,3600,1'
FULL = SHORT + ',backup=900,gb=1,gw=off,le=1,tu=-,im=0,gx=off,os=0'


class Clock:
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now


@pytest.fixture
def clock():
    return Clock()


@pytest.fixture
def gate(clock):
    return reply_policy.ReplyGate(refresh_s=60, clock=clock)


# -- the gate ---------------------------------------------------------------------------------

def test_the_first_reply_to_a_device_is_whole(gate):
    assert gate.full(1, ',backup=900,gb=0') is True


def test_the_same_settings_again_are_not(gate):
    gate.full(1, ',backup=900,gb=0')
    assert gate.full(1, ',backup=900,gb=0') is False
    assert gate.full(1, ',backup=900,gb=0') is False


def test_changed_settings_go_out_whole_and_then_the_short_one_resumes(gate):
    gate.full(1, ',backup=900,gb=0')
    assert gate.full(1, ',backup=900,gb=1') is True
    assert gate.full(1, ',backup=900,gb=1') is False


def test_a_unit_that_has_just_booted_gets_them_whatever(gate):
    gate.full(1, ',backup=900')
    assert gate.full(1, ',backup=900', boot=True) is True
    assert gate.full(1, ',backup=900') is False


def test_a_published_firmware_is_news(gate):
    gate.full(1, ',backup=900,gb=0')
    assert gate.full(1, ',backup=900,fota=0.4.129,gb=0') is True


def test_the_settings_are_repeated_once_a_minute_in_case_a_reply_was_lost(gate, clock):
    gate.full(1, ',backup=900')
    clock.now += 59.9
    assert gate.full(1, ',backup=900') is False
    clock.now += 0.2
    assert gate.full(1, ',backup=900') is True
    clock.now += 59.0
    assert gate.full(1, ',backup=900') is False       # counted from the repeat, not the first


def test_repeats_count_from_the_last_whole_reply_not_the_last_short_one(gate, clock):
    answers = [gate.full(1, ',backup=900')]
    for _ in range(5):                      # a reply every 20 s
        clock.now += 20.0
        answers.append(gate.full(1, ',backup=900'))
    # whole at 0, short at 20 and 40, whole again at 60 (a minute since the whole one), short at 80 and 100
    assert answers == [True, False, False, True, False, False]


def test_devices_are_independent(gate):
    gate.full(1, ',backup=900')
    assert gate.full(2, ',backup=900') is True
    assert gate.full(1, ',backup=900') is False


def test_a_fresh_gate_knows_nothing(clock):
    # a server restart: the first reply of each device is whole again
    reply_policy.ReplyGate(clock=clock).full(1, ',backup=900')
    assert reply_policy.ReplyGate(clock=clock).full(1, ',backup=900') is True


# -- build_response ---------------------------------------------------------------------------

class Quiet:
    def info(self, *args):
        pass


class Queue:
    """A command queue, and a record of what else was written."""
    def __init__(self, commands=()):
        self.commands = [{'id': i + 1, 'command': c} for i, c in enumerate(commands)]
        self.writes = []

    def all(self, *args):
        return list(self.commands)

    def query(self, sql, params=None):
        self.writes.append(sql.split()[0] + ' ' + ' '.join(sql.split()[1:3]))


def test_without_a_gate_the_reply_is_whole_as_ever():
    assert telemetry.build_response(DEVICE, Queue(), Quiet()) == FULL


def test_the_gate_is_asked_with_the_settings_part():
    asked = []
    telemetry.build_response(DEVICE, Queue(), Quiet(),
                             gate=lambda tail: asked.append(tail) or True)
    assert asked == [FULL[len(SHORT):]]
    assert asked[0].startswith(',backup=')


def test_a_gate_that_says_no_leaves_the_short_reply():
    assert telemetry.build_response(DEVICE, Queue(), Quiet(), gate=lambda tail: False) == SHORT


def test_a_gate_that_says_yes_leaves_the_whole_one():
    assert telemetry.build_response(DEVICE, Queue(), Quiet(), gate=lambda tail: True) == FULL


def test_commands_ride_the_short_reply_too():
    out = telemetry.build_response(DEVICE, Queue(['locate', 'status']), Quiet(),
                                   gate=lambda tail: False)
    assert out == SHORT + ',locate,status'


def test_commands_come_before_the_settings_when_they_are_sent():
    out = telemetry.build_response(DEVICE, Queue(['locate']), Quiet(), gate=lambda tail: True)
    assert out == SHORT + ',locate' + FULL[len(SHORT):]


def test_the_advert_is_part_of_what_a_change_is():
    tails = []
    for version in (None, '0.4.129'):
        telemetry.build_response(DEVICE, Queue(), Quiet(), firmware_version=version,
                                 gate=lambda tail: tails.append(tail) or False)
    assert tails[0] != tails[1] and ',fota=0.4.129' in tails[1]


def test_the_applied_settings_are_still_filed_on_a_short_reply():
    queue = Queue()
    telemetry.build_response(DEVICE, queue, Quiet(), gate=lambda tail: False)
    assert any(w.startswith('UPDATE `device`') for w in queue.writes)


def test_the_interval_and_movement_alarm_are_always_there():
    device = dict(DEVICE, int=60, movement_alarm=0)
    assert telemetry.build_response(device, Queue(), Quiet(), gate=lambda t: False) == '1,60,0'
