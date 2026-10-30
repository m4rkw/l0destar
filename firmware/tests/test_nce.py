"""Host tests for nce.py: the 1NCE client, its archive, and the readings built on it.

No network: the HTTP session is a stand-in that answers the way the API does,
quirks included (pages past the total repeat; an error body echoes the login).
The identifiers below are made up.

    uv run --python 3.12 --with pytest pytest -q firmware/tests
"""
import base64
import datetime as dt
import json
import os
import stat
import sys
from zoneinfo import ZoneInfo

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import nce  # noqa: E402

CID, SECRET, TOKEN = "login-0001", "s3cr3t-value-xyz", "tok-abcdef123456"
ICCID, IMSI, MSISDN = "8900000000000000001", "999990000000001", "999000000001"
UTC = dt.timezone.utc
LONDON = ZoneInfo("Europe/London")
TYPES = {1: "Update location", 2: "Update GPRS location", 3: "Create PDP Context",
         4: "Update PDP Context", 5: "Delete PDP Context", 16: "Purge GPRS location"}


def utc(s):
    return dt.datetime.fromisoformat(s).replace(tzinfo=UTC)


def raw(i, ts, tid, eci=5425152, tac=26731, created=None, mb=0.006, desc=None, rat=6):
    """An event as the API gives it, identifiers and all."""
    e = {"id": i, "timestamp": ts, "alert": False, "event_type": {"id": tid, "description": TYPES[tid]},
         "event_severity": {"id": 0, "description": "INFO"}, "event_source": {"id": 0, "description": "Network"},
         "description": desc or TYPES[tid], "imsi": {"id": 7, "imsi": IMSI},
         "organisation": {"id": 9, "name": "90009999"}, "sim": {"iccid": ICCID, "id": 5, "msisdn": MSISDN},
         "endpoint": {"id": 4, "imei": "3550259363868708", "ip_address": "10.0.0.1", "name": IMSI, "platform": 2}}
    if tid in (3, 4, 5):
        e["detail"] = {"name": "EE", "country": {"mcc": "234"}, "pdp_context": {
            "imsi": IMSI, "imeisv": "3550259363868708", "rat_type": rat, "tunnel_created": created or ts,
            "sgsn_control_plane_ip_address": "10.1.1.1", "gtp_v2_uli": {"eci": eci, "tac": tac}}}
        if tid == 5:
            e["detail"]["volume"] = {"rx": 0.001, "tx": mb - 0.001, "total": mb}
    elif tid in (1, 2):
        e["detail"] = {"name": "EE", "mnc": "30"}
        e["description"] = desc or ("New location received from VLR for IMSI = '%s', now attached to "
                                    "VLR = '441234560127'" % IMSI if tid == 1 else
                                    "New location received from MME for IMSI = '%s', now attached to "
                                    "MME='acm17bktmmk.epc.mnc030.mcc234.3gppnetwork.org'" % IMSI)
    return e


class Resp:
    def __init__(self, status=200, body=None, headers=None):
        self.status_code, self._body, self.headers = status, body, headers or {}
        self.text = json.dumps(body) if body is not None else ""

    @property
    def ok(self):
        return self.status_code < 400

    def json(self):
        if self._body is None:
            raise ValueError("no body")
        return self._body


class Session:
    """request(method, url, **kw) -> Resp, from a handler; keeps the calls."""

    def __init__(self, handler):
        self.handler, self.calls = handler, []

    def request(self, method, url, **kw):
        self.calls.append((method, url, kw))
        return self.handler(method, url, **kw)


def creds_file(tmp_path, text="%s\n%s\n" % (CID, SECRET)):
    p = tmp_path / "creds"
    p.write_text(text)
    return str(p)


def api(events, sims=None, total=None):
    """The API: token, /v2/sims, and paged events that repeat once the pages run out."""
    sims = sims if sims is not None else [{"label": "car", "iccid": ICCID}]

    def handler(method, url, **kw):
        if url.endswith("/oauth/token"):
            return Resp(200, {"access_token": TOKEN, "expires_in": 3600})
        if url.endswith("/v2/sims"):
            return Resp(200, sims)
        page, size = kw["params"]["page"], kw["params"]["pageSize"]
        n = len(events)
        start = ((page - 1) * size) % n if n else 0                  # wraps: the real API does
        chunk = (events * 3)[start:start + size] if page * size > n and page > 1 else events[start:start + size]
        return Resp(200, chunk, {"x-total-count": str(total if total is not None else n)})
    return Session(handler)


def client(tmp_path, session):
    return nce.Client(creds=creds_file(tmp_path), session=session, sleep=lambda s: None)


