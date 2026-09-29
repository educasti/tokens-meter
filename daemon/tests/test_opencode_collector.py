#!/usr/bin/env python3
"""Unit tests for the OpenCode collector (SPEC §2, §8, §9).

Everything is hermetic: a throwaway SQLite DB in tmp_path standing in for
opencode.db, a fake transport standing in for the Go usage endpoint, and a
sentinel key that must never show up in a log line.

Run: python -m pytest daemon/tests/test_opencode_collector.py -q
"""
import json
import logging
import sqlite3
import time
from datetime import datetime, timezone
from pathlib import Path

import pytest

import daemon.claude_usage_daemon as daemon_mod
from daemon.opencode_collector import (
    PAYLOAD_MAX,
    USAGE_URL,
    collect,
    estimate_usage,
    map_api_usage,
    model_monthly_limit,
    short_model_name,
)

# A key that must never be logged, printed or embedded in an exception.
SECRET = "sk-test-SUPERSECRET-GO-KEY-0001"

# Wednesday 2026-09-30 12:00 UTC — mid-week and mid-month, so the weekly and
# monthly windows are unambiguous.
NOW = datetime(2026, 9, 30, 12, 0, tzinfo=timezone.utc).timestamp()

HOUR_MS = 3_600_000


# ---------------------------------------------------------------------------
# fixtures: a small opencode.db with the real column names (research/01)
# ---------------------------------------------------------------------------

def _msg(tokens: dict, cost: float, model: str = "deepseek-v4.1-flash",
         provider: str = "opencode-go") -> str:
    return json.dumps({
        "time": {"created": 0, "streamed": 0, "completed": 0},
        "agent": "build",
        "model": {"id": model, "providerID": provider, "variant": "default"},
        "finish": "stop",
        "cost": cost,
        "tokens": tokens,
    })


def _tokens(inp, out, cache_read=0, cache_write=0, reasoning=0) -> dict:
    return {"input": inp, "output": out, "reasoning": reasoning,
            "cache": {"read": cache_read, "write": cache_write}}


@pytest.fixture
def oc_db(tmp_path: Path) -> Path:
    """Fixture DB: two sessions and a handful of assistant steps.

    Layout (relative to NOW):
      -30h  space-bunny-free, 7.7M tokens, $0  (free model -> must be skipped)
      -20h  deepseek-v4.1-flash, 51.2M tokens, $0.60  (before the 5h window)
       -2h  deepseek-v4.1-flash, 20.6M tokens, $0.60  (starts the 5h window)
       -1h  deepseek-v4.1-flash, 10.0M tokens, $0.60  (last step)
      -10d  space-bunny-free, 50.0M tokens, $0  (outside the 7d window)
    """
    path = tmp_path / "opencode.db"
    conn = sqlite3.connect(path)
    conn.execute(
        "create table session_v2 (id text primary key, agent text, model text, "
        "time_created integer not null, time_updated integer not null)"
    )
    conn.execute(
        "create table session_message (id text primary key, session_id text not null, "
        "type text not null, seq integer not null, time_created integer not null, "
        "time_updated integer not null, data text not null)"
    )
    now_ms = int(NOW * 1000)
    # One session touched 2 min ago (active), one an hour ago (not).
    conn.execute("insert into session_v2 values (?,?,?,?,?)",
                 ("ses_active", "build", None, now_ms - 3 * HOUR_MS, now_ms - 2 * 60_000))
    conn.execute("insert into session_v2 values (?,?,?,?,?)",
                 ("ses_old", "plan", None, now_ms - 4 * HOUR_MS, now_ms - 1 * HOUR_MS))

    steps = [
        (-30 * HOUR_MS, _tokens(1_000_000, 200_000, 6_500_000), 0.0, "space-bunny-free"),
        (-20 * HOUR_MS, _tokens(1_000_000, 200_000, 50_000_000), 0.60, "deepseek-v4.1-flash"),
        (-2 * HOUR_MS, _tokens(500_000, 100_000, 20_000_000), 0.60, "deepseek-v4.1-flash"),
        (-1 * HOUR_MS, _tokens(2_000_000, 1_000_000, 7_000_000), 0.60, "deepseek-v4.1-flash"),
        (-10 * 24 * HOUR_MS, _tokens(1_000_000, 1_000_000, 48_000_000), 0.0, "space-bunny-free"),
    ]
    for i, (offset, tokens, cost, model) in enumerate(steps):
        ts = now_ms + offset
        conn.execute("insert into session_message values (?,?,?,?,?,?,?)",
                     (f"msg_{i}", "ses_active", "assistant", i, ts, ts, _msg(tokens, cost, model)))
    # A user turn must not be counted as a step.
    conn.execute("insert into session_message values (?,?,?,?,?,?,?)",
                 ("msg_user", "ses_active", "user", 99, now_ms - 60_000, now_ms - 60_000,
                  json.dumps({"type": "user"})))
    conn.commit()
    conn.close()
    return path


