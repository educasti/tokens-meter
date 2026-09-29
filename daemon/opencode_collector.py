#!/usr/bin/env python3
"""OpenCode collector — one BLE payload for the two OpenCode screens.

Speaks SPEC §8: a ``{"k":"oc", ...}`` dict on the same RX characteristic
Claude already uses, distinguished by the ``k`` key. Everything here is
synchronous — the daemon calls it through ``asyncio.to_thread`` so its one
10 s HTTP call never stalls the BLE loop.

Three sources, in order of trust:

1. ``GET https://opencode.ai/zen/go/v1/usage/`` with the ``opencode-go`` key
   from ``auth.json`` → exact percents and reset times (``src:"api"``).
   The trailing slash is REQUIRED; the endpoint 401s without it (research/09).
2. ``opencode.db``, opened read-only (``file:...?mode=ro``, so the WAL is
   still readable while OpenCode holds the DB). It always supplies the local
   activity numbers, and it is the *estimated* fallback (``src:"est"``,
   research/07) when the endpoint can't answer.
3. No key at all → ``src:"none"``, consumption-only (SPEC §2.4).

The Go API key is a credential. It is never logged, never printed and never
part of a log message or exception: every failure path emits one fixed,
generic line instead (see ``test_key_never_logged``).
"""

from __future__ import annotations

import datetime
import json
import logging
import re
import sqlite3
import time
import urllib.error
import urllib.request
from pathlib import Path

logger = logging.getLogger(__name__)

DEFAULT_DATA_DIR = Path.home() / ".local" / "share" / "opencode"
DEFAULT_DB_PATH = DEFAULT_DATA_DIR / "opencode.db"
DEFAULT_AUTH_PATH = DEFAULT_DATA_DIR / "auth.json"

# Trailing slash REQUIRED — without it the server answers 401 (research/09).
USAGE_URL = "https://opencode.ai/zen/go/v1/usage/"
HTTP_TIMEOUT = 10.0
GO_PROVIDER = "opencode-go"

DAY = 86400
FIVE_H = 5 * 3600
# Window sizes as a fraction of each model's monthly limit (research/07):
# 5h = 20 %, weekly = 50 %, monthly = 100 %.
FIVE_H_FRACTION = 0.20
WEEK_FRACTION = 0.50
MONTH_FRACTION = 1.00
# How far back the local numbers reach.
ACTIVITY_DAYS = 7
# Session updated within this many seconds counts as "active" (SPEC §2.3).
ACTIVE_WINDOW_S = 600
# The fixed-start 5h window is the first step after the last idle gap of >= 5h,
# so the gap scan needs a bit of history; 31 days is a bounded compromise
# (a window older than that is expired anyway, and the answer is 0 %).
GAP_SCAN_S = 31 * DAY
# Display-name budget for the top model and the agent, keeping the payload
# inside the 240-byte BLE limit.
NAME_MAX = 14
PAYLOAD_MAX = 240


# --- per-model monthly limits (OpenCode Go plan, research/07) ---------------
#
# There is no single dollar cap: every model carries its own monthly limit, and
# a step burns ``cost / that model's limit`` of the quota. So the local
# estimate can only be computed with the table below.
#
# Keys are substrings of the *normalized* model id (lowercased, non-alphanumerics
# folded to "-"). The first match wins, so the more specific ids come first:
# "deepseek-v4-1-flash" must beat "deepseek-v4", and "deepseek-v4-flash-vision"
# must beat "deepseek-v4-flash".
#
# Free models (Space Bunny Free, LongCat 2.5 Preview) are not listed: they cost
# $0, so the ``cost > 0`` filter skips them exactly as the server does
# ("if you reach the usage limit you can continue using the free models").
MODEL_LIMITS: tuple[tuple[str, float], ...] = (
    ("deepseek-v4-flash-vision", 15.0),
    ("deepseek-v4-pro", 15.0),
    ("deepseek-v4-flash", 30.0),
    ("deepseek-v4-1-flash", 60.0),
    ("deepseek-v4-1", 60.0),
    ("deepseek-v4", 60.0),
    ("qwen3-8-max", 15.0),
    ("qwen3-8-flash", 30.0),
    ("qwen3-7-plus", 60.0),
    ("glm-5-3-flash", 60.0),
    ("glm-5-3", 15.0),
    ("glm-5-2", 60.0),
    ("kimi-k3", 15.0),
    ("kimi-k2-7", 60.0),
    ("kimi-k2-6", 60.0),
    ("mimo-v2-6-pro", 15.0),
    ("mimo-v2-6-flash", 60.0),
    ("mimo-v2-5-pro", 15.0),
    ("mimo-v2-5", 60.0),
    ("minimax-m3", 60.0),
    ("minimax-m2", 60.0),
    ("muse-spark", 60.0),
    ("longcat-2-0", 60.0),
    ("longcat-2-5", 60.0),
    ("hy4", 30.0),
    ("hy3", 60.0),
    ("grok-4", 15.0),
    ("gpt-6-luna", 15.0),
    ("gpt-5-6-luna", 15.0),
)
# Unknown model: assume the $60 tier and stay approximate (research/07).
DEFAULT_MODEL_LIMIT = 60.0