# -- the client --------------------------------------------------------------

def test_it_authenticates_with_basic_auth_then_bearer(tmp_path):
    s = api([raw(1, "2026-10-26T10:00:00.000Z", 3)])
    client(tmp_path, s).sims()
    (m1, u1, k1), (m2, u2, k2) = s.calls
    assert (m1, u1.endswith("/oauth/token"), k1["json"]) == ("POST", True, {"grant_type": "client_credentials"})
    assert base64.b64decode(k1["headers"]["Authorization"].split()[1]).decode() == "%s:%s" % (CID, SECRET)
    assert k2["headers"]["Authorization"] == "Bearer " + TOKEN


def test_an_expired_token_is_replaced_once(tmp_path):
    seen = {"n": 0}

    def handler(method, url, **kw):
        if url.endswith("/oauth/token"):
            return Resp(200, {"access_token": TOKEN})
        seen["n"] += 1
        return Resp(401, {"error": "Unauthorized"}) if seen["n"] == 1 else Resp(200, [])
    s = Session(handler)
    assert client(tmp_path, s).sims() == []
    assert sum(1 for c in s.calls if c[1].endswith("/oauth/token")) == 2


def test_events_stop_at_the_total_although_the_api_wraps(tmp_path, monkeypatch):
    monkeypatch.setattr(nce, "PAGE", 3)
    evs = [raw(i, "2026-10-26T10:00:%02d.000Z" % i, 3) for i in range(1, 8)]          # 7 events, pages of 3
    got = client(tmp_path, api(evs)).events(ICCID)
    assert sorted(e["id"] for e in got) == list(range(1, 8))


def test_events_stop_when_a_page_adds_nothing_new(tmp_path, monkeypatch):
    monkeypatch.setattr(nce, "PAGE", 2)
    evs = [raw(i, "2026-10-26T10:00:%02d.000Z" % i, 3) for i in range(1, 5)]
    got = client(tmp_path, api(evs, total=999)).events(ICCID)                  # a total that is never reached
    assert len(got) == 4


@pytest.mark.parametrize("status", [400, 403, 500])
def test_an_error_never_carries_the_body_the_login_the_secret_or_the_iccid(tmp_path, status):
    def handler(method, url, **kw):
        if url.endswith("/oauth/token"):
            return Resp(200, {"access_token": TOKEN})
        return Resp(status, {"error": "Internal Server Error", "message": "Could not load data for login %s" % CID,
                             "path": "/v2/sims/%s/events" % ICCID})
    with pytest.raises(nce.NceError) as e:
        client(tmp_path, Session(handler)).events(ICCID)
    text = str(e.value)
    assert "HTTP %d" % status in text
    for leaked in (CID, SECRET, TOKEN, ICCID, "Could not load"):
        assert leaked not in text


def test_a_network_error_is_named_not_quoted(tmp_path):
    def handler(method, url, **kw):
        if url.endswith("/oauth/token"):
            return Resp(200, {"access_token": TOKEN})
        raise ConnectionError("HTTPSConnectionPool: Max retries exceeded with url: /v2/sims/%s/events" % ICCID)
    with pytest.raises(nce.NceError) as e:
        client(tmp_path, Session(handler)).events(ICCID)
    assert "ConnectionError" in str(e.value) and ICCID not in str(e.value)


def test_a_failed_login_says_so_without_the_credentials(tmp_path):
    s = Session(lambda m, u, **kw: Resp(401, {"error": "Unauthorized", "message": "bad login %s" % CID}))
    with pytest.raises(nce.NceError) as e:
        client(tmp_path, s).sims()
    assert "authentication failed: HTTP 401" in str(e.value)
    assert CID not in str(e.value) and SECRET not in str(e.value)


@pytest.mark.parametrize("text", ["", "only-one-line\n", "\n\n"])
def test_missing_or_short_credentials_are_reported_by_path_only(tmp_path, text):
    with pytest.raises(nce.NceError) as e:
        nce.Client(creds=creds_file(tmp_path, text), session=api([]), sleep=lambda s: None).sims()
    assert "no usable credentials" in str(e.value) and "only-one-line" not in str(e.value)


def test_the_time_budget_stops_a_slow_api(tmp_path):
    c = nce.Client(creds=creds_file(tmp_path), session=api([]), sleep=lambda s: None, budget_s=0.5)
    with pytest.raises(nce.NceError, match="out of time"):
        c.sims()


# -- the archive -------------------------------------------------------------