@pytest.fixture
def auth_file(tmp_path: Path) -> Path:
    path = tmp_path / "auth.json"
    path.write_text(json.dumps({"opencode-go": {"type": "api", "key": SECRET}}))
    return path


# ---------------------------------------------------------------------------
# fake transport
# ---------------------------------------------------------------------------

def _iso(ts: float) -> str:
    return datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.249Z")


def usage_body(now: float = NOW, rolling: int = 0, weekly: int = 0, monthly: int = 3,
               statuses: tuple[str, str, str] = ("ok", "ok", "ok"),
               reset_in: tuple[float, float, float] = (5 * 3600, 4.5 * 86400, 12.5 * 86400)) -> str:
    """The endpoint's real body, with all clocks pinned to `now`."""
    return json.dumps({"usage": {
        "rolling": {"status": statuses[0], "percent": rolling, "resetsAt": _iso(now + reset_in[0])},
        "weekly": {"status": statuses[1], "percent": weekly, "resetsAt": _iso(now + reset_in[1])},
        "monthly": {"status": statuses[2], "percent": monthly, "resetsAt": _iso(now + reset_in[2])},
    }})


class FakeFetch:
    """Stand-in for the HTTP transport.

    Callable as ``fetch(url, headers, timeout) -> (status, body)``, records
    every call in ``.calls`` so tests can assert the URL and the auth header,
    and can be told to fail a specific way.
    """

    def __init__(self, status: int = 200, body: str | None = None,
                 raises: Exception | None = None) -> None:
        self.status = status
        self.body = usage_body() if body is None else body
        self.raises = raises
        self.calls: list[dict] = []

    def __call__(self, url, headers, timeout):
        self.calls.append({"url": url, "headers": dict(headers), "timeout": timeout})
        if self.raises is not None:
            raise self.raises
        return self.status, self.body


@pytest.fixture
def fetch():
    """Default transport: HTTP 200 with the endpoint's real response shape."""
    return FakeFetch(200)


# ---------------------------------------------------------------------------
# happy path: the live endpoint
# ---------------------------------------------------------------------------

def test_api_payload_matches_spec_shape(oc_db, auth_file, fetch):
    body = usage_body(rolling=37, weekly=22, monthly=3,
                      reset_in=(134 * 60, 5040 * 60, 20354 * 60))
    payload = collect(now=NOW, db_path=oc_db, auth_path=auth_file,
                      fetch=FakeFetch(200, body))
    assert payload["k"] == "oc" and payload["ok"] is True and payload["src"] == "api"
    # SPEC §8's example payload, to the digit.
    assert (payload["p5"], payload["r5"]) == (37, 134)
    assert (payload["pw"], payload["rw"]) == (22, 5040)
    assert (payload["pm"], payload["rm"]) == (3, 20354)
    assert payload["st"] == "ok"


def test_api_request_uses_trailing_slash_and_bearer(oc_db, auth_file):
    fetch = FakeFetch(200)
    collect(now=NOW, db_path=oc_db, auth_path=auth_file, fetch=fetch)
    assert len(fetch.calls) == 1
    call = fetch.calls[0]
    # research/09: without the trailing slash the server answers 401.
    assert call["url"] == USAGE_URL == "https://opencode.ai/zen/go/v1/usage/"
    assert call["headers"]["Authorization"] == f"Bearer {SECRET}"
    assert call["timeout"] == 10.0


def test_rolling_zero_means_no_active_window(oc_db, auth_file):
    # The server's idle placeholder: percent 0 with resetsAt = now + 5h.
    payload = collect(now=NOW, db_path=oc_db, auth_path=auth_file, fetch=FakeFetch(200))
    assert payload["src"] == "api"
    assert payload["p5"] == 0
    assert payload["r5"] == -1  # not a 300-minute countdown