# ---------------------------------------------------------------------------
# small parsing helpers
# ---------------------------------------------------------------------------

def _as_int(value) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def _as_float(value) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def _as_text(value) -> str:
    return value if isinstance(value, str) else ""


def _normalize_model(name: str) -> str:
    """Fold a model id to a comparable key: "DeepSeek V4.1 Flash" -> "deepseek-v4-1-flash"."""
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def model_monthly_limit(model_id: str) -> float:
    """Go-plan monthly USD limit for a model id; ``DEFAULT_MODEL_LIMIT`` when unknown."""
    norm = _normalize_model(model_id)
    for needle, limit in MODEL_LIMITS:
        if needle in norm:
            return limit
    return DEFAULT_MODEL_LIMIT


def short_model_name(raw: str) -> str:
    """Display name for the top model: no provider, ASCII, <= 14 chars.

    ``opencode-go/deepseek-v4.1-flash#default`` -> ``ds-v4.1-flash`` — the
    "deepseek-" prefix is the only one dropped; the version is what tells
    Flash from Pro on screen.
    """
    name = _as_text(raw).split("/", 1)[-1].split("#", 1)[0].strip()
    if name.startswith("deepseek-"):
        name = "ds-" + name[len("deepseek-"):]
    name = name.encode("ascii", "ignore").decode("ascii")
    name = re.sub(r"[\s_/]+", "-", name).strip("-")
    return name[:NAME_MAX]


def short_text(value) -> str:
    """ASCII, <= 14 chars — used for the agent name."""
    text = _as_text(value).encode("ascii", "ignore").decode("ascii").strip()
    return re.sub(r"[\s_]+", "-", text)[:NAME_MAX]


def _iso_to_epoch(value) -> float | None:
    """Parse an ISO-8601 timestamp (``...Z`` included) to epoch seconds."""
    text = _as_text(value)
    if not text:
        return None
    if text.endswith(("Z", "z")):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=datetime.timezone.utc)
    return parsed.timestamp()


def _minutes_until(epoch: float | None, now: float) -> int:
    if epoch is None:
        return 0
    return max(0, int(round((epoch - now) / 60.0)))


def _clamp_pct(value: float) -> int:
    return max(0, min(100, int(round(value))))


def _local_midnight_epoch_ms(now: float) -> int:
    """Local midnight (the "today" boundary for ``src:"none"``), in epoch ms."""
    lt = time.localtime(now)
    return int(time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, 0, 0, 0, 0, 0, -1)) * 1000)


def _utc_window(now: float) -> tuple[datetime.datetime, datetime.datetime]:
    """(start, end) of the current UTC week: Monday 00:00 UTC -> next Monday."""
    utc = datetime.datetime.fromtimestamp(now, datetime.timezone.utc)
    start = (utc - datetime.timedelta(days=utc.weekday())).replace(
        hour=0, minute=0, second=0, microsecond=0
    )
    return start, start + datetime.timedelta(days=7)


def _utc_month(now: float) -> tuple[datetime.datetime, datetime.datetime]:
    """(start, end) of the current UTC month.

    Approximate stand-in for the server's subscription anniversary, which the
    device can't know locally (SPEC §2.2 / research/07).
    """
    utc = datetime.datetime.fromtimestamp(now, datetime.timezone.utc)
    start = utc.replace(day=1, hour=0, minute=0, second=0, microsecond=0)
    end = start.replace(year=start.year + 1, month=1) if start.month == 12 else start.replace(
        month=start.month + 1
    )
    return start, end


# ---------------------------------------------------------------------------
# OpenCode Go key + official usage endpoint
# ---------------------------------------------------------------------------

def read_go_key(auth_path: Path) -> str | None:
    """The ``opencode-go`` key from ``auth.json``, or None.

    Shape: ``{"opencode-go": {"type": "api", "key": "sk-..."}}``. A missing
    file, unreadable file, wrong shape or blank key all read as "no key".
    """
    try:
        data = json.loads(auth_path.read_text())
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict):
        return None
    entry = data.get(GO_PROVIDER)
    if not isinstance(entry, dict):
        return None
    key = entry.get("key")
    if isinstance(key, str) and key.strip():
        return key.strip()
    return None