def test_sanitize_keeps_the_cell_and_drops_every_identifier():
    e = nce.sanitize(raw(1, "2026-10-26T10:00:00.000Z", 5, created="2026-10-26T09:59:00.000Z"), "car")
    text = json.dumps(e)
    for ident in (IMSI, ICCID, MSISDN, "90009999"):
        assert ident not in text
    f = nce.flat(e)
    assert (f["eci"], f["tac"], f["sess_s"], f["op"]) == (5425152, 26731, 60, "EE")
    assert e["sim"] == "car" and e["endpoint"]["imei"] == "3550259363868708"


def test_location_events_lose_the_imsi_from_their_text_but_keep_the_node():
    f = nce.flat(nce.sanitize(raw(1, "2026-10-26T10:00:00.000Z", 2), "car"))
    g = nce.flat(nce.sanitize(raw(2, "2026-10-26T10:00:01.000Z", 1), "car"))
    assert f["mme"] == "acm17bktmmk" and g["vlr"] == "0127"
    assert IMSI not in json.dumps(nce.sanitize(raw(1, "2026-10-26T10:00:00.000Z", 2), "car"))


def test_the_archive_merges_without_duplicates_and_is_private(tmp_path):
    a = nce.Archive("car", str(tmp_path))
    evs = [nce.sanitize(raw(i, "2026-10-26T10:00:%02d.000Z" % i, 3), "car") for i in (3, 1, 2)]
    assert a.merge(evs) == 3
    assert a.merge(evs + [nce.sanitize(raw(4, "2026-10-26T10:00:04.000Z", 3), "car")]) == 1
    assert [e["id"] for e in map(json.loads, open(a.path))] == [1, 2, 3, 4]            # kept in time order
    assert stat.S_IMODE(os.stat(a.path).st_mode) == 0o600
    assert stat.S_IMODE(os.stat(a.dir).st_mode) == 0o700


def test_sync_files_each_sim_under_its_label_and_ensure_fresh_only_syncs_when_stale(tmp_path):
    evs = [raw(1, "2026-10-26T10:00:00.000Z", 3)]
    s = api(evs, sims=[{"label": "car", "iccid": ICCID}, {"label": "bench", "iccid": "8900000000000000002"}])
    c = client(tmp_path, s)
    home = str(tmp_path / "home")
    assert nce.sync(c, home) == {"car": (1, 1), "bench": (1, 1)}
    assert nce.sync(c, home) == {"car": (0, 1), "bench": (0, 1)}                         # nothing new the second time
    assert len(nce.load_events("car", home)) == 1 and len(nce.load_events("bench", home)) == 1
    n = len(s.calls)
    assert nce.ensure_fresh(600, home, c) is None and len(s.calls) == n                  # fresh: no call
    state = json.load(open(nce.state_path(home)))
    state["synced"] -= 700
    json.dump(state, open(nce.state_path(home), "w"))
    assert nce.ensure_fresh(600, home, c) is None and len(s.calls) > n                   # stale: synced


def test_ensure_fresh_reports_a_failed_refresh_instead_of_raising(tmp_path):
    bad = Session(lambda m, u, **kw: Resp(500, {"error": "Internal Server Error"}))
    why = nce.ensure_fresh(600, str(tmp_path / "h"), client(tmp_path, bad))
    assert why and "HTTP" in why


# -- times and windows -------------------------------------------------------

def test_local_times_become_utc_across_the_clock_change():
    assert nce.parse_when("2026-10-25 14:52:27", LONDON) == utc("2026-10-25 14:52:27")          # GMT
    assert nce.parse_when("2026-10-24 15:30:11", LONDON) == utc("2026-10-24 14:30:11")          # BST
    assert nce.parse_when("14:52", LONDON, utc("2026-10-26 12:00:00")) == utc("2026-10-26 14:52:00")


@pytest.mark.parametrize("s,secs", [("90s", 90), ("15m", 900), ("2h", 7200), ("1d", 86400), ("1.5h", 5400)])
def test_spans(s, secs):
    assert nce.parse_span(s).total_seconds() == secs


def test_bad_times_and_spans_are_refused_by_name():
    for bad in ("tomorrow", "25/10 14:52"):
        with pytest.raises(SystemExit, match="bad time"):
            nce.parse_when(bad, LONDON)
    with pytest.raises(SystemExit, match="bad span"):
        nce.parse_span("soon")


# -- the readings ------------------------------------------------------------