def test_any_non_ok_status_reports_limited(oc_db, auth_file):
    for i in (0, 1, 2):
        statuses = ["ok", "ok", "ok"]
        statuses[i] = "rate-limited"
        body = usage_body(rolling=100, statuses=tuple(statuses))
        payload = collect(now=NOW, db_path=oc_db, auth_path=auth_file, fetch=FakeFetch(200, body))
        assert payload["st"] == "limited", f"status[{i}] must trip st"


def test_all_ok_statuses_stay_ok(oc_db, auth_file):
    payload = collect(now=NOW, db_path=oc_db, auth_path=auth_file, fetch=FakeFetch(200))
    assert payload["st"] == "ok"


def test_percent_is_clamped(oc_db, auth_file):
    body = usage_body(rolling=140, weekly=-5, monthly=100)
    payload = collect(now=NOW, db_path=oc_db, auth_path=auth_file, fetch=FakeFetch(200, body))
    assert (payload["p5"], payload["pw"], payload["pm"]) == (100, 0, 100)


# ---------------------------------------------------------------------------
# local activity, on every source
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("src", ["api", "est", "none"])
def test_local_activity_fields(oc_db, auth_file, src):
    transport = FakeFetch(200) if src != "est" else FakeFetch(500, "")
    if src == "none":
        auth_file.write_text(json.dumps({"opencode-go": {"type": "api", "key": ""}}))
    payload = collect(now=NOW, db_path=oc_db, auth_path=auth_file, fetch=transport)
    assert payload["src"] == src
    # 7d tokens: 51.2M + 20.6M + 10.0M + 7.7M (free model counts toward the
    # total) = 89.5M; the 10-day-old 50M row is outside the window.
    assert payload["t7"] == 89500
    # Top model: deepseek-v4.1-flash, 81.8M / 89.5M = 91.4 % -> 91
    assert payload["m"] == "ds-v4.1-flash"
    assert payload["ms"] == 91
    # One session updated within 10 min, agent of the most recent one.
    assert payload["a"] == 1
    assert payload["ag"] == "build"
    # Last assistant step was 1h ago; the user turn is not a step.
    assert payload["la"] == 3600


def test_unreadable_database_still_yields_a_payload(tmp_path, auth_file):
    db = tmp_path / "opencode.db"
    db.write_text("not a database")
    payload = collect(now=NOW, db_path=db, auth_path=auth_file, fetch=FakeFetch(200))
    assert payload["src"] == "api"
    assert (payload["t7"], payload["a"], payload["la"]) == (0, 0, 0)
    assert payload["m"] == ""


def test_missing_database_but_valid_key_still_reports_api(tmp_path, auth_file):
    payload = collect(now=NOW, db_path=tmp_path / "nope.db", auth_path=auth_file,
                      fetch=FakeFetch(200))
    assert payload["src"] == "api" and payload["t7"] == 0


# ---------------------------------------------------------------------------
# fallback: local estimate
# ---------------------------------------------------------------------------

def test_http_error_falls_back_to_estimate(oc_db, auth_file, fetch):
    payload = collect(now=NOW, db_path=oc_db, auth_path=auth_file, fetch=FakeFetch(503, ""))
    assert payload["src"] == "est"
    # 5h window starts at the first step after the >=5h idle gap (NOW-2h) and
    # ends NOW+3h. Spend in it: $0.60 + $0.60 on a $60 model -> 0.02 of the
    # monthly limit, /0.20 of the 5h allowance -> 10 %.
    assert payload["p5"] == 10
    assert payload["r5"] == 180
    # Week and month cover all three paid steps: $1.80 / $60 = 0.03
    # -> /0.5 = 6 %, /1.0 = 3 %.
    assert payload["pw"] == 6
    assert payload["pm"] == 3
    # Resets: next Monday 00:00 UTC and the 1st of next month, both UTC.
    assert payload["rw"] == 4 * 24 * 60 + 12 * 60   # Mon 2026-10-05 00:00
    assert payload["rm"] == 12 * 60                 # Thu 2026-10-01 00:00
    assert payload["st"] == "ok"