def _urllib_fetch(url: str, headers: dict, timeout: float) -> tuple[int, str]:
    """Default transport. Returns ``(status, body)``; HTTP errors are statuses,
    everything else (DNS, TLS, timeout) raises and is logged generically."""
    request = urllib.request.Request(url, headers=dict(headers), method="GET")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        # 401 = key invalid/re-auth, 403 = no Go subscription (research/09).
        # The body can echo the request, so it is dropped, not logged.
        return e.code, ""


def fetch_usage(key: str, fetch=None) -> dict | None:
    """One GET against the Go usage endpoint; the ``usage`` object, or None.

    ``fetch(url, headers, timeout) -> (status, body_text)`` is injectable so
    tests never touch the network. Any failure (transport, non-200, malformed
    body) returns None and the caller falls back to the local estimate — the
    key is never part of a log line, and exception text is deliberately
    dropped rather than logged.
    """
    transport = fetch or _urllib_fetch
    headers = {
        "Authorization": f"Bearer {key}",
        "Accept": "application/json",
        "User-Agent": "clawdmeter-daemon",
    }
    try:
        status, body = transport(USAGE_URL, headers, HTTP_TIMEOUT)
    except Exception:  # noqa: BLE001 — the message may quote the URL/headers
        logger.warning("OpenCode Go usage request failed; using the local estimate")
        return None
    if status != 200:
        logger.warning("OpenCode Go usage endpoint returned HTTP %s", status)
        return None
    if isinstance(body, (bytes, bytearray)):
        body = body.decode("utf-8", "replace")
    try:
        usage = json.loads(body)["usage"]
    except (ValueError, TypeError, KeyError):
        logger.warning("OpenCode Go usage response was not the expected shape")
        return None
    if not isinstance(usage, dict):
        logger.warning("OpenCode Go usage response was not the expected shape")
        return None
    return usage


# ---------------------------------------------------------------------------
# local database
# ---------------------------------------------------------------------------

def _open_readonly(db_path: Path) -> sqlite3.Connection:
    """Open the OpenCode DB read-only, WAL included (``?mode=ro``, uri=True)."""
    return sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)


def _sum_tokens(tokens) -> int:
    """Total tokens of one step, cache included (research/01)."""
    if not isinstance(tokens, dict):
        return 0
    total = _as_int(tokens.get("input")) + _as_int(tokens.get("output"))
    total += _as_int(tokens.get("reasoning"))
    cache = tokens.get("cache")
    if isinstance(cache, dict):
        total += _as_int(cache.get("read")) + _as_int(cache.get("write"))
    return total


def _model_fields(model) -> tuple[str, str]:
    if not isinstance(model, dict):
        return "", ""
    return _as_text(model.get("id")), _as_text(model.get("providerID"))


def read_local(db_path: Path, now: float) -> dict:
    """Everything the payload needs from ``opencode.db``.

    Always returns a dict; a missing/unreadable DB or a schema we don't know
    yields zeros rather than an exception, because a usage screen with no
    activity numbers still beats no payload at all.
    """
    stats: dict = {
        "tokens_7d": 0,
        "cost_7d": 0.0,
        "tokens_today": 0,
        "cost_today": 0.0,
        "top_model": "",
        "top_share": 0,
        "active": 0,
        "agent": "",
        "last_step_ms": None,
        # (timestamp_ms, cost_usd, model_id) for the estimate: paid Go steps.
        "paid": [],
    }
    try:
        conn = _open_readonly(db_path)
    except (sqlite3.Error, OSError):
        logger.warning("OpenCode database could not be opened; reporting zeros")
        return stats
    # Independently, so a schema we don't know for sessions (v1 leftovers, say)
    # can't cost us the token numbers too.
    try:
        _read_sessions(conn, now, stats)
    except (sqlite3.Error, ValueError):
        logger.warning("OpenCode session table could not be read; reporting 0 active")
    try:
        _read_messages(conn, now, stats)
    except (sqlite3.Error, ValueError):
        logger.warning("OpenCode message table could not be read; reporting zeros")
    finally:
        conn.close()
    return stats


def _read_sessions(conn: sqlite3.Connection, now: float, stats: dict) -> None:
    """Active sessions (updated in the last 10 min) and the newest one's agent."""
    cutoff = int((now - ACTIVE_WINDOW_S) * 1000)
    rows = conn.execute(
        "select agent, time_updated from session_v2 "
        "where time_updated >= ? order by time_updated desc",
        (cutoff,),
    ).fetchall()
    stats["active"] = len(rows)
    stats["agent"] = short_text(rows[0][0]) if rows else ""


