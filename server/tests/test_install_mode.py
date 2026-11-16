"""Installation mode: im= reply token and the ir= / ie= parsing."""
from tracker import telemetry


def test_parse_report_accepts_states():
    assert telemetry.parse_install_report('v12:1;ign:0;k12v:-') == [
        {'name': 'v12', 'ok': True}, {'name': 'ign', 'ok': False},
        {'name': 'k12v', 'ok': None}]


def test_parse_report_refuses_junk():
    for bad in ('', 'v12', 'v12:2', 'V12:1', 'a b:1', 'v12:1;', 'x' * 300):
        assert telemetry.parse_install_report(bad) is None


def test_parse_enable():
    assert telemetry.parse_install_enable('1') == (True, None)
    assert telemetry.parse_install_enable('0') == (False, None)
    assert telemetry.parse_install_enable('0:nofix') == (False, 'nofix')
    assert telemetry.parse_install_enable('2') is None
    assert telemetry.parse_install_enable('0:<script>') is None


def test_install_state_reads_json():
    mode, rep, en = telemetry.install_state(
        {'install_mode': 1, 'install_report': '{"at":"x","checks":[]}',
         'install_enable': 'garbage'})
    assert mode is True and rep == {'at': 'x', 'checks': []} and en is None
    assert telemetry.install_state({}) == (False, None, None)