def test_estimate_skips_free_models(oc_db, auth_file, fetch):
    """space-bunny-free costs $0 and must not move any percentage."""
    with_free = collect(now=NOW, db_path=oc_db, auth_path=auth_file, fetch=FakeFetch(500, ""))
    # Same DB with the free steps removed: the paid numbers must not move.
    conn = sqlite3.connect(oc_db)
    conn.execute("delete from session_message where id in ('msg_0', 'msg_4')")
    conn.commit()
    conn.close()
    without_free = collect(now=NOW, db_path=oc_db, auth_path=auth_file, fetch=FakeFetch(500, ""))
    for key in ("p5", "pw", "pm"):
        assert with_free[key] == without_free[key]


def test_estimate_zero_when_the_5h_window_expired(tmp_path, auth_file):
    """No step after the last >=5h gap -> no active 5h window (p5 0, r5 -1)."""
    db = tmp_path / "old.db"
    conn = sqlite3.connect(db)
    conn.execute("create table session_v2 (id text primary key, agent text, model text, "
                 "time_created integer not null, time_updated integer not null)")
    conn.execute("create table session_message (id text primary key, session_id text not null, "
                 "type text not null, seq integer not null, time_created integer not null, "
                 "time_updated integer not null, data text not null)")
    old = int((NOW - 9 * 3600) * 1000)  # 9h ago: window ended 4h ago
    conn.execute("insert into session_message values ('m1','s','assistant',0,?,?,?)",
                 (old, old, _msg(_tokens(1_000, 1_000), 0.60)))
    conn.commit()
    conn.close()
    payload = collect(now=NOW, db_path=db, auth_path=auth_file, fetch=FakeFetch(500, ""))
    assert payload["src"] == "est"
    assert payload["p5"] == 0 and payload["r5"] == -1
    # The month still counts that spend: $0.60 / $60 = 1 %.
    assert payload["pm"] == 1


def test_estimate_honours_the_monthly_model_limit(tmp_path, auth_file):
    """Same $0.60 against a $15 model burns 4x the quota of a $60 model."""
    db = tmp_path / "kimi.db"
    conn = sqlite3.connect(db)
    conn.execute("create table session_v2 (id text primary key, agent text, model text, "
                 "time_created integer not null, time_updated integer not null)")
    conn.execute("create table session_message (id text primary key, session_id text not null, "
                 "type text not null, seq integer not null, time_created integer not null, "
                 "time_updated integer not null, data text not null)")
    ts = int((NOW - 3600) * 1000)
    conn.execute("insert into session_message values ('m1','s','assistant',0,?,?,?)",
                 (ts, ts, _msg(_tokens(1_000, 1_000), 0.60, model="kimi-k3")))
    conn.commit()
    conn.close()
    payload = collect(now=NOW, db_path=db, auth_path=auth_file, fetch=FakeFetch(500, ""))
    # 0.60 / 15 = 0.04 of the monthly limit; the 5h allowance is 20 % of it.
    assert payload["p5"] == 20
    assert payload["pm"] == 4


def test_estimate_clamps_to_100(tmp_path, auth_file):
    db = tmp_path / "spendy.db"
    conn = sqlite3.connect(db)
    conn.execute("create table session_v2 (id text primary key, agent text, model text, "
                 "time_created integer not null, time_updated integer not null)")
    conn.execute("create table session_message (id text primary key, session_id text not null, "
                 "type text not null, seq integer not null, time_created integer not null, "
                 "time_updated integer not null, data text not null)")
    ts = int((NOW - 3600) * 1000)
    conn.execute("insert into session_message values ('m1','s','assistant',0,?,?,?)",
                 (ts, ts, _msg(_tokens(1_000, 1_000), 500.0, model="kimi-k3")))
    conn.commit()
    conn.close()
    payload = collect(now=NOW, db_path=db, auth_path=auth_file, fetch=FakeFetch(500, ""))
    assert payload["p5"] == 100 and payload["pw"] == 100 and payload["pm"] == 100
    assert payload["st"] == "limited"


def test_transport_exception_falls_back_to_estimate(oc_db, auth_file):
    transport = FakeFetch(raises=OSError("network unreachable"))
    payload = collect(now=NOW, db_path=oc_db, auth_path=auth_file, fetch=transport)
    assert payload["src"] == "est"


def test_malformed_body_falls_back_to_estimate(oc_db, auth_file, fetch):
    payload = collect(now=NOW, db_path=oc_db, auth_path=auth_file,
                      fetch=FakeFetch(200, "<html>nope</html>"))
    assert payload["src"] == "est"


# ---------------------------------------------------------------------------
# no key at all: consumption-only
# ---------------------------------------------------------------------------

