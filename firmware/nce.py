#!/usr/bin/env python3
"""
The car's SIM as the network saw it: 1NCE's event log, kept and lined up with the tracker's.

1NCE (Management API, api.1nce.com) records every attach a SIM makes: the MME's
location updates and each PDP context created, updated and deleted, with the cell
(ECI/TAC), operator and radio it landed on.  It keeps about a week.  This pulls the
events into ~/.nce/events/<label>.jsonl (de-duplicated, SIM identifiers dropped) and
reads them back, so a failure can still be matched to the network's side of it after
the API has forgotten it.

    nce.py sync                       fetch every SIM's events (cron, daily; the commands below
                                      do it themselves when the archive is more than 10 min old)
    nce.py sims                       the account's SIMs
    nce.py events  WINDOW             the events in a window, one line each
    nce.py summary WINDOW             counts, cells, session and off times, quiet stretches
    nce.py timeline WINDOW            the network's events, the tracker's datagrams and its
                                      hold-breaker log lines, interleaved on one clock

    WINDOW: --last 90m | --since "2026-10-25 14:50" [--until ...] | --around "14:52:27" [--span 5m]
    --sim car|bench (default car).  Times are UK local (Europe/London) unless --utc.

Credentials are ~/.1nce_creds: client_id, then client_secret, one per line.  They, the
token and the API's response bodies are never printed (an error body can echo the
login); an error shows a status and the API's generic error name.
"""
import argparse
import base64
import bisect
import collections
import datetime as dt
import fcntl
import json
import math
import os
import re
import statistics
import sys
import time
from zoneinfo import ZoneInfo

BASE = "https://api.1nce.com/management-api"
CREDS = os.path.expanduser("~/.1nce_creds")
HOME = os.path.expanduser("~/.nce")
TZ = "Europe/London"
PAGE = 1000                  # the API takes up to 1000 and refuses 2000
PACE_S = 0.15                # it allows 10 requests a second
FRESH_S = 600
QUIET_S = 90                 # a breaker cycle is ~26 s: this is a miss
UTC = dt.timezone.utc

RAT = {1: "UTRAN", 2: "GERAN", 3: "WLAN", 4: "GAN", 5: "HSPA+", 6: "EUTRAN", 7: "virtual",
       8: "NB-IoT", 9: "LTE-M"}
SHORT = {1: "Update loc", 2: "Update GPRS loc", 3: "Create PDP", 4: "Update PDP",
         5: "Delete PDP", 16: "Purge GPRS loc"}


class NceError(Exception):
    """Only what is safe to print: a status and the API's generic error name."""


# -- the API -----------------------------------------------------------------

def mask(path):
    return re.sub(r"/sims/(\d+)", lambda m: "/sims/..." + m.group(1)[-4:], path)


def generic_error(r):
    try:
        e = r.json().get("error", "")
    except (ValueError, AttributeError):
        return ""
    return e if isinstance(e, str) and re.fullmatch(r"[A-Za-z ._-]{1,40}", e) else ""


