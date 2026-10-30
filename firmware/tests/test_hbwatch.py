"""Host tests for hbwatch.py: finding the stretches of silence the hold breaker
could have caused, and queueing them for review.

The server is never touched: the fetch is a stand-in that returns what the real
one would read from app.log, device.log and the log table.

    uv run --python 3.12 --with pytest pytest -q firmware/tests
"""
import datetime as dt
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import hbwatch as hb  # noqa: E402


@pytest.fixture(autouse=True)
def _breaker_on(monkeypatch):
    """Most of these tests are about a car with the breaker on, where every gap is a suspect;
    the ones for the breaker off (the end of the file) say so themselves."""
    monkeypatch.setattr(hb, "BREAKER_ON", True)


T0 = dt.datetime(2026, 10, 26, 12, 0, 0)
IMEI = hb.IMEI
HEAD = ("id\ttimestamp\tspeed\tcombined_speed\tignition_state\tfw\trec_id\tattach_ms\t"
        "rsrp\tsnr\tband\trat\tcid\tlatitude\tlongitude")


def at(s):
    return T0 + dt.timedelta(seconds=s)


def row(s, mph=20.0, ign=1, fw="0.4.83", rid=None, **kw):
    r = {"id": str(1000 + int(s)), "timestamp": at(s).isoformat(sep=" ", timespec="microseconds"),
         "t": at(s), "mph": mph, "ign": ign, "fw": fw, "rec_id": str(rid if rid is not None else int(s)),
         "attach_ms": "NULL", "rsrp": "NULL", "snr": "NULL", "band": "NULL", "rat": "CATM1",
         "cid": "5425152", "latitude": "50.0", "longitude": "-4.0"}
    r.update(kw)
    return r


def drive(*secs, **kw):
    return [row(s, **kw) for s in secs]


def tsv(rows):
    out = [HEAD]
    for r in rows:
        out.append("\t".join([r["id"], r["timestamp"], "%.2f" % r["mph"], "NULL", str(r["ign"]), r["fw"],
                              r["rec_id"], r["attach_ms"], r["rsrp"], r["snr"], r["band"], r["rat"],
                              r["cid"], r["latitude"], r["longitude"]]))
    return "\n".join(out)