def test_no_key_reports_none_source_and_skips_the_network(tmp_path, oc_db):
    auth = tmp_path / "auth.json"
    auth.write_text(json.dumps({"opencode-go": {"type": "api", "key": ""}}))
    transport = FakeFetch(200)
    payload = collect(now=NOW, db_path=oc_db, auth_path=auth, fetch=transport)
    assert payload["src"] == "none"
    assert transport.calls == []  # no key, no request
    # Consumption-only extras (SPEC §2.4).
    assert "tk" in payload and "cd" in payload and "c7" in payload


def test_no_key_consumption_numbers(tmp_path, oc_db, monkeypatch):
    """Today's tokens/cost and the 7d cost, with a local-midnight boundary."""
    monkeypatch.setenv("TZ", "UTC")
    time.tzset()
    try:
        auth = tmp_path / "auth.json"
        auth.write_text(json.dumps({"opencode-go": {"type": "api", "key": ""}}))
        payload = collect(now=NOW, db_path=oc_db, auth_path=auth)
    finally:
        time.tzset()
    # Today (UTC) holds the -2h and -1h steps: 20.6M + 10.0M = 30.6M tokens
    # and $0.60 + $0.60.
    assert payload["tk"] == 30600
    assert payload["cd"] == 1.20
    # 7d cost is every paid step in the window: $1.80.
    assert payload["c7"] == 1.80


def test_missing_auth_file_is_no_key(oc_db, tmp_path):
    payload = collect(now=NOW, db_path=oc_db, auth_path=tmp_path / "absent.json",
                      fetch=FakeFetch(200))
    assert payload["src"] == "none"
    assert payload["c7"] == 1.8


def test_unreadable_auth_file_is_no_key(oc_db, tmp_path):
    auth = tmp_path / "auth.json"
    auth.write_text("{not json")
    payload = collect(now=NOW, db_path=oc_db, auth_path=auth, fetch=FakeFetch(200))
    assert payload["src"] == "none"


# ---------------------------------------------------------------------------
# not installed
# ---------------------------------------------------------------------------

def test_returns_none_when_opencode_is_not_installed(tmp_path):
    assert collect(now=NOW, db_path=tmp_path / "no.db",
                   auth_path=tmp_path / "no.json", fetch=FakeFetch(200)) is None
    assert FakeFetch(200).calls == []


# ---------------------------------------------------------------------------
# payload budget
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("model", ["deepseek-v4.1-flash", "some-extremely-long-model-name"])
@pytest.mark.parametrize("agent", ["build", "a-rather-long-agent-name"])
def test_payload_fits_the_240_byte_ble_budget(tmp_path, model, agent):
    """Worst case: the longest allowed names on the largest possible numbers,
    in the fattest mode (src "none" carries every base field plus tk/cd/c7)."""
    db = tmp_path / "fat.db"
    conn = sqlite3.connect(db)
    conn.execute("create table session_v2 (id text primary key, agent text, model text, "
                 "time_created integer not null, time_updated integer not null)")
    conn.execute("create table session_message (id text primary key, session_id text not null, "
                 "type text not null, seq integer not null, time_created integer not null, "
                 "time_updated integer not null, data text not null)")
    now_ms = int(NOW * 1000)
    for i in range(5):
        conn.execute("insert into session_v2 values (?,?,?,?,?)",
                     (f"s{i}", agent, None, now_ms, now_ms - 10_000))
    ts = now_ms - 60_000
    for i in range(3):
        conn.execute("insert into session_message values (?,?,?,?,?,?,?)",
                     (f"m{i}", "s0", "assistant", i, ts, ts,
                      _msg(_tokens(300_000_000, 0, 900_000_000), 987.65, model=model)))
    conn.commit()
    conn.close()
    auth = tmp_path / "auth.json"
    auth.write_text(json.dumps({"opencode-go": {"key": SECRET}}))

    for body, src in ((usage_body(rolling=100, weekly=100, monthly=100,
                                  reset_in=(300 * 60, 7 * 86400 * 60, 31 * 86400 * 60)),
                       "api"),
                      ("", "est")):
        transport = FakeFetch(200, body) if src == "api" else FakeFetch(500, "")
        payload = collect(now=NOW, db_path=db, auth_path=auth, fetch=transport)
        wire = json.dumps(payload, separators=(",", ":"))
        assert payload["src"] == src
        assert len(wire.encode()) <= PAYLOAD_MAX, f"{src}: {len(wire)} bytes -> {wire}"

    # The fattest mode is src "none": every base field plus the three extras.
    auth.write_text(json.dumps({"opencode-go": {"key": ""}}))
    payload = collect(now=NOW, db_path=db, auth_path=auth, fetch=FakeFetch(200))
    wire = json.dumps(payload, separators=(",", ":"))
    assert payload["src"] == "none"
    assert len(wire.encode()) <= PAYLOAD_MAX, f"none: {len(wire)} bytes -> {wire}"