def cycle(i, hh_mm_ss, eci=5425152):
    """Create, Update loc, Update PDP, then a Delete 2 s later: one breaker cycle."""
    base = dt.datetime.fromisoformat("2026-10-25 " + hh_mm_ss)
    ts = lambda s: (base + dt.timedelta(seconds=s)).strftime("%Y-%m-%dT%H:%M:%S.000Z")
    return [nce.flat(raw(i, ts(0), 3, eci)), nce.flat(raw(i + 1, ts(.4), 1)), nce.flat(raw(i + 2, ts(.8), 4, eci)),
            nce.flat(raw(i + 3, ts(2), 5, eci, created=ts(0)))]


def test_summary_finds_the_stretch_the_network_went_quiet_and_the_off_times():
    evs = []
    for n, t in enumerate(["14:50:00", "14:50:26", "14:52:25"]):
        evs += cycle(10 * n + 1, t)
    evs += cycle(100, "15:35:29")
    out = "\n".join(nce.summarize(evs, UTC, quiet_s=90))
    assert "16 events" in out and "Create PDP 4" in out
    assert "off the network" in out and "longest 2582 s" in out
    assert "14:52:27 -> 15:35:29  43.0 min" in out


def test_summary_of_nothing_says_so():
    assert nce.summarize([], UTC) == ["no events in the window"]


def test_the_cut_and_the_reattach_bound_the_silence():
    # 25 Oct 14:52 on the real car: the tracker's last datagram, the network's Delete 0.3 s
    # later, nothing for 43 min, then the attach 6 s before the tracker's next datagram
    evs = cycle(1, "14:52:25") + cycle(10, "15:35:29")
    out = nce.gap_facts(evs, utc("2026-10-25 14:52:26.700"), utc("2026-10-25 15:35:35.250"), UTC)
    assert out[0].startswith("last event before the silence: Update PDP 14:52:25")
    assert out[1] == ("detached at 14:52:27 (0 s after the last datagram); no attach attempt reached the network "
                      "until Create PDP 15:35:29 (eci 5425152 tac 26731), 43.0 min later")
    assert out[2] == "the tracker's first datagram came 6 s after that attach"


def test_a_cut_ten_seconds_after_the_last_datagram_is_still_the_cut():
    # 26 Oct 10:55 on the real car: the probe held the connection 10 s past the last send before
    # cutting, and the unit stayed off the network for 66 min.  A fixed 10 s edge called the
    # Delete "inside the silence" and the bundle said the SIM had reached the network.
    evs = [nce.flat(raw(1, "2026-10-25T10:53:59.000Z", 3)), nce.flat(raw(2, "2026-10-25T10:54:00.111Z", 4)),
           nce.flat(raw(3, "2026-10-25T10:54:58.183Z", 5, created="2026-10-25T10:53:59.000Z"))] + cycle(10, "12:01:18")
    out = nce.gap_facts(evs, utc("2026-10-25 10:54:48.132"), utc("2026-10-25 12:01:19.602"), UTC)
    assert out[0].startswith("last event before the silence: Update PDP 10:54:00")
    assert out[1].startswith("detached at 10:54:58 (10 s after the last datagram); no attach attempt reached the network "
                             "until Create PDP 12:01:18")
    assert out[1].endswith("66.3 min later")
    assert out[2] == "the tracker's first datagram came 1 s after that attach"


def test_an_attach_inside_the_silence_is_reported_with_how_little_of_it_the_network_explains():
    evs = cycle(1, "14:52:25") + cycle(10, "14:56:00") + cycle(20, "15:35:29")
    out = nce.gap_facts(evs, utc("2026-10-25 14:52:26.700"), utc("2026-10-25 15:35:35.250"), UTC)
    assert out[1] == ("detached at 14:52:27 (0 s after the last datagram); no attach attempt reached the network "
                      "until Create PDP 14:56:00 (eci 5425152 tac 26731), 3.5 min later")
    assert out[2] == ("the tracker's first datagram came 39.6 min after that attach, so the network side "
                      "explains only the first 3.5 min of the silence")


def test_a_slow_first_datagram_after_a_reboot_is_still_matched_to_its_attach():
    # 26 Oct 13:10 on the real car: cut at once, watchdog reset, re-attach 2.2 min later and the
    # first datagram 19 s after that (a fresh boot waits for a fix)
    evs = cycle(1, "13:10:43") + cycle(10, "13:12:59")
    out = nce.gap_facts(evs, utc("2026-10-25 13:10:44.644"), utc("2026-10-25 13:13:18.873"), UTC)
    assert out[1].startswith("detached at 13:10:45 (0 s after the last datagram); no attach attempt reached the network")
    assert out[1].endswith("2.2 min later")
    assert out[2] == "the tracker's first datagram came 19 s after that attach"