def arrival_line(s, n=3):
    t = at(s)
    return "%s,%03d - INFO - %d records from %s[l0destar] (3.74.85.174)" % (
        t.strftime(hb.TS), t.microsecond // 1000, n, IMEI)


def scenario(arrive, rows, now_s, log=()):
    """What fetch_remote would return at now_s seconds after T0."""
    return {"NOW": at(now_s).strftime(hb.TS), "ARRIVALS": "\n".join(arrival_line(s) for s in arrive),
            "LOG": "\n".join(log), "ROWS": tsv(rows)}


def logline(s, msg, mod="main", up="100.0"):
    return "%s IMEI=%s[l0destar] up=%s W %s: %s" % (at(s).strftime(hb.TS), IMEI, up, mod, msg)


CYCLING = [0, 1, 26, 27, 52, 53]            # a breaker cycling: ~26 s apart
DRIVING = drive(0, 1, 26, 27, 52, 53)       # ...and the car on the move for all of it


# -- the gap rule ------------------------------------------------------------

def test_closed_gap_while_driving_is_found():
    arr = [at(s) for s in CYCLING + [200]]
    (g,) = hb.find_gaps(arr, DRIVING + drive(200), at(400))
    assert (g.start, g.end, g.seconds) == (at(53), at(200), 147)


@pytest.mark.parametrize("why,arrive,rows", [
    ("a slow restore is not a gap", [0, 1, 26, 27, 52, 112], drive(0, 1, 26, 27, 52, 53)),
    ("parked", CYCLING + [200], drive(0, 1, 26, 27, 52, 53, ign=0)),
    ("crawling", CYCLING + [200], drive(0, 1, 26, 27, 52, 53, mph=3.0)),
    ("the build has no breaker", CYCLING + [200], drive(0, 1, 26, 27, 52, 53, fw="0.4.79")),
])
def test_gaps_that_are_not_ours(why, arrive, rows):
    assert hb.find_gaps([at(s) for s in arrive], rows, at(400)) == [], why


def test_a_failed_restore_then_a_car_that_drives_on_is_a_gap_even_if_it_looked_stopped_at_the_start():
    # the last datagram showed walking pace (a queue), the breaker cut, the restore failed, and the
    # car then drove on for over a minute: the rows the unit sent afterwards say so
    rows = drive(0, 1, 26, 27, 52, 53, mph=0.4) + drive(70, 90, 110, 130, mph=25.0)
    arr = [at(s) for s in CYCLING + [4000]]
    log = hb.parse_log(logline(100, "hold break: no registration 30 s after CFUN=21 ??? radio left searching", "modem"))
    assert hb.find_gaps(arr, rows, at(5000)) == []                       # nothing in the log: not ours
    (g,) = hb.find_gaps(arr, rows, at(5000), log)
    assert (g.start, g.end) == (at(53), at(4000))


def test_a_restore_that_fails_as_the_car_pulls_up_to_park_is_not_a_gap():
    # 26 Oct 10:55 on the real car: 17 s more driving after the last datagram, the restore failed,
    # the ignition went off a minute later, the unit searched for ten minutes and slept for the
    # hour.  Nothing was lost and nobody was alerted: a parked car, not a stall.
    rows = drive(0, 1, 26, 27, 52, 53, mph=0.4) + drive(60, 70, mph=8.0) + [row(130, mph=0.0, ign=0)]
    arr = [at(s) for s in CYCLING + [4000]]
    log = hb.parse_log(logline(100, "hold break: no registration 30 s after CFUN=21 ??? radio left searching", "modem"))
    assert hb.find_gaps(arr, rows, at(5000), log) == []


def test_a_failure_line_outside_the_silence_does_not_make_a_gap_of_a_parked_one():
    rows = drive(0, 1, 26, 27, 52, 53, ign=0)
    arr = [at(s) for s in CYCLING + [4000]]
    elsewhere = hb.parse_log(logline(4500, "hold break: no registration 30 s after CFUN=21 ??? x", "modem"))
    assert hb.find_gaps(arr, rows, at(5000), elsewhere) == []


def test_a_failure_line_does_not_flag_a_short_gap_or_one_with_no_rows():
    log = hb.parse_log(logline(60, "hold break: no registration 30 s after CFUN=21 ??? x", "modem"))
    assert hb.find_gaps([at(0), at(40)], drive(0, 1), at(5000), log) == []
    assert hb.find_gaps([at(0), at(500)], [], at(5000), log) == []


def test_a_gap_just_over_the_limit_counts():
    rows = drive(0, 1)
    assert len(hb.find_gaps([at(0), at(hb.GAP_S + 1)], rows, at(400))) == 1
    assert hb.find_gaps([at(0), at(hb.GAP_S)], rows, at(400)) == []


def test_a_gap_waits_for_the_log_lines_that_ride_in_behind_the_data():
    arr = [at(s) for s in CYCLING + [200]]
    rows = DRIVING + drive(200)
    assert hb.find_gaps(arr, rows, at(200 + hb.SETTLE_S - 1)) == []
    assert len(hb.find_gaps(arr, rows, at(200 + hb.SETTLE_S))) == 1


def test_a_gap_that_is_still_open_is_not_one_yet_but_is_noticed():
    arr = [at(s) for s in CYCLING]
    assert hb.find_gaps(arr, DRIVING, at(5000)) == []
    assert hb.open_gap(arr, DRIVING, at(5000)) == 5000 - 53
    assert hb.open_gap(arr, DRIVING, at(60)) is None                     # still within the limit
    assert hb.open_gap(arr, drive(0, 1, 26, 27, 52, 53, ign=0), at(5000)) is None


def test_the_state_is_the_car_at_the_start_not_the_rows_parked_inside_the_gap():
    # a row stamped inside the gap (the unit back-dated it) says parked, and has
    # the highest id: it must not hide that the car was moving when the silence began
    rows = drive(95, 100) + [row(150, mph=0, ign=0, id="9999")]
    (g,) = hb.find_gaps([at(100), at(300)], rows, at(600))
    assert g.last[-1]["t"] == at(100)


# -- reading what the server gives -------------------------------------------

def test_arrivals_are_records_duplicates_and_alerts_only():
    text = "\n".join([
        arrival_line(10),
        "2026-10-26 12:00:20,500 - INFO - duplicate rid=77 from %s[l0destar] dropped" % IMEI,
        "2026-10-26 12:00:30,100 - INFO - alert from %s[l0destar] (pri=1): tilt" % IMEI,
        "2026-10-26 12:00:40,000 - INFO - fw http: GET /fw/manifest-%s.txt (12 bytes) from 1.2.3.4" % IMEI,
        "2026-10-26 12:00:50,000 - ERROR - unexpected error IMEI=%s[l0destar]" % IMEI,
    ])
    assert hb.parse_arrivals(text) == [at(10), at(20.5), at(30.1)]


def test_rows_with_no_speed_are_zero_and_the_faster_of_gnss_and_obd_wins():
    text = HEAD + "\n" + "\t".join(["1", "2026-10-26 12:00:00.5", "NULL", "12.5", "1", "0.4.83", "5", "NULL",
                                    "NULL", "NULL", "NULL", "CATM1", "7", "50", "-4"])
    (r,) = hb.parse_rows(text)
    assert (r["mph"], r["ign"]) == (12.5, 1)


def test_the_reply_must_be_whole_and_the_database_must_have_answered():
    with pytest.raises(hb.FetchError, match="cut short"):
        hb.split_sections("##NOW\n2026-10-26 12:00:00\n##ARRIVALS\n")
    with pytest.raises(hb.FetchError, match="database"):
        hb.split_sections("##NOW\nx\n##ROWS\nERROR 2002 (HY000): Can't connect\n##DBFAIL\n##END\n")
    assert hb.split_sections("##NOW\nx\n##ROWS\n##END\n")["ROWS"] == ""


# -- what the log says -------------------------------------------------------

LOG = [
    logline(50, "hold break: connected 5 s past our last send at 29 km/h on cell 5425152 tac 26731 ??? "
                "taking LTE off (27 cycles, 0 failed, re-attach 2971 ms me"),
    logline(133, "hold break: no registration 30 s after CFUN=21 ??? radio left searching", "modem"),
    logline(133, "hold break: LTE off 21 s, not registered 30 s after turning it on (-116) ??? the modem "
                 "keeps looking, hold breaking off until it registers"),
    logline(140, '''hold break: 5 s without a registration: CFUN 1 | CEREG 5,2,"686B","004FBF01",7 | '''
                 '''CSCON 1,1 | CESQ 99,99,255,255,0,31''', "modem"),
    logline(170, '''hold break: 15 s without a registration: CFUN 1 | CEREG 5,2,"686B","004FBF01",7 | '''
                 '''CSCON 1,0 | CESQ 99,99,255,255,255,255''', "modem"),
    logline(180, "Registration rejected, EMM cause: 11, Cell ID: 1, Tracking area: 2", "lte_lc"),
    logline(190, "hold break: registered again 57 s after the restore gave up ??? cutting resumes in 5 min"),
    logline(20, "pre shared key psk=SECRET"),
]


def test_facts_from_the_lines_the_unit_really_writes():
    gap = hb.Gap(at(53), at(200), drive(53))
    f = hb.facts(gap, hb.parse_log("\n".join(LOG)))
    assert f["cut"] == (at(50), [5, 29, 27, 0, 2971])
    assert f["failed"] == {at(133)}
    assert [(d[1], d[2], d[3]) for d in f["diag"]] == [(5, 2, True), (15, 2, False)]
    assert f["reject"] == [(at(180), 11)]
    assert f["again"] == [(at(190), 57)]


def test_a_line_that_mentions_the_psk_never_gets_into_a_bundle():
    log = hb.parse_log("\n".join(LOG))
    assert all("psk" not in l["raw"].lower() for l in log)
    gap = hb.Gap(at(53), at(200), drive(53))
    text = hb.bundle("x-gap", [gap], DRIVING, [at(s) for s in CYCLING + [200]], log, at(400))
    assert "SECRET" not in text and "pre shared key" not in text


def test_the_bundle_states_what_a_reviewer_needs():
    log = hb.parse_log("\n".join(LOG))
    gap = hb.Gap(at(53), at(200), drive(53, mph=28))
    text = hb.bundle("20261026-120053-gap", [gap], DRIVING, [at(s) for s in CYCLING + [200]], log, at(400))
    assert "### Gap 1: 10-26 12:00:53 -> 12:03:20, 147 s" in text
    assert "restore failed inside the gap: 12:02:13" in text
    assert "a cell heard in 1" in text
    assert "cause 11" in text
    assert "--done 20261026-120053-gap" in text and "PushNotification" in text


# -- the pass, as cron runs it -----------------------------------------------

def fetcher(*scenarios):
    it = iter(scenarios)
    return lambda now: next(it)


def test_first_pass_only_records_then_a_new_gap_is_queued_once(tmp_path):
    d = str(tmp_path)
    old = scenario(CYCLING + [200], DRIVING + drive(200), 400)
    r = hb.run_once(d, fetcher(old), clock=lambda: 1000.0)
    assert (r["baseline"], r["new"], r["incident"]) == (1, [], None)
    assert hb.pending(d) == []

    again = scenario(CYCLING + [200], DRIVING + drive(200), 700)
    assert hb.run_once(d, fetcher(again), clock=lambda: 1300.0)["incident"] is None

    worse = scenario(CYCLING + [200, 226, 227, 700], DRIVING + drive(200, 226, 227, 700), 900)
    r = hb.run_once(d, fetcher(worse), clock=lambda: 1600.0)
    assert r["incident"] == "20261026-120347-gap"
    (p,) = hb.pending(d)
    assert os.path.exists(p[:-len(".pending")] + ".md")
    assert "Gap 1: 10-26 12:03:47 -> 12:11:40" in open(p[:-len(".pending")] + ".md").read()

    assert hb.run_once(d, fetcher(worse), clock=lambda: 1900.0)["incident"] is None
    assert len(hb.pending(d)) == 1


def test_gaps_found_inside_the_cooldown_wait_for_the_next_review(tmp_path):
    d = str(tmp_path)
    clock = [1000.0]
    hb.run_once(d, fetcher(scenario(CYCLING, DRIVING, 100)), clock=lambda: clock[0])      # baseline
    a = scenario(CYCLING + [200], DRIVING + drive(200), 400)
    assert hb.run_once(d, fetcher(a), clock=lambda: clock[0])["incident"]
    b = scenario(CYCLING + [200, 226, 227, 700], DRIVING + drive(200, 226, 227, 700), 900)
    clock[0] += 300
    r = hb.run_once(d, fetcher(b), clock=lambda: clock[0])
    assert r["incident"] is None and len(r["new"]) == 1                                    # held back
    clock[0] += hb.COOLDOWN_S
    r = hb.run_once(d, fetcher(b), clock=lambda: clock[0])
    assert r["incident"] and len(hb.pending(d)) == 2


def test_a_failed_read_is_counted_and_the_next_good_one_clears_it(tmp_path):
    d = str(tmp_path)

    def broken(now):
        raise hb.FetchError("ssh t exit 255: no route")

    with pytest.raises(hb.FetchError):
        hb.run_once(d, broken, clock=lambda: 5.0)
    st = hb.load_state(d)
    assert st["fails"] == 1 and "no route" in st["last_error"]
    hb.run_once(d, fetcher(scenario(CYCLING, DRIVING, 100)), clock=lambda: 6.0)
    st = hb.load_state(d)
    assert (st["fails"], st["last_error"], st["last_ok_epoch"]) == (0, "", 6.0)


def test_a_reply_missing_a_section_is_a_failed_read_not_a_crash(tmp_path):
    with pytest.raises(hb.FetchError, match="unreadable"):
        hb.run_once(str(tmp_path), lambda now: {"NOW": at(0).strftime(hb.TS)}, clock=lambda: 1.0)


def test_dry_run_changes_nothing(tmp_path):
    d = str(tmp_path)
    r = hb.run_once(d, fetcher(scenario(CYCLING + [200], DRIVING + drive(200), 400)), dry_run=True)
    assert len(r["new"]) == 1 and "Gap 1" in r["bundle"]
    assert hb.load_state(d) == {} and hb.pending(d) == []


# -- the waiter and the close ------------------------------------------------

def test_wait_returns_as_soon_as_a_review_is_queued(tmp_path, capsys):
    d = str(tmp_path)
    open(os.path.join(hb.paths(d)[1], "20261026-120053-gap.pending"), "w").close()
    assert hb.wait(d, sleep=lambda s: pytest.fail("slept")) == 0
    assert "20261026-120053-gap.md" in capsys.readouterr().out


def test_wait_reports_a_dead_check_once_and_forgives_a_sleeping_mac(tmp_path, capsys):
    d = str(tmp_path)
    hb.save_state(d, {"last_ok_epoch": 1000.0, "last_error": "ssh t timed out"})
    now = [1000.0 + hb.STALE_S + 1]

    def sleep(s):
        now[0] += s

    # the Mac has just woken: nothing is reported until the grace has run out
    assert hb.wait(d, poll=20, sleep=sleep, clock=lambda: now[0]) == 2
    assert now[0] >= 1000.0 + hb.STALE_S + 600
    out = capsys.readouterr().out
    assert "HB-WATCH-STALE" in out and "ssh t timed out" in out

    # the same stall is not reported again; a new review still gets through
    calls = []

    def sleep2(s):
        calls.append(s)
        if len(calls) == 3:
            open(os.path.join(hb.paths(d)[1], "x-gap.pending"), "w").close()
        now[0] += s

    assert hb.wait(d, poll=20, sleep=sleep2, clock=lambda: now[0]) == 0
    assert len(calls) == 3


def test_done_closes_a_review_and_status_shows_the_verdict(tmp_path, capsys):
    d = str(tmp_path)
    marker = os.path.join(hb.paths(d)[1], "x-gap.pending")
    open(marker, "w").close()
    hb.done(d, "x-gap", "NOT-BREAKER: coverage")
    assert hb.pending(d) == [] and not os.path.exists(marker)
    hb.status(d)
    assert "closed x-gap" in capsys.readouterr().out and "NOT-BREAKER" in open(
        os.path.join(hb.paths(d)[1], "x-gap.done")).read()
    with pytest.raises(SystemExit):
        hb.done(d, "x-gap", "again")


# -- the network's side (nce.py) in the bundle -------------------------------

import nce  # noqa: E402


def net_events(*raws):
    return {"events": sorted((nce.flat(r) for r in raws), key=lambda e: e["t"]), "note": None}


def nce_raw(i, ts, tid, eci=5425152):
    created = ts
    e = {"id": i, "timestamp": ts, "alert": False, "event_type": {"id": tid, "description": "x"},
         "event_severity": {"id": 0, "description": "INFO"}, "event_source": {"id": 0, "description": "Network"},
         "description": "d", "detail": {"name": "EE", "pdp_context": {
             "rat_type": 6, "tunnel_created": created, "gtp_v2_uli": {"eci": eci, "tac": 26731}}}}
    return e


def test_the_bundle_says_when_the_network_never_saw_an_attach():
    gap = hb.Gap(at(53), at(200), drive(53))                       # 12:00:53 -> 12:03:20, GMT = UTC
    net = net_events(nce_raw(1, "2026-10-26T12:00:40.000Z", 4), nce_raw(2, "2026-10-26T12:00:53.800Z", 5),
                     nce_raw(3, "2026-10-26T12:03:14.900Z", 3))
    text = hb.bundle("x-gap", [gap], DRIVING, [at(s) for s in CYCLING + [200]], [], at(400), net)
    assert "- 1NCE, last event before the silence: Update PDP 12:00:40" in text
    assert "- 1NCE, detached at 12:00:53 (0 s after the last datagram); no attach attempt reached the network " \
           "until Create PDP 12:03:14" in text
    assert "- 1NCE, the tracker's first datagram came 5 s after that attach" in text
    assert "## 1NCE network events around each gap" in text


def test_the_bundle_reads_an_open_session_as_a_stall_above_the_network():
    gap = hb.Gap(at(53), at(200), drive(53))
    net = net_events(nce_raw(1, "2026-10-26T11:58:00.000Z", 4), nce_raw(2, "2026-10-26T13:00:00.000Z", 5))
    text = hb.bundle("x-gap", [gap], DRIVING, [at(s) for s in CYCLING + [200]], [], at(400), net)
    assert "a PDP session was open throughout" in text


def test_a_summer_gap_lines_up_with_the_networks_utc_events():
    # 13:00:53 BST is 12:00:53 UTC: the Delete at 12:00:53Z is the cut, not an hour-old event
    t0 = dt.datetime(2026, 7, 1, 13, 0, 0)
    gap = hb.Gap(t0 + dt.timedelta(seconds=53), t0 + dt.timedelta(seconds=200), [dict(row(53), t=t0)])
    net = net_events(nce_raw(1, "2026-07-01T12:00:40.000Z", 4), nce_raw(2, "2026-07-01T12:00:53.800Z", 5),
                     nce_raw(3, "2026-07-01T12:03:14.900Z", 3))
    lines = hb.net_facts(gap, net)
    assert "Update PDP 13:00:40" in lines[0]
    assert "detached at 13:00:53" in lines[1] and "Create PDP 13:03:14" in lines[1]


def test_an_unavailable_network_side_is_a_line_not_a_failure():
    gap = hb.Gap(at(53), at(200), drive(53))
    net = {"events": [], "note": "NceError: GET /v2/sims: HTTP 500 Internal Server Error"}
    text = hb.bundle("x-gap", [gap], DRIVING, [at(s) for s in CYCLING + [200]], [], at(400), net)
    assert "1NCE (network side): not available (NceError: GET /v2/sims: HTTP 500" in text
    assert "## 1NCE network events" not in text


def test_without_a_provider_the_bundle_is_as_before():
    gap = hb.Gap(at(53), at(200), drive(53))
    text = hb.bundle("x-gap", [gap], DRIVING, [at(s) for s in CYCLING + [200]], [], at(400))
    assert "- 1NCE" not in text and "## 1NCE network events" not in text


def test_the_provider_turns_every_failure_into_a_note(monkeypatch):
    def boom(*a, **k):
        raise RuntimeError("disk on fire")
    monkeypatch.setattr(hb, "refresh_net", boom)
    r = hb.net_provider([], at(0))
    assert r["events"] == [] and "disk on fire" in r["note"]


def test_a_fresh_archive_is_not_synced_again(monkeypatch):
    monkeypatch.setattr(nce, "archive_age", lambda home=None: 30.0)
    monkeypatch.setattr(hb.subprocess, "run", lambda *a, **k: pytest.fail("ran a sync"))
    assert hb.refresh_net(120) is None


def test_a_stale_archive_is_synced_by_the_interpreter_that_has_requests(monkeypatch):
    seen = {}

    class Done:
        returncode, stderr = 0, ""

    def run(cmd, **kw):
        seen["cmd"] = cmd
        return Done()
    monkeypatch.setattr(nce, "archive_age", lambda home=None: 9999.0)
    monkeypatch.setattr(hb.os.path, "exists", lambda p: True)
    monkeypatch.setattr(hb.subprocess, "run", run)
    assert hb.refresh_net(120) is None
    assert seen["cmd"][0] == hb.NCE_PYTHON and seen["cmd"][1].endswith("nce.py") and seen["cmd"][2] == "sync"


def test_a_failed_sync_is_the_notes_last_stderr_line_and_a_missing_venv_is_named(monkeypatch):
    class Failed:
        returncode, stderr = 1, "Traceback...\nnce: GET /v2/sims: HTTP 500 Internal Server Error\n"
    monkeypatch.setattr(nce, "archive_age", lambda home=None: 9999.0)
    monkeypatch.setattr(hb.os.path, "exists", lambda p: True)
    monkeypatch.setattr(hb.subprocess, "run", lambda *a, **k: Failed())
    assert hb.refresh_net(120) == "nce: GET /v2/sims: HTTP 500 Internal Server Error"
    monkeypatch.setattr(hb.os.path, "exists", lambda p: False)
    assert "venv with requests" in hb.refresh_net(120)


def test_the_pass_hands_the_gaps_to_the_provider_and_puts_its_answer_in_the_bundle(tmp_path):
    d = str(tmp_path)
    hb.run_once(d, fetcher(scenario(CYCLING, DRIVING, 100)), clock=lambda: 1000.0)               # baseline
    seen = []

    def net(gaps, now):
        seen.append(len(gaps))
        return net_events(nce_raw(1, "2026-10-26T12:00:53.800Z", 5))
    hb.run_once(d, fetcher(scenario(CYCLING + [200], DRIVING + drive(200), 400)), clock=lambda: 2000.0, net=net)
    assert seen == [1]
    (p,) = hb.pending(d)
    assert "1NCE, last event before the silence" in open(p[:-len(".pending")] + ".md").read()


# -- saying whether the car parked -------------------------------------------

def test_the_bundle_says_when_the_car_parked_inside_the_silence_and_when_its_records_got_through():
    # 25 Oct 14:52 on the real car: 17 s more driving, parked 127 s after the last datagram, and
    # the records reached the server 41 min after that
    rows = drive(0, 1, 26, 27, 52, 53, mph=28.0) + drive(60, 70, mph=20.0) + [row(176, mph=0.5), row(180, mph=0.0, ign=0)]
    gap = hb.Gap(at(53), at(2640), drive(53, mph=28.0))
    text = hb.bundle("x-gap", [gap], rows, [at(s) for s in CYCLING + [2640]], [], at(3000))
    assert "- car: kept moving 17 s into the silence, parked (ignition off) at 12:03:00, 127 s after the last datagram" in text
    assert "a unit asleep, not an outage" in text and "reached the server at 12:44:00, 41 min after it parked" in text


def test_the_bundle_says_when_there_is_no_ignition_off_inside_the_silence():
    rows = drive(0, 1, 26, 27, 52, 53, mph=28.0) + drive(60, 90, 120, mph=25.0)
    gap = hb.Gap(at(53), at(400), drive(53, mph=28.0))
    text = hb.bundle("x-gap", [gap], rows, [at(s) for s in CYCLING + [400]], [], at(800))
    assert "- car: kept moving 67 s into the silence; no ignition-off record inside it" in text


def test_the_playbook_tells_the_reviewer_not_to_call_a_parked_unit_dark():
    assert "once an ignition-off record is" in hb.PLAYBOOK and "parked unit sleeping" in hb.PLAYBOOK


def test_a_car_that_parked_after_the_silence_ended_was_not_parked_inside_it():
    # 25 Oct 11:59: the datagram after the 63 s gap came while the car was still driving; it parked 2 min later
    rows = drive(0, 1, 26, 27, 52, 53, mph=17.0) + drive(60, 62, 64, mph=17.0) + [row(190, mph=0.0, ign=0)]
    gap = hb.Gap(at(53), at(116), drive(53, mph=17.0))
    text = hb.bundle("x-gap", [gap], rows, [at(s) for s in CYCLING + [116]], [], at(400))
    assert "- car: kept moving 11 s into the silence; no ignition-off record inside it" in text
    assert "after it parked" not in text


def test_a_silence_with_no_surviving_records_says_so_instead_of_claiming_the_car_stood_still():
    # 26 Oct 13:10: the unit was reset by its watchdog mid-drive; nothing it recorded in the silence exists
    rows = drive(0, 1, 26, 27, 52, 53, mph=11.0) + drive(300, 301, mph=20.0)
    gap = hb.Gap(at(53), at(200), drive(53, mph=11.0))
    gap.start, gap.end = at(54), at(200)                         # no row is stamped inside (54, 200)
    text = hb.bundle("x-gap", [gap], rows, [at(s) for s in CYCLING + [200]], [], at(400))
    assert "- car: no record from inside the silence survives (lost with a reset, or never made)" in text
    assert "kept moving 0 s" not in text


# -- the breaker's note after a reset (firmware 0.4.84: rst=wdt+hb:Q98/3) -------

def test_the_bundle_decodes_the_breakers_note_from_the_reset_record():
    rows = drive(0, 1, 26, 27, 52, 53, mph=11.0) + [row(300, mph=20.0, rst="wdt+hb:Q98/3+stall:4aa02/3e254")]
    gap = hb.Gap(at(53), at(250), drive(53, mph=11.0))
    text = hb.bundle("x-gap", [gap], rows, [at(s) for s in CYCLING + [250]], [], at(900))
    assert "- reset: rst=wdt+hb:Q98/3+stall:4aa02/3e254 on the record at 12:05:00; the breaker was in phase Q " \
           "(a modem query: AT commands with no timeout of their own), last alive 98 s after the cut and 3 s " \
           "into the phase" in text


def test_a_reset_with_no_note_says_so_and_an_unknown_phase_is_not_hidden():
    gap = hb.Gap(at(53), at(250), drive(53, mph=11.0))
    arr = [at(s) for s in CYCLING + [250]]
    plain = hb.bundle("x-gap", [gap], drive(0, 53, mph=11.0) + [row(300, rst="wdt")], arr, [], at(900))
    assert "- reset: rst=wdt on the record at 12:05:00; no breaker note" in plain
    odd = hb.bundle("x-gap", [gap], drive(0, 53, mph=11.0) + [row(300, rst="wdt+hb:Z5/5")], arr, [], at(900))
    assert "phase Z (unknown phase)" in odd


def test_no_reset_record_no_reset_line():
    gap = hb.Gap(at(53), at(250), drive(53, mph=11.0))
    arr = [at(s) for s in CYCLING + [250]]
    assert "\n- reset: rst=" not in hb.bundle("x-gap", [gap], drive(0, 53, 300, mph=11.0), arr, [], at(900))
    # a reset long after the silence is another event
    assert "\n- reset: rst=" not in hb.bundle("x-gap", [gap], drive(0, 53, mph=11.0) + [row(900, rst="wdt")], arr, [], at(2000))


# -- a firmware update in the silence ----------------------------------------

def alert_line(s, text):
    return "%s,100 - INFO - alert from %s[l0destar] (pri=0): %s" % (at(s).strftime(hb.TS), IMEI, text)


def test_alerts_are_read_from_the_servers_log_and_others_are_ignored():
    text = "\n".join([alert_line(10, "fota: 0.4.83 -> 0.4.84 available, downloading"),
                      arrival_line(20), "x"])
    assert hb.parse_alerts(text) == [(at(10), "fota: 0.4.83 -> 0.4.84 available, downloading")]


def test_the_bundle_says_an_update_ran_in_the_silence_and_what_its_reboot_cost():
    gap = hb.Gap(at(53), at(200), drive(53, mph=30.0))
    alerts = hb.parse_alerts("\n".join([alert_line(190, "fota: 0.4.83 -> 0.4.84 available, downloading"),
                                        alert_line(280, "fota: 0.4.83 -> 0.4.84, rebooting")]))
    text = hb.bundle("x-gap", [gap], DRIVING, [at(s) for s in CYCLING + [200]], [], at(900), None, alerts)
    assert "- firmware update: fota: 0.4.83 -> 0.4.84 available, downloading at 12:03:10; " \
           "fota: 0.4.83 -> 0.4.84, rebooting at 12:04:40; the reboot discarded whatever the unit still held" in text


def test_no_update_no_line_and_an_old_update_is_not_this_gap_s():
    gap = hb.Gap(at(53), at(200), drive(53, mph=30.0))
    arr = [at(s) for s in CYCLING + [200]]
    assert "\n- firmware update: fota:" not in hb.bundle("x-gap", [gap], DRIVING, arr, [], at(900))
    old = hb.parse_alerts(alert_line(2000, "fota: 0.4.83 -> 0.4.84, rebooting"))
    assert "\n- firmware update: fota:" not in hb.bundle("x-gap", [gap], DRIVING, arr, [], at(3000), None, old)
    before = hb.parse_alerts(alert_line(10, "fota: 0.4.83 -> 0.4.84, rebooting"))
    assert "\n- firmware update: fota:" not in hb.bundle("x-gap", [gap], DRIVING, arr, [], at(900), None, before)


# -- a restart the log does not show: the boot's update check ------------------

def fetch_line(s):
    return "%s,600 - INFO - fw http: GET /fw/manifest-%s.txt (76 bytes) from 3.127.42.194" % (at(s).strftime(hb.TS), IMEI)


def boot_bundle(fetch_at, attach="2026-10-26T12:05:00.000Z", between=(), net=True, gap_end=400):
    gap = hb.Gap(at(53), at(gap_end), drive(53, mph=30.0))
    n = net_events(nce_raw(1, attach, 3)) if net else None
    arr = [at(s) for s in CYCLING + list(between) + [gap_end]]
    return hb.bundle("x-gap", [gap], DRIVING, arr, [], at(900), n, (), hb.parse_fetches(fetch_line(fetch_at)))


def test_manifest_fetches_are_read_from_the_servers_log():
    assert hb.parse_fetches("\n".join([fetch_line(10), arrival_line(20), "x"])) == [at(10.6)]


def test_a_manifest_fetch_soon_after_an_attach_with_no_telemetry_is_a_probable_reboot():
    text = boot_bundle(308)
    assert "- probable reboot: a manifest fetch at 12:05:08 came 8 s after the attach at 12:05:00 with no telemetry " \
           "between them" in text
    assert "the unit most likely restarted at about 12:04:57" in text


def test_telemetry_between_the_attach_and_the_fetch_means_it_was_not_a_boot():
    assert "\n- probable reboot: a manifest fetch" not in boot_bundle(308, between=(303,))


def test_a_fetch_far_from_an_attach_or_outside_the_gap_or_without_the_network_view_is_not_one():
    assert "\n- probable reboot: a manifest fetch" not in boot_bundle(330)                      # 30 s after the attach
    assert "\n- probable reboot: a manifest fetch" not in boot_bundle(303)                      # 3 s: too quick for a boot
    assert "\n- probable reboot: a manifest fetch" not in boot_bundle(308, gap_end=200)         # the gap closed long before
    assert "\n- probable reboot: a manifest fetch" not in boot_bundle(308, net=False)           # no 1NCE events to date the attach


def test_a_silence_that_starts_with_a_datagram_carrying_no_recent_rows_is_not_a_gap_of_a_moving_car():
    # 26 Oct 14:37:45: the datagram before the 90 s of firmware download was the update's own alert and
    # carried no telemetry, so the nearest rows were five minutes old and still said 33 mph
    rows = drive(0, 1, 26, 27, 52, 53, mph=33.0)
    arr = [at(s) for s in CYCLING + [353, 443]]
    flagged = hb.find_gaps(arr, rows, at(900))
    assert [(g.start, g.end) for g in flagged] == [(at(53), at(353))]        # the first silence is real
    # the second starts at 353, whose nearest rows are 300 s old: unknown, so not flagged
    assert all(g.start != at(353) for g in flagged)


# -- with the breaker off (the car from 0.4.86) -------------------------------

@pytest.fixture
def breaker_off(monkeypatch):
    monkeypatch.setattr(hb, "BREAKER_ON", False)


OPEN = net_events(nce_raw(1, "2026-10-26T11:58:00.000Z", 4))         # a session open, nothing deleted


def provider(net):
    return lambda gaps, now: net


def baseline(d):
    hb.run_once(d, fetcher(scenario(CYCLING, DRIVING, 100)), clock=lambda: 1000.0)


HOLD = (CYCLING + [200], DRIVING + drive(200))                       # 12:00:53 -> 12:03:20, 147 s
LONG = (CYCLING + [200, 226, 227, 700], DRIVING + drive(200, 226, 227, 700))   # ...and 227 -> 700, 473 s


def test_an_ordinary_hold_is_not_reviewed_and_is_set_aside_once_the_log_has_caught_up(breaker_off, tmp_path, capsys):
    d = str(tmp_path)
    baseline(d)
    r = hb.run_once(d, fetcher(scenario(*HOLD, 400)), clock=lambda: 2000.0, net=provider(OPEN))
    assert (r["new"], r["incident"], r["quiet"]) == ([], None, 0)
    assert hb.pending(d) == [] and "quiet" not in hb.load_state(d)["reported"].values()   # looked at again

    r = hb.run_once(d, fetcher(scenario(*HOLD, 1200)), clock=lambda: 3000.0, net=provider(OPEN))
    assert (r["new"], r["incident"], r["quiet"]) == ([], None, 1)
    st = hb.load_state(d)
    assert "quiet" in st["reported"].values()
    assert list(st["quiet"].values()) == [147]

    hb.status(d)
    assert "1 silence(s) over 60 s set aside as ordinary holds in the last 24 h, the longest 147 s" in capsys.readouterr().out
    r = hb.run_once(d, fetcher(scenario(*HOLD, 1500)), clock=lambda: 3300.0, net=provider(OPEN))
    assert r["quiet"] == 0                                                                  # counted once


def test_a_silence_the_car_drove_through_for_five_minutes_is_reviewed_even_with_the_breaker_off(breaker_off, tmp_path):
    d = str(tmp_path)
    baseline(d)
    r = hb.run_once(d, fetcher(scenario(*LONG, 900)), clock=lambda: 2000.0, net=provider(OPEN))
    assert r["incident"] == "20261026-120347-gap" and len(r["new"]) == 1                    # not the 147 s one
    (p,) = hb.pending(d)
    text = open(p[:-len(".pending")] + ".md").read()
    assert "- queued because: silent 7.9 min, and the car moved for 7.9 min of it" in text
    assert "Gap 1: 10-26 12:03:47 -> 12:11:40" in text and "12:00:53" not in text.split("## device.log")[0]


def test_a_deleted_session_that_no_attach_follows_for_two_minutes_is_reviewed(breaker_off, tmp_path):
    d = str(tmp_path)
    baseline(d)
    net = net_events(nce_raw(1, "2026-10-26T12:00:40.000Z", 4), nce_raw(2, "2026-10-26T12:00:56.000Z", 5),
                     nce_raw(3, "2026-10-26T12:03:20.000Z", 3))
    r = hb.run_once(d, fetcher(scenario(*HOLD, 400)), clock=lambda: 2000.0, net=provider(net))
    assert r["incident"] == "20261026-120053-gap"
    (p,) = hb.pending(d)
    assert "- queued because: the network deleted the session and saw no attach for 144 s" in open(
        p[:-len(".pending")] + ".md").read()


def test_a_deleted_session_that_is_back_within_two_minutes_is_an_ordinary_hold(breaker_off, tmp_path):
    d = str(tmp_path)
    baseline(d)
    net = net_events(nce_raw(2, "2026-10-26T12:00:56.000Z", 5), nce_raw(3, "2026-10-26T12:01:20.000Z", 3))
    r = hb.run_once(d, fetcher(scenario(*HOLD, 400)), clock=lambda: 2000.0, net=provider(net))
    assert (r["new"], r["incident"]) == ([], None)


def test_no_attach_in_the_log_is_not_read_until_the_api_has_had_time_to_catch_up(breaker_off, tmp_path):
    d = str(tmp_path)
    baseline(d)
    net = net_events(nce_raw(2, "2026-10-26T12:00:56.000Z", 5))                             # deleted, nothing since
    assert hb.run_once(d, fetcher(scenario(*HOLD, 400)), clock=lambda: 2000.0, net=provider(net))["incident"] is None
    r = hb.run_once(d, fetcher(scenario(*HOLD, 1200)), clock=lambda: 3000.0, net=provider(net))
    (p,) = hb.pending(d)
    assert r["incident"] and "has logged no attach since" in open(p[:-len(".pending")] + ".md").read()


def test_without_the_network_log_only_the_long_silence_is_reviewed(breaker_off, tmp_path):
    d = str(tmp_path)
    baseline(d)
    gone = {"events": [], "note": "NceError: HTTP 500"}
    assert hb.run_once(d, fetcher(scenario(*HOLD, 400)), clock=lambda: 2000.0, net=provider(gone))["incident"] is None
    assert hb.run_once(d, fetcher(scenario(*LONG, 900)), clock=lambda: 2300.0, net=provider(gone))["incident"]
    assert hb.load_state(d)["budget"]["note"] == "NceError: HTTP 500"


def test_with_no_provider_at_all_nothing_but_the_long_silence_gets_through(breaker_off, tmp_path):
    d = str(tmp_path)
    baseline(d)
    assert hb.run_once(d, fetcher(scenario(*HOLD, 400)), clock=lambda: 2000.0)["incident"] is None


def test_a_parked_car_s_silence_is_not_a_long_one(breaker_off, tmp_path):
    # 11 minutes of quiet, but the car stopped 2 minutes in (the unit sleeps): not five minutes of driving
    d = str(tmp_path)
    baseline(d)
    rows = DRIVING + drive(100, mph=0.0, ign=1) + [row(120, mph=0.0, ign=0), row(660, mph=0.0, ign=0)]
    r = hb.run_once(d, fetcher(scenario(CYCLING + [660], rows, 900)), clock=lambda: 2000.0, net=provider(OPEN))
    assert (r["new"], r["incident"]) == ([], None)


def test_with_the_breaker_on_every_gap_is_still_a_suspect_and_says_why(tmp_path, monkeypatch):
    monkeypatch.setattr(hb, "BREAKER_ON", True)
    d = str(tmp_path)
    baseline(d)
    r = hb.run_once(d, fetcher(scenario(*HOLD, 400)), clock=lambda: 2000.0, net=provider(OPEN))
    (p,) = hb.pending(d)
    assert r["incident"] and "- queued because: the breaker is on, so every silence over 60 s" in open(
        p[:-len(".pending")] + ".md").read()


# -- the SIM's allowance -----------------------------------------------------

def update_locs(first_s, step_s, n):
    return [nce_raw(100 + i, (T0 + dt.timedelta(seconds=first_s + i * step_s)).strftime("%Y-%m-%dT%H:%M:%S.000Z"), 1)
            for i in range(n)]


def test_the_count_is_update_locations_in_the_last_15_and_45_minutes():
    ev = net_events(*update_locs(0, 30, 10), nce_raw(50, "2026-10-26T12:04:00.000Z", 3),
                    nce_raw(51, "2026-10-26T12:04:30.000Z", 2),
                    nce_raw(52, "2026-10-26T11:10:00.000Z", 1))                             # too old
    # ten 30 s apart from 12:00:00, a 2G/3G one at 12:04:30 (counted), a Create PDP (not), one an hour old (not)
    assert hb.budget_counts(ev["events"], at(600)) == (11, 11)
    assert hb.budget_counts(ev["events"], at(2400)) == (0, 11)         # 12:40: nothing in the last 15 min
    assert hb.budget_counts(ev["events"], at(901)) == (10, 11)         # 12:15:01: the 12:00:00 one has just left the 15


def test_over_fifteen_in_fifteen_minutes_queues_a_review_once_per_cooldown(breaker_off, tmp_path):
    d = str(tmp_path)
    baseline(d)
    over = net_events(*update_locs(10, 50, 16))                                              # 12:00:10 ... 12:13:40
    clock = [2000.0]
    r = hb.run_once(d, fetcher(scenario(CYCLING, DRIVING, 900)), clock=lambda: clock[0], net=provider(over))
    assert r["budget"] == (16, 16) and r["budget_incident"] == "20261026-121500-budget"
    (p,) = hb.pending(d)
    text = open(p[:-len(".pending")] + ".md").read()
    assert "Update location events: 16 in the last 15 min (reviewed above 15), 16 in the last 45 min" in text
    assert "## 1NCE events, last 45 min" in text and "{id}" not in text and "{script}" not in text
    clock[0] += 300
    r = hb.run_once(d, fetcher(scenario(CYCLING, DRIVING, 900)), clock=lambda: clock[0], net=provider(over))
    assert r["budget_incident"] is None and len(hb.pending(d)) == 1                         # same episode
    clock[0] += hb.BUDGET_COOLDOWN_S
    r = hb.run_once(d, fetcher(scenario(CYCLING, DRIVING, 905)), clock=lambda: clock[0], net=provider(over))
    assert r["budget_incident"] == "20261026-121505-budget" and len(hb.pending(d)) == 2


def test_fifteen_is_not_over_and_old_events_do_not_count(breaker_off, tmp_path):
    d = str(tmp_path)
    baseline(d)
    fine = net_events(*update_locs(10, 55, 15), *[nce_raw(900 + i, "2026-10-26T10:%02d:00.000Z" % (10 + i), 1) for i in range(30)])
    r = hb.run_once(d, fetcher(scenario(CYCLING, DRIVING, 900)), clock=lambda: 2000.0, net=provider(fine))
    assert r["budget"] == (15, 15) and r["budget_incident"] is None and hb.pending(d) == []


def test_twenty_three_in_forty_five_minutes_is_over_even_if_the_last_fifteen_are_quiet(breaker_off, tmp_path):
    d = str(tmp_path)
    baseline(d)
    spread = net_events(*update_locs(0, 70, 23))                                             # 12:00:00 ... 12:25:40
    r = hb.run_once(d, fetcher(scenario(CYCLING, DRIVING, 2400)), clock=lambda: 2000.0, net=provider(spread))
    assert r["budget"][0] <= 15 < 23 == r["budget"][1] and r["budget_incident"] == "20261026-124000-budget"


def test_the_allowance_is_counted_with_the_breaker_on_too_and_shows_in_the_status(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(hb, "BREAKER_ON", True)
    d = str(tmp_path)
    baseline(d)
    hb.run_once(d, fetcher(scenario(CYCLING, DRIVING, 900)), clock=lambda: 2000.0,
                net=provider(net_events(*update_locs(10, 50, 8))))
    hb.status(d)
    assert "SIM Update location events: 8 in the last 15 min, 8 in 45" in capsys.readouterr().out


def test_an_unavailable_log_is_said_in_the_status_not_counted_as_zero(breaker_off, tmp_path, capsys):
    d = str(tmp_path)
    baseline(d)
    hb.run_once(d, fetcher(scenario(CYCLING, DRIVING, 900)), clock=lambda: 2000.0,
                net=provider({"events": [], "note": "NceError: HTTP 500"}))
    hb.status(d)
    assert "SIM Update location events: not available (NceError: HTTP 500)" in capsys.readouterr().out


def test_a_dry_run_shows_the_budget_review_without_queueing_it(breaker_off, tmp_path):
    over = net_events(*update_locs(10, 50, 16))
    r = hb.run_once(str(tmp_path), fetcher(scenario(CYCLING, DRIVING, 900)), dry_run=True, net=provider(over))
    assert "SIM allowance" in r["budget_bundle"] and hb.pending(str(tmp_path)) == []