def test_payload_is_compact_json(oc_db, auth_file, fetch):
    """No spaces: every byte counts against the 240-byte budget."""
    payload = collect(now=NOW, db_path=oc_db, auth_path=auth_file, fetch=FakeFetch(200))
    wire = json.dumps(payload, separators=(",", ":"))
    assert ", " not in wire and ": " not in wire
    assert wire.startswith('{"k":"oc"')


# ---------------------------------------------------------------------------
# the key is a credential, not a log line
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("status,body,raises", [
    (200, None, None),
    (200, "<html>not json</html>", None),
    (401, "", None),
    (500, "", None),
    (0, "", OSError("boom")),
    (0, "", RuntimeError(f"request failed with Authorization: Bearer {SECRET}")),
])
def test_key_never_appears_in_log_output(oc_db, auth_file, caplog, status, body, raises):
    """Every failure path, plus the happy path, must stay silent about the key.

    The last case is the important one: the transport raised an exception whose
    message quotes the header, and the collector must not log it.
    """
    with caplog.at_level(logging.DEBUG):
        payload = collect(now=NOW, db_path=oc_db, auth_path=auth_file,
                          fetch=FakeFetch(status, body, raises))
    assert payload is not None
    assert SECRET not in caplog.text
    assert "Authorization" not in caplog.text
    assert SECRET not in json.dumps(payload)
    if status != 200 or raises is not None:
        # The failures must still say something — just never anything secret.
        assert caplog.records, "a failure should log one generic line"


def test_daemon_logging_never_shows_the_key(oc_db, auth_file, caplog):
    """The daemon's own short line and its payload dump are key-free too."""
    with caplog.at_level(logging.DEBUG):
        payload = collect(now=NOW, db_path=oc_db, auth_path=auth_file, fetch=FakeFetch(401, ""))
    daemon_mod.log(f"OpenCode: {payload['src']} 5h {payload['p5']}%")
    assert SECRET not in caplog.text


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def test_model_limits_follow_the_go_table():
    assert model_monthly_limit("deepseek-v4.1-flash") == 60.0
    assert model_monthly_limit("deepseek-v4-flash") == 30.0
    assert model_monthly_limit("DeepSeek V4 Pro (Peak)") == 15.0
    assert model_monthly_limit("deepseek-v4-flash-vision-exp") == 15.0
    assert model_monthly_limit("qwen3.8-max") == 15.0
    assert model_monthly_limit("qwen3.8-flash") == 30.0
    assert model_monthly_limit("hy4-preview") == 30.0
    assert model_monthly_limit("kimi-k3") == 15.0
    assert model_monthly_limit("space-bunny-free") == 60.0
    # Unknown models assume the $60 tier (and stay approximate).
    assert model_monthly_limit("some-model-from-next-year") == 60.0


def test_short_model_name():
    assert short_model_name("deepseek-v4.1-flash") == "ds-v4.1-flash"
    assert short_model_name("opencode-go/deepseek-v4.1-flash#default") == "ds-v4.1-flash"
    assert short_model_name("space-bunny-free") == "space-bunny-fr"  # clipped to 14
    assert len(short_model_name("a-very-long-model-identifier")) <= 14


def test_map_api_usage_tolerates_a_partial_body():
    out = map_api_usage({"weekly": {"status": "ok", "percent": 10}}, now=NOW)
    assert out["pw"] == 10
    assert out["p5"] == 0 and out["r5"] == -1
    assert out["st"] == "ok"


def test_estimate_without_any_spend():
    stats = {"paid": [], "tokens_7d": 0, "cost_7d": 0.0, "tokens_today": 0,
             "cost_today": 0.0, "top_model": "", "top_share": 0, "active": 0,
             "agent": "", "last_step_ms": None}
    out = estimate_usage(stats, NOW)
    assert (out["p5"], out["pw"], out["pm"], out["st"]) == (0, 0, 0, "ok")
    assert out["r5"] == -1