class Client:
    def __init__(self, creds=None, session=None, sleep=time.sleep, budget_s=None):
        self.creds, self.session, self.sleep = creds or CREDS, session, sleep
        self.token, self.last = None, 0.0
        self.deadline = time.monotonic() + budget_s if budget_s else None

    def _send(self, method, path, **kw):
        if self.session is None:
            import requests
            self.session = requests.Session()
        why = ""
        for attempt in range(4):
            left = self.deadline - time.monotonic() if self.deadline else 30
            if left <= 1:
                raise NceError("%s %s: out of time%s" % (method, mask(path), " (" + why + ")" if why else ""))
            wait = PACE_S - (time.monotonic() - self.last)
            if wait > 0:
                self.sleep(wait)
            self.last = time.monotonic()
            try:
                r = self.session.request(method, BASE + path, timeout=min(30, left), **kw)
            except Exception as e:                       # the text of a requests error holds the URL
                why = "network error (%s)" % type(e).__name__
            else:
                if r.status_code != 429 and r.status_code < 500:
                    return r
                why = "HTTP %d %s" % (r.status_code, generic_error(r))
            if attempt < 3:
                self.sleep(2 ** attempt)
        raise NceError("%s %s: %s" % (method, mask(path), why.strip()))

    def _authenticate(self):
        try:
            lines = open(self.creds).read().split("\n")
            cid, secret = lines[0].strip(), lines[1].strip()
        except (OSError, IndexError):
            cid = secret = ""
        if not cid or not secret:
            raise NceError("no usable credentials in %s (client_id, then client_secret, one per line)"
                           % self.creds)
        basic = base64.b64encode(("%s:%s" % (cid, secret)).encode()).decode()
        r = self._send("POST", "/oauth/token", json={"grant_type": "client_credentials"},
                       headers={"Authorization": "Basic " + basic, "Content-Type": "application/json"})
        try:
            self.token = r.json()["access_token"] if r.ok else None
        except (ValueError, KeyError, TypeError):
            self.token = None
        if not self.token:
            raise NceError("authentication failed: HTTP %d %s" % (r.status_code, generic_error(r)))

    def get(self, path, **params):
        for refreshed in (False, True):
            if not self.token:
                self._authenticate()
            r = self._send("GET", path, params=params, headers={"Authorization": "Bearer " + self.token})
            if r.status_code == 401 and not refreshed:
                self.token = None
                continue
            break
        if not r.ok:
            raise NceError("GET %s: HTTP %d %s" % (mask(path), r.status_code, generic_error(r)))
        return r

    def sims(self):
        sims = self.get("/v2/sims").json()
        if not isinstance(sims, list):
            raise NceError("GET /v2/sims: unexpected reply")
        return sims

    def events(self, iccid):
        """Everything the API holds, newest first.  Pages past x-total-count repeat
        older events instead of ending, so it stops at the total."""
        out, seen, page = [], set(), 1
        while True:
            r = self.get("/v2/sims/%s/events" % iccid, page=page, pageSize=PAGE)
            chunk = r.json()
            if not isinstance(chunk, list):
                raise NceError("GET /v2/sims/...%s/events: unexpected reply" % iccid[-4:])
            total = int(r.headers.get("x-total-count") or 0)
            new = [e for e in chunk if e["id"] not in seen]
            seen.update(e["id"] for e in new)
            out += new
            if not new or len(chunk) < PAGE or (total and len(seen) >= total):
                return out
            page += 1


# -- the archive -------------------------------------------------------------

def sanitize(e, label):
    """The event without the SIM's identifiers (IMSI, ICCID, MSISDN, the account)."""
    out = {k: e[k] for k in ("id", "timestamp", "alert", "event_type", "event_severity",
                             "event_source") if k in e}
    out["description"] = re.sub(r"(IMSI\s*=\s*')\d+(')", "\\1...\\2", e.get("description") or "")
    if e.get("detail"):
        d = json.loads(json.dumps(e["detail"]))
        (d.get("pdp_context") or {}).pop("imsi", None)
        out["detail"] = d
    ep = e.get("endpoint") or {}
    out["endpoint"] = {k: ep[k] for k in ("imei", "ip_address") if k in ep}
    out["sim"] = label
    return out