def test_events_between_the_cut_and_the_attach_are_counted():
    evs = cycle(1, "14:52:25") + [nce.flat(raw(50, "2026-10-25T14:55:00.000Z", 4)),
                                  nce.flat(raw(51, "2026-10-25T14:55:30.000Z", 4))] + cycle(10, "15:35:29")
    evs.sort(key=lambda e: e["t"])
    out = nce.gap_facts(evs, utc("2026-10-25 14:52:26.700"), utc("2026-10-25 15:35:35.250"), UTC)
    assert "the network saw 2 more events (Update PDP x2) until Create PDP 15:35:29" in out[1]


def test_a_cut_with_no_attach_since_says_so():
    out = nce.gap_facts(cycle(1, "14:52:25"), utc("2026-10-25 14:52:26.700"), utc("2026-10-25 16:00:00"), UTC)
    assert out[1] == "detached at 14:52:27 (0 s after the last datagram) and no attach attempt has reached the network since"
    assert len(out) == 2


def test_an_empty_archive_is_said_not_guessed():
    out = nce.gap_facts([], utc("2026-10-25 14:52:26.700"), utc("2026-10-25 15:35:35.250"), UTC)
    assert out == ["last event before the silence: none in the archive",
                   "no detach seen near the last datagram; next event: none yet"]


def test_an_open_session_through_the_silence_is_a_stall_above_the_network():
    evs = [nce.flat(raw(1, "2026-10-25T11:58:41.000Z", 4)), nce.flat(raw(2, "2026-10-25T13:29:58.000Z", 5))]
    out = nce.gap_facts(evs, utc("2026-10-25 11:59:04"), utc("2026-10-25 12:00:07"), UTC)
    assert "a PDP session was open throughout" in out[1] and "not a failed re-attach" in out[1]


def test_lines_are_in_the_requested_zone():
    e = nce.flat(raw(1, "2026-07-01T11:00:00.500Z", 3))
    assert nce.line(e, LONDON).startswith("07-01 12:00:00.500")
    assert nce.line(e, UTC).startswith("07-01 11:00:00.500")


def test_the_cli_prints_events_in_local_time_from_the_archive_alone(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(nce, "HOME", str(tmp_path))
    nce.Archive("car").merge([nce.sanitize(raw(1, "2026-10-25T14:52:27.829Z", 5), "car"),
                              nce.sanitize(raw(2, "2026-10-25T15:35:29.911Z", 3), "car")])
    assert nce.main(["events", "--offline", "--since", "2026-10-25 14:50", "--until", "2026-10-25 15:40"]) == 0
    out = capsys.readouterr()
    assert "10-25 14:52:27.829  Delete PDP" in out.out and "10-25 15:35:29.911  Create PDP" in out.out
    assert nce.main(["events", "--offline", "--since", "2026-10-25 14:50", "--until", "2026-10-25 15:40",
                     "--type", "create"]) == 0
    assert "Delete PDP" not in capsys.readouterr().out


def test_a_reply_that_is_not_a_list_is_an_error_not_a_crash(tmp_path):
    def handler(method, url, **kw):
        if url.endswith("/oauth/token"):
            return Resp(200, {"access_token": TOKEN})
        return Resp(200, {"unexpected": "shape"})
    with pytest.raises(nce.NceError, match="unexpected reply"):
        client(tmp_path, Session(handler)).events(ICCID)
    with pytest.raises(nce.NceError, match="unexpected reply"):
        client(tmp_path, Session(handler)).sims()


def test_it_stops_retrying_after_four_goes_and_does_not_wait_after_the_last(tmp_path):
    naps = []
    s = Session(lambda m, u, **kw: Resp(200, {"access_token": TOKEN}) if u.endswith("/oauth/token")
                else Resp(500, {"error": "Internal Server Error"}))
    c = nce.Client(creds=creds_file(tmp_path), session=s, sleep=naps.append)
    with pytest.raises(nce.NceError, match="HTTP 500 Internal Server Error"):
        c.sims()
    assert sum(1 for x in s.calls if not x[1].endswith("/oauth/token")) == 4
    assert [n for n in naps if n >= 1] == [1, 2, 4]


def test_an_unknown_sim_label_is_named_with_the_ones_there_are(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(nce, "HOME", str(tmp_path))
    nce.Archive("car").merge([nce.sanitize(raw(1, "2026-10-25T14:52:27.829Z", 5), "car")])
    assert nce.main(["events", "--offline", "--sim", "carr", "--last", "1h"]) == 1
    assert "no archive for SIM 'carr' (have: car)" in capsys.readouterr().err