def test_read_local_survives_a_foreign_schema(tmp_path):
    from daemon.opencode_collector import read_local
    db = tmp_path / "other.db"
    conn = sqlite3.connect(db)
    conn.execute("create table unrelated (x integer)")
    conn.commit()
    conn.close()
    stats = read_local(db, NOW)
    assert stats["tokens_7d"] == 0 and stats["paid"] == [] and stats["active"] == 0


# ---------------------------------------------------------------------------
# daemon wiring
# ---------------------------------------------------------------------------

def test_read_opencode_setting_defaults_to_off(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    monkeypatch.setattr(daemon_mod, "CONFIG_FILE", cfg)
    assert daemon_mod.read_opencode_setting() == "off"          # absent file
    cfg.write_text("chime = on\n")
    assert daemon_mod.read_opencode_setting() == "off"          # key absent
    cfg.write_text("opencode = ON   # the OpenCode screens\n")
    assert daemon_mod.read_opencode_setting() == "on"
    cfg.write_text("opencode = maybe\n")
    assert daemon_mod.read_opencode_setting() == "off"          # bad value


def test_read_opencode_paths_are_optional(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    monkeypatch.setattr(daemon_mod, "CONFIG_FILE", cfg)
    assert daemon_mod.read_opencode_paths() == (None, None)
    cfg.write_text("opencode_db = ~/db/opencode.db\nopencode_auth = ~/db/auth.json\n")
    assert daemon_mod.read_opencode_paths() == (Path.home() / "db/opencode.db",
                                               Path.home() / "db/auth.json")


def test_config_example_documents_the_option():
    text = (Path(daemon_mod.__file__).parent / "config.example").read_text()
    assert "opencode = off" in text
    assert "opencode_db" in text and "opencode_auth" in text


class _FakeClient:
    """Minimal BleakClient stand-in: records writes and drops the link after N."""

    def __init__(self, target, stop_after: int) -> None:
        self.address = "FAKE"
        self.is_connected = True
        self.writes: list[tuple[float, str]] = []
        self._stop_after = stop_after

    async def connect(self) -> bool:
        return True

    async def start_notify(self, *_a, **_k) -> None:
        return None

    async def write_gatt_char(self, _char, data, response=True) -> bool:
        self.writes.append((time.monotonic(), data.decode()))
        if len(self.writes) >= self._stop_after:
            self.is_connected = False
        return True

    async def disconnect(self) -> None:
        self.is_connected = False


def test_opencode_payload_follows_the_claude_one_by_250ms(tmp_path, monkeypatch):
    """SPEC §8: same RX characteristic, so the device must see Claude first."""
    import asyncio

    cfg = tmp_path / "config"
    cfg.write_text("opencode = on\n")
    monkeypatch.setattr(daemon_mod, "CONFIG_FILE", cfg)
    monkeypatch.setattr(daemon_mod, "POLL_INTERVAL", 0)
    monkeypatch.setattr(daemon_mod, "OPENCODE_INTERVAL", 0)
    monkeypatch.setattr(daemon_mod, "TICK", 0.01)
    monkeypatch.setattr(daemon_mod, "OPENCODE_WRITE_DELAY", 0.25)

    client = _FakeClient("FAKE", stop_after=2)
    monkeypatch.setattr(daemon_mod, "BleakClient", lambda target: client)

    oc_payload = {"k": "oc", "ok": True, "src": "api", "p5": 37, "r5": 134, "pw": 22,
                  "rw": 5040, "pm": 3, "rm": 20354, "st": "ok", "t7": 106969,
                  "m": "ds-v4.1-flash", "ms": 93, "a": 1, "ag": "build", "la": 35}
    monkeypatch.setattr(daemon_mod, "collect", lambda *a, **k: oc_payload)

    async def fake_poll_active():
        return {"s": 45, "sr": 120, "w": 28, "wr": 7200, "st": "allowed", "ok": True}, False

    monkeypatch.setattr(daemon_mod, "poll_active", fake_poll_active)

    async def run():
        return await daemon_mod.connect_and_run("FAKE", asyncio.Event())

    assert asyncio.run(run()) is True
    assert len(client.writes) == 2
    (t1, first), (t2, second) = client.writes
    assert '"k":"oc"' not in first          # Claude first
    assert json.loads(second) == oc_payload  # then OpenCode
    assert 0.25 <= t2 - t1 < 1.0