def _read_messages(conn: sqlite3.Connection, now: float, stats: dict) -> None:
    """One pass over recent assistant steps: 7d tokens/cost, today's, the top
    model, and the paid Go steps the local estimate needs."""
    now_ms = int(now * 1000)
    week_cutoff = now_ms - ACTIVITY_DAYS * DAY * 1000
    midnight = _local_midnight_epoch_ms(now)
    per_model: dict[str, int] = {}
    last_step = stats["last_step_ms"]
    paid: list[tuple[int, float, str]] = stats["paid"]

    rows = conn.execute(
        "select time_created, data from session_message "
        "where type = 'assistant' and time_created >= ? order by time_created",
        (now_ms - GAP_SCAN_S * 1000,),
    )
    for time_created, blob in rows:
        ts = _as_int(time_created)
        try:
            data = json.loads(blob) if isinstance(blob, str) else None
        except ValueError:
            continue
        if not isinstance(data, dict):
            continue
        if last_step is None or ts > last_step:
            last_step = ts

        model_id, provider = _model_fields(data.get("model"))
        cost = _as_float(data.get("cost"))
        # Free models cost $0 and the server never counts them, so they drop
        # out here too (research/07).
        if provider == GO_PROVIDER and cost > 0.0:
            paid.append((ts, cost, model_id))

        if ts < week_cutoff:
            continue
        tokens = _sum_tokens(data.get("tokens"))
        stats["tokens_7d"] += tokens
        stats["cost_7d"] += cost
        if model_id:
            per_model[model_id] = per_model.get(model_id, 0) + tokens
        if ts >= midnight:
            stats["tokens_today"] += tokens
            stats["cost_today"] += cost

    # The true last step may predate the gap-scan window, so ask the index
    # rather than trusting the scan.
    try:
        row = conn.execute(
            "select max(time_created) from session_message where type = 'assistant'"
        ).fetchone()
        newest = _as_int(row[0]) if row else 0
        if newest > 0:
            last_step = newest if last_step is None else max(last_step, newest)
    except sqlite3.Error:
        pass

    stats["last_step_ms"] = last_step
    if stats["tokens_7d"] > 0 and per_model:
        top_model, top_tokens = max(per_model.items(), key=lambda kv: kv[1])
        stats["top_model"] = short_model_name(top_model)
        stats["top_share"] = max(0, min(100, int(round(top_tokens * 100.0 / stats["tokens_7d"]))))


# ---------------------------------------------------------------------------
# estimation from the local database (research/07, "What the device should compute")
# ---------------------------------------------------------------------------

def _window_start_5h(paid: list[tuple[int, float, str]]) -> int | None:
    """Start of the current fixed 5h window: the first step after the last idle
    gap of >= 5h. The window is fixed-start, not sliding — it never resets
    while requests keep arriving inside it (research/07, Q2)."""
    t0: int | None = None
    previous: int | None = None
    for ts, _cost, _model in paid:
        if previous is not None and (ts - previous) >= FIVE_H * 1000:
            t0 = ts
        previous = ts
    if t0 is None:
        t0 = paid[0][0] if paid else None
    return t0


def _spend_fraction(paid: list[tuple[int, float, str]], since_ms: int) -> float:
    """``sum(cost_step / model_limit)`` over the steps at/after ``since_ms`` —
    the base limit cancels out, so the table is all that's needed."""
    total = 0.0
    for ts, cost, model_id in paid:
        if ts < since_ms:
            continue
        total += cost / model_monthly_limit(model_id)
    return total


def estimate_usage(stats: dict, now: float) -> dict:
    """Local estimate of the three Go windows. Approximate by construction
    (this machine only, no peak pricing, calendar month) — the device labels
    it ``est.``"""
    paid = stats["paid"]
    p5 = 0
    r5 = -1
    week_start, week_end = _utc_window(now)
    month_start, month_end = _utc_month(now)
    rw = _minutes_until(week_end.timestamp(), now)
    rm = _minutes_until(month_end.timestamp(), now)

    t0 = _window_start_5h(paid)
    if t0 is not None and now * 1000 <= t0 + FIVE_H * 1000:
        p5 = _clamp_pct(100.0 * _spend_fraction(paid, t0) / FIVE_H_FRACTION)
        r5 = _minutes_until((t0 + FIVE_H * 1000) / 1000.0, now)

    pw = _clamp_pct(
        100.0 * _spend_fraction(paid, int(week_start.timestamp() * 1000)) / WEEK_FRACTION
    )
    pm = _clamp_pct(
        100.0 * _spend_fraction(paid, int(month_start.timestamp() * 1000)) / MONTH_FRACTION
    )
    return {
        "p5": p5,
        "r5": r5,
        "pw": pw,
        "rw": rw,
        "pm": pm,
        "rm": rm,
        # The server's "limit reached" signal has no local equivalent, so the
        # estimate reports it when a window's allowance is fully spent.
        "st": "limited" if max(p5, pw, pm) >= 100 else "ok",
    }