class Archive:
    def __init__(self, label, home=None):
        self.dir = os.path.join(home or HOME, "events")
        self.path = os.path.join(self.dir, "%s.jsonl" % label)

    def load(self):
        try:
            with open(self.path) as f:
                return {e["id"]: e for e in map(json.loads, filter(None, f.read().split("\n")))}
        except OSError:
            return {}

    def merge(self, events):
        os.makedirs(self.dir, mode=0o700, exist_ok=True)
        with open(os.path.join(self.dir, ".lock"), "w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            have = self.load()
            new = sum(1 for e in events if e["id"] not in have)
            have.update({e["id"]: e for e in events})
            tmp = self.path + ".tmp"
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                for e in sorted(have.values(), key=lambda e: (e["timestamp"], e["id"])):
                    f.write(json.dumps(e, separators=(",", ":")) + "\n")
            os.replace(tmp, self.path)
        return new


def state_path(home=None):
    return os.path.join(home or HOME, "state.json")


def sync(client=None, home=None, labels=None):
    """Pull every SIM's events into its archive.  Returns {label: (new, total)}."""
    client, home = client or Client(), home or HOME
    out = {}
    for s in client.sims():
        label = s.get("label") or s["iccid"][-6:]
        if labels and label not in labels:
            continue
        arch = Archive(label, home)
        new = arch.merge([sanitize(e, label) for e in client.events(s["iccid"])])
        out[label] = (new, len(arch.load()))
    os.makedirs(home, mode=0o700, exist_ok=True)
    with open(state_path(home), "w") as f:
        json.dump({"synced": time.time()}, f)
    return out


def archive_age(home=None):
    """Seconds since the last sync (infinity if there has been none)."""
    try:
        return time.time() - json.load(open(state_path(home)))["synced"]
    except (OSError, ValueError, KeyError):
        return math.inf


def ensure_fresh(max_age_s=FRESH_S, home=None, client=None):
    """Sync if the archive is older than max_age_s.  Returns None if it was fresh or
    synced, else why it could not be (the archive is then as old as its last sync)."""
    if archive_age(home) <= max_age_s:
        return None
    try:
        sync(client, home)
    except NceError as e:
        return str(e)
    return None


# -- reading it --------------------------------------------------------------

def parse_utc(s):
    return dt.datetime.fromisoformat(s.replace("Z", "+00:00"))


def flat(e):
    """One event as the fields that matter, times as aware UTC datetimes."""
    d = e.get("detail") or {}
    p = d.get("pdp_context") or {}
    uli = p.get("gtp_v2_uli") or {}
    t = parse_utc(e["timestamp"])
    created = parse_utc(p["tunnel_created"]) if p.get("tunnel_created") else None
    mme = re.search(r"MME='([^'.]+)", e.get("description") or "")
    vlr = re.search(r"VLR = '(\d+)'", e.get("description") or "")
    tid = e["event_type"]["id"]
    return {"id": e["id"], "t": t, "type_id": tid, "type": e["event_type"]["description"],
            "short": SHORT.get(tid, e["event_type"]["description"]), "op": d.get("name") or "",
            "rat": p.get("rat_type"), "eci": uli.get("eci"), "tac": uli.get("tac"),
            "mme": mme.group(1) if mme else None, "vlr": vlr.group(1)[-4:] if vlr else None,
            "sgsn": p.get("sgsn_control_plane_ip_address"),
            "created": created, "sess_s": (t - created).total_seconds() if created and tid == 5 else None,
            "mb": (d.get("volume") or {}).get("total"), "alert": bool(e.get("alert"))}


def load_events(label="car", home=None):
    return sorted((flat(e) for e in Archive(label, home).load().values()), key=lambda f: (f["t"], f["id"]))


def window(events, lo, hi):
    ts = [e["t"] for e in events]
    return events[bisect.bisect_left(ts, lo):bisect.bisect_right(ts, hi)]


def body(e):
    if e["eci"] is not None:
        what = "eci %s tac %s %s" % (e["eci"], e["tac"], RAT.get(e["rat"], e["rat"]))
    elif e["mme"]:
        what = "mme %s" % e["mme"][:14]
    elif e["vlr"]:
        what = "vlr ...%s" % e["vlr"]
    else:
        what = ""
    if e["sess_s"] is not None:
        what += "  session %ds %.3fMB" % (e["sess_s"], e["mb"] or 0)
    return "%-15s %-8s %s%s" % (e["short"], e["op"], what, "  ALERT" if e["alert"] else "")


def line(e, tz):
    return "%s  %s" % (e["t"].astimezone(tz).strftime("%m-%d %H:%M:%S.%f")[:-3], body(e))


def dist(xs):
    xs = sorted(xs)
    return "n %d, median %.0f s, p90 %.0f s, longest %.0f s" % (
        len(xs), statistics.median(xs), xs[min(len(xs) - 1, int(0.9 * len(xs)))], xs[-1])


def off_times(events):
    """Delete -> the next Create: how long the SIM was off the network, per cycle."""
    out, gone = [], None
    for e in events:
        if e["type_id"] == 5:
            gone = e
        elif e["type_id"] == 3 and gone:
            out.append((gone, e, (e["t"] - gone["t"]).total_seconds()))
            gone = None
    return out


def quiet(events, over_s):
    return [(a, b, (b["t"] - a["t"]).total_seconds()) for a, b in zip(events, events[1:])
            if (b["t"] - a["t"]).total_seconds() > over_s]


def summarize(events, tz, quiet_s=QUIET_S):
    if not events:
        return ["no events in the window"]
    n = collections.Counter(e["short"] for e in events)
    out = ["%d events, %s -> %s" % (len(events), events[0]["t"].astimezone(tz).strftime("%m-%d %H:%M:%S"),
                                    events[-1]["t"].astimezone(tz).strftime("%m-%d %H:%M:%S")),
           "  " + "  ".join("%s %d" % kv for kv in n.most_common())]
    cells = collections.Counter("%s/%s" % (e["eci"], e["tac"]) for e in events if e["eci"] is not None)
    if cells:
        out.append("  cells (eci/tac): " + "  ".join("%s x%d" % kv for kv in cells.most_common(8)))
    out.append("  operators: " + "  ".join("%s x%d" % kv for kv in collections.Counter(
        e["op"] for e in events if e["op"]).most_common()) + "   radio: " + "  ".join(
        "%s x%d" % (RAT.get(k, k), v) for k, v in collections.Counter(
            e["rat"] for e in events if e["rat"] is not None).most_common()))
    sess = [e["sess_s"] for e in events if e["sess_s"] is not None]
    if sess:
        out.append("  sessions (create -> delete): " + dist(sess))
    off = off_times(events)
    if off:
        worst = max(off, key=lambda o: o[2])
        out.append("  off the network (delete -> next create): " + dist([o[2] for o in off])
                   + " (at %s)" % worst[0]["t"].astimezone(tz).strftime("%H:%M:%S"))
    for e in events:
        if e["type_id"] == 16:
            out.append("  PURGE of the MME's location at " + e["t"].astimezone(tz).strftime("%m-%d %H:%M:%S"))
    q = quiet(events, quiet_s)
    out.append("  quiet stretches over %d s: none" % quiet_s if not q
               else "  quiet stretches over %d s:" % quiet_s)
    for a, b, s in q:
        out.append("    %s -> %s  %s   after %s (eci %s), before %s (eci %s)" % (
            a["t"].astimezone(tz).strftime("%H:%M:%S"), b["t"].astimezone(tz).strftime("%H:%M:%S"),
            "%.1f min" % (s / 60) if s >= 120 else "%d s" % s, a["short"], a["eci"], b["short"], b["eci"]))
    return out


def dur(seconds):
    return "%.1f h" % (seconds / 3600) if seconds >= 7200 else "%.1f min" % (seconds / 60) \
        if seconds >= 120 else "%d s" % seconds


def gap_facts(events, start, end, tz, cut_within_s=60):
    """The network's side of a silence on the tracker: start is the last datagram before
    it, end the first after (aware).  Anchored on the events, not on slack around the
    edges: the breaker cuts up to ~30 s after the last datagram, and a unit that rebooted
    sends its first datagram ~20 s after it attaches.  So: the first Delete PDP after the
    last datagram is the cut, the first Create PDP after that is the re-attach, and the
    rest is how they sit against the tracker's silence."""
    def at(e):
        return "%s %s (eci %s tac %s)" % (e["short"], e["t"].astimezone(tz).strftime("%H:%M:%S"),
                                          e["eci"], e["tac"])
    d = dt.timedelta
    before = [e for e in events if e["t"] <= start][-1:]
    session = [e for e in events if e["type_id"] in (3, 4, 5) and e["t"] <= start][-1:]
    cut = next((e for e in events if e["type_id"] == 5 and start - d(seconds=3) <= e["t"] <= start + d(seconds=cut_within_s)), None)
    out = ["last event before the silence: " + (at(before[0]) if before else "none in the archive")]
    if cut is None:
        nxt = next((e for e in events if e["t"] > start), None)
        if session and session[0]["type_id"] != 5:
            out.append("no detach near the last datagram: a PDP session was open throughout (%s), so the network "
                       "saw nothing go wrong and the stall was above it (radio link or RRC), not a failed "
                       "re-attach; next event: %s" % (at(session[0]), at(nxt) if nxt else "none yet"))
        else:
            out.append("no detach seen near the last datagram; next event: " + (at(nxt) if nxt else "none yet"))
        return out
    attach = next((e for e in events if e["type_id"] == 3 and e["t"] > cut["t"]), None)
    lead = "detached at %s (%d s after the last datagram)" % (cut["t"].astimezone(tz).strftime("%H:%M:%S"),
                                                               (cut["t"] - start).total_seconds())
    if attach is None:
        out.append(lead + " and no attach attempt has reached the network since")
        return out
    off = (attach["t"] - cut["t"]).total_seconds()
    between = [e for e in events if cut["t"] < e["t"] < attach["t"]]
    out.append("%s; %s until %s, %s later" % (
        lead, "the network saw %d more events (%s)" % (len(between), ", ".join(
            "%s x%d" % kv for kv in collections.Counter(e["short"] for e in between).most_common()))
        if between else "no attach attempt reached the network", at(attach), dur(off)))
    lag = (end - attach["t"]).total_seconds()
    out.append("the tracker's first datagram came %s after that attach%s" % (
        dur(lag), "" if lag <= 60 else ", so the network side explains only the first %s of the silence" % dur(off)))
    return out


# -- times -------------------------------------------------------------------

def parse_span(s):
    m = re.fullmatch(r"(\d+(?:\.\d+)?)([smhd])", s.strip())
    if not m:
        raise SystemExit("bad span %r (use 90s, 15m, 2h, 1d)" % s)
    return dt.timedelta(seconds=float(m.group(1)) * {"s": 1, "m": 60, "h": 3600, "d": 86400}[m.group(2)])


def parse_when(s, tz, now=None):
    now = now or dt.datetime.now(UTC)
    if s == "now":
        return now
    m = re.fullmatch(r"(?:(\d{4}-\d\d-\d\d)[ T])?(\d\d:\d\d(?::\d\d(?:\.\d+)?)?)", s.strip())
    if not m:
        raise SystemExit("bad time %r (use 'YYYY-MM-DD HH:MM[:SS]' or 'HH:MM[:SS]' for today)" % s)
    day = m.group(1) or now.astimezone(tz).strftime("%Y-%m-%d")
    t = m.group(2) if m.group(2).count(":") == 2 else m.group(2) + ":00"
    return dt.datetime.fromisoformat("%s %s" % (day, t)).replace(tzinfo=tz).astimezone(UTC)


def get_window(a, tz, now=None):
    now = now or dt.datetime.now(UTC)
    if a.around:
        c, w = parse_when(a.around, tz, now), parse_span(a.span)
        return c - w, c + w
    if a.since:
        return parse_when(a.since, tz, now), parse_when(a.until, tz, now) if a.until else now
    return now - parse_span(a.last or "2h"), now


# -- the tracker's side, for the timeline ------------------------------------

def timeline(events, lo, hi, tz, all_log=False):
    import hbwatch
    lo_l, hi_l = (t.astimezone(ZoneInfo(hbwatch.SERVER_TZ)).replace(tzinfo=None) for t in (lo, hi))
    hours = math.ceil((hi_l - lo_l).total_seconds() / 3600) + 1              # the server cuts from hi_l back
    sec = hbwatch.fetch_remote(hi_l.strftime(hbwatch.TS), lookback_h=hours)
    srv = ZoneInfo(hbwatch.SERVER_TZ)
    rows = [(e["t"], "NET", body(e)) for e in events]
    arr = [t for t in hbwatch.parse_arrivals(sec["ARRIVALS"]) if lo_l <= t <= hi_l]
    burst = []
    for t in arr:
        if burst and (t - burst[-1]).total_seconds() <= 3:
            burst.append(t)
        else:
            if burst:
                rows.append((burst[0].replace(tzinfo=srv).astimezone(UTC), "DGM", "%d datagram(s)" % len(burst)))
            burst = [t]
    if burst:
        rows.append((burst[0].replace(tzinfo=srv).astimezone(UTC), "DGM", "%d datagram(s)" % len(burst)))
    for l in hbwatch.parse_log(sec["LOG"]):
        if lo_l <= l["t"] <= hi_l and (all_log or re.search(
                r"hold break|registration|ignition|PSM|modem|rejected|LTE", l["msg"])):
            rows.append((l["t"].replace(tzinfo=srv).astimezone(UTC), "LOG", l["msg"][:150]))
    rows.sort(key=lambda r: r[0])
    return ["%s  %s  %s" % (t.astimezone(tz).strftime("%m-%d %H:%M:%S.%f")[:-3], k, text) for t, k, text in rows]


# -- command line ------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("sync")
    sub.add_parser("sims")
    for name in ("events", "summary", "timeline"):
        p = sub.add_parser(name)
        p.add_argument("--sim", default="car")
        p.add_argument("--last")
        p.add_argument("--since")
        p.add_argument("--until")
        p.add_argument("--around")
        p.add_argument("--span", default="5m")
        p.add_argument("--utc", action="store_true")
        p.add_argument("--offline", action="store_true", help="do not refresh the archive first")
        if name == "events":
            p.add_argument("--type", help="e.g. 'Create PDP', 'Delete PDP', 'purge'")
            p.add_argument("--json", action="store_true")
        if name == "summary":
            p.add_argument("--quiet", type=int, default=QUIET_S, help="seconds of silence to report")
        if name == "timeline":
            p.add_argument("--all-log", action="store_true")
    a = ap.parse_args(argv)

    try:
        if a.cmd == "sync":
            for label, (new, total) in sorted(sync().items()):
                print("%s %s: %d new, %d archived" % (time.strftime("%Y-%m-%d %H:%M:%S"), label, new, total))
            return 0
        if a.cmd == "sims":
            for s in Client().sims():
                print("%-6s ...%s  %-8s imei %s  quota %s  %s" % (
                    s.get("label"), s["iccid"][-4:], s.get("status"), (s.get("imei") or "")[:14] + "..",
                    s.get("current_quota"), (s.get("quota_status") or {}).get("description", "")))
            return 0
        tz = UTC if a.utc else ZoneInfo(TZ)
        if not a.offline:
            why = ensure_fresh()
            if why:
                print("nce: could not refresh (%s); using the archive as it is" % why, file=sys.stderr)
        known = sorted(f[:-6] for f in os.listdir(os.path.join(HOME, "events")) if f.endswith(".jsonl")) \
            if os.path.isdir(os.path.join(HOME, "events")) else []
        if a.sim not in known:
            raise NceError("no archive for SIM %r (have: %s)" % (a.sim, ", ".join(known) or "none"))
        lo, hi = get_window(a, tz)
        evs = window(load_events(a.sim), lo, hi)
        if a.cmd == "events":
            if a.type:
                evs = [e for e in evs if a.type.lower() in e["type"].lower() or a.type.lower() in e["short"].lower()]
            for e in evs:
                print(json.dumps({**e, "t": e["t"].isoformat(), "created": e["created"] and e["created"].isoformat()})
                      if a.json else line(e, tz))
            print("# %d events %s -> %s (%s)" % (len(evs), lo.astimezone(tz).strftime("%m-%d %H:%M:%S"),
                                                 hi.astimezone(tz).strftime("%m-%d %H:%M:%S"), tz.key if hasattr(tz, "key") else "UTC"),
                  file=sys.stderr)
        elif a.cmd == "summary":
            print("\n".join(summarize(evs, tz, a.quiet)))
        else:
            print("\n".join(timeline(evs, lo, hi, tz, a.all_log)))
    except NceError as e:
        print("nce: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