def map_api_usage(usage: dict, now: float) -> dict:
    """Map the endpoint's ``usage`` object onto the payload's window fields.

    ``rolling.percent == 0`` is the server's idle placeholder: its
    ``resetsAt`` is "now + 5h", not a real window end, so it is reported as
    ``r5 = -1`` (SPEC §8: no active window) rather than a countdown.
    """
    out: dict = {}
    statuses: list[str] = []
    for src_key, pct_key, reset_key in (
        ("rolling", "p5", "r5"),
        ("weekly", "pw", "rw"),
        ("monthly", "pm", "rm"),
    ):
        window = usage.get(src_key)
        window = window if isinstance(window, dict) else {}
        percent = _clamp_pct(_as_float(window.get("percent")))
        out[pct_key] = percent
        if src_key == "rolling" and percent == 0:
            out[reset_key] = -1
        else:
            out[reset_key] = _minutes_until(_iso_to_epoch(window.get("resetsAt")), now)
        statuses.append(_as_text(window.get("status")))
    out["st"] = "limited" if any(s and s != "ok" for s in statuses) else "ok"
    return out


# ---------------------------------------------------------------------------
# payload
# ---------------------------------------------------------------------------

def _activity_fields(stats: dict, now: float) -> dict:
    last_step = stats["last_step_ms"]
    la = 0 if last_step is None else max(0, int((now * 1000 - last_step) / 1000))
    return {
        "t7": int(round(stats["tokens_7d"] / 1000.0)),
        "m": stats["top_model"],
        "ms": stats["top_share"],
        "a": stats["active"],
        "ag": stats["agent"],
        "la": la,
    }


def _payload(src: str, windows: dict, stats: dict, now: float, extra: dict | None = None) -> dict:
    """Assemble the SPEC §8 payload in wire order.

    ``src:"none"`` keeps the same base fields (the firmware defaults a missing
    one to 0) and adds ``tk``/``cd``/``c7`` for its consumption-only heroes.
    """
    payload = {
        "k": "oc",
        "ok": True,
        "src": src,
        "p5": windows.get("p5", 0),
        "r5": windows.get("r5", -1),
        "pw": windows.get("pw", 0),
        "rw": windows.get("rw", 0),
        "pm": windows.get("pm", 0),
        "rm": windows.get("rm", 0),
        "st": windows.get("st", "ok"),
    }
    payload.update(_activity_fields(stats, now))
    if extra:
        payload.update(extra)
    return payload


def collect(
    now: float | None = None,
    db_path: Path | str | None = None,
    auth_path: Path | str | None = None,
    fetch=None,
) -> dict | None:
    """Build one OpenCode BLE payload, or None when OpenCode isn't installed.

    ``fetch(url, headers, timeout) -> (status, body_text)`` is the injection
    point for tests; it defaults to the real urllib call.
    """
    now = time.time() if now is None else float(now)
    db = Path(db_path) if db_path is not None else DEFAULT_DB_PATH
    auth = Path(auth_path) if auth_path is not None else DEFAULT_AUTH_PATH

    if not db.exists() and not auth.exists():
        # Neither the database nor the credential file: no OpenCode here, so
        # the device must never open the OpenCode screens (SPEC §7).
        return None

    stats = read_local(db, now)
    key = read_go_key(auth)
    if not key:
        # No Go subscription: consumption-only mode (SPEC §2.4). No network
        # call is made at all in this mode.
        return _payload(
            "none",
            {},
            stats,
            now,
            {
                "tk": int(round(stats["tokens_today"] / 1000.0)),
                "cd": round(stats["cost_today"], 2),
                "c7": round(stats["cost_7d"], 2),
            },
        )

    usage = fetch_usage(key, fetch)
    if usage is None:
        return _payload("est", estimate_usage(stats, now), stats, now)
    return _payload("api", map_api_usage(usage, now), stats, now)


__all__ = [
    "PAYLOAD_MAX",
    "USAGE_URL",
    "collect",
    "estimate_usage",
    "map_api_usage",
    "model_monthly_limit",
    "read_go_key",
    "read_local",
    "short_model_name",
]
