#!/usr/bin/env python3
"""BVC portfolio collector — one BLE payload for the portfolio screen.

Speaks SPEC §6: a ``{"k":"pf", ...}`` dict on the same RX characteristic
Claude already uses, distinguished by the ``k`` key. Everything here is
synchronous — the daemon calls it through ``asyncio.to_thread`` so its one
HTTP call never stalls the BLE loop.

One source of prices, no credentials:

``GET https://query1.finance.yahoo.com/v7/finance/spark?symbols=A,B,C`` with
``range=1d&interval=1d``. No API key, no crumb, no cookie — just a browser
``User-Agent``. It is the only Yahoo batch endpoint that answers (research:
``v7/finance/quote`` is ``Unauthorized``, ``v8/finance/chart`` is ``Not Found``
on a comma list, ``v6/finance/quote`` is 404), so the 12 positions of a real
portfolio cost **one request per poll**, not 12. That is the whole reason this
collector is viable: an unofficial scraper asking for 12 × 60 requests a minute
gets throttled, one request a minute does not.

Two rules the data forced, both from the recorded verification:

1. **``chartPreviousClose`` is never read.** Yahoo's two endpoints disagree
   about the previous close; the spark one leaves it *unadjusted* (no dividend
   or split), which is >0.5 % wrong on 11 of 12 real positions and inverts the
   sign on GAMMA. Every day-change number here comes from
   ``regularMarketChangePercent`` / ``regularMarketChange``, which match the
   broker to the peso. When a previous close is needed it is *derived*,
   ``prev = price / (1 + pct/100)``.
2. **Every quote must say ``exchangeName == "BVC"``.** A mistyped ticker
   answers silently instead of failing: ``EPM`` without its suffix is Evolution
   Petroleum, a US company at USD 3.50, with no error anywhere. A quote from
   another exchange is dropped with a log line, never passed on.

Yahoo reports ``exchangeTimezoneName: America/New_York`` for BVC assets, which
is wrong metadata, so the Colombia time is computed by hand at a fixed UTC-5.
"""

from __future__ import annotations

import datetime
import json
import logging
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import NamedTuple

logger = logging.getLogger(__name__)

DEFAULT_PORTFOLIO_PATH = Path.home() / ".config" / "claude-usage-monitor" / "portfolio"

# The only batch endpoint Yahoo answers without a crumb (see the module
# docstring). One request for the whole portfolio.
SPARK_URL = "https://query1.finance.yahoo.com/v7/finance/spark"
SPARK_RANGE = "1d"
SPARK_INTERVAL = "1d"
HTTP_TIMEOUT = 10.0
# Yahoo rejects urllib's stock agent with a 429, but a *full browser* UA is
# rejected too — verified 2026-09-30: the long Chrome string gets a 429 on
# every request while the bare "Mozilla/5.0" gets a 200 on the same URL and
# the same headers. A minimal UA is both sufficient and the one that works.
USER_AGENT = "Mozilla/5.0"

# The one exchange this screen is about. Anything else is a wrong ticker.
BVC = "BVC"
# Suffix stripped for the wire only; the file and the BVC check keep the symbol
# verbatim, because auto-appending ".CL" is exactly what turns a typo into a
# plausible-looking number for a different company.
SUFFIX = ".CL"
# Symbols are Yahoo tickers, verbatim. 20 chars is generous (the longest ones
# on the exchange are 10) and keeps a pasted URL from parsing as a position.
SYMBOL_RE = re.compile(r"^[A-Z][A-Z0-9.\-]{0,19}$")

# Colombia has no DST, so a fixed offset is exact — and it avoids depending on
# a tz database for a screen that only ever needs one zone.
COT_OFFSET_S = -5 * 3600
# BVC session, Colombia time. Weekday only; the *holiday* half of the calendar
# never comes into it (see ``session_state``).
SESSION_OPEN = (10, 0)
SESSION_CLOSE = (15, 0)
SESSION_WEEKDAYS = (0, 1, 2, 3, 4)  # Mon..Fri

# Row selection: 2 up, 2 down (SPEC §6). Never a filler row.
ROWS_UP = 2
ROWS_DOWN = 2
# Firmware budget for the symbol column; a longer one is cut on screen with "…",
# never inside the payload.
SYM_MAX = 10

# Same hard BLE ceiling as the OpenCode payload (SPEC §6).
PAYLOAD_MAX = 240


class Position(NamedTuple):
    """One line of the portfolio file: a verbatim Yahoo symbol and a share count."""

    symbol: str
    qty: float


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


def _as_number(value) -> float | None:
    """A real number, or None. Unlike ``_as_float`` this does not turn a missing
    field into a zero — the difference matters for ``regularMarketChange``,
    where 0 is a legitimate quote and a missing key is not."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    number = float(value)
    return number if number == number and abs(number) != float("inf") else None


def wire_bytes(payload: dict) -> int:
    """Serialized size of a payload, in the compact form the daemon writes."""
    return len(json.dumps(payload, separators=(",", ":")).encode("utf-8"))


def _bogota(epoch: float) -> datetime.datetime:
    """Epoch seconds -> naive datetime in Colombia time.

    Hand-shifted by a fixed -5 h because Yahoo's ``exchangeTimezoneName`` for
    BVC assets says America/New_York, which is wrong by five hours (research).
    """
    return datetime.datetime.fromtimestamp(
        epoch + COT_OFFSET_S, datetime.timezone.utc
    ).replace(tzinfo=None)


def _bogota_stamp(epoch: float) -> str:
    """``MMDDhhmm`` in Colombia time — SPEC §6's ``t``."""
    return _bogota(epoch).strftime("%m%d%H%M")


# ---------------------------------------------------------------------------
# portfolio file
# ---------------------------------------------------------------------------

def _parse_qty(text: str) -> float | None:
    try:
        return float(text.replace(",", ""))
    except ValueError:
        return None


def read_positions(path: Path) -> tuple[list[Position], int]:
    """The positions file, plus how many warnings it produced.

    ``SIMBOLO.CL  cantidad`` per line, ``#`` starts a comment. Read on *every*
    poll, like the config, so an edit lands in ~60 s without a restart.

    A duplicate keeps the **first** occurrence: summing two lines would be
    guessing which one the user meant. Every rejected line is logged — silence
    here reads as "the numbers are right", and they are not.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        logger.warning("Portfolio file %s could not be read; no positions", path)
        return [], 0

    positions: list[Position] = []
    first_seen: dict[str, int] = {}
    warnings = 0
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 2:
            warnings += 1
            logger.warning(
                "portfolio:%d: expected 'SIMBOLO%s cantidad', got %r — ignored",
                lineno, SUFFIX, raw.strip(),
            )
            continue
        symbol, raw_qty = parts[0].upper(), parts[1]
        if not SYMBOL_RE.match(symbol):
            warnings += 1
            logger.warning(
                "portfolio:%d: %r is not a ticker — ignored", lineno, parts[0]
            )
            continue
        if symbol in first_seen:
            warnings += 1
            logger.warning(
                "portfolio:%d: %s already listed on line %d — keeping the first",
                lineno, symbol, first_seen[symbol],
            )
            continue
        qty = _parse_qty(raw_qty)
        if qty is None or qty <= 0:
            warnings += 1
            logger.warning(
                "portfolio:%d: %s has a non-numeric or non-positive quantity %r "
                "— ignored", lineno, symbol, raw_qty,
            )
            continue
        if not symbol.endswith(SUFFIX):
            # Not a warning for the payload: the BVC check below is what
            # actually keeps a bare "EPM" out of the numbers. This is here so
            # the log says why it got dropped, instead of the drop being a
            # mystery.
            logger.warning(
                "portfolio:%d: %s has no %s suffix — Yahoo may answer with a "
                "different exchange", lineno, symbol, SUFFIX,
            )
        first_seen[symbol] = lineno
        positions.append(Position(symbol, qty))
    return positions, warnings


# ---------------------------------------------------------------------------
# Yahoo spark
# ---------------------------------------------------------------------------

def _urllib_fetch(url: str, headers: dict, timeout: float) -> tuple[int, str]:
    """Default transport. Returns ``(status, body)``; HTTP errors are statuses,
    everything else (DNS, TLS, timeout) raises and is logged generically."""
    request = urllib.request.Request(url, headers=dict(headers), method="GET")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, ""


def spark_url(symbols) -> str:
    """The batch URL for ``symbols`` — every position in one request.

    The commas go out literal: that is the form verified against Yahoo, and
    ``urlencode`` would escape them to ``%2C``.
    """
    quoted = ",".join(urllib.parse.quote(sym, safe=".") for sym in symbols)
    return f"{SPARK_URL}?symbols={quoted}&range={SPARK_RANGE}&interval={SPARK_INTERVAL}"


def fetch_quotes(symbols, fetch=None) -> dict[str, dict] | None:
    """``{SYMBOL: meta}`` for the whole portfolio in one request, or None.

    ``fetch(url, headers, timeout) -> (status, body_text)`` is injectable so
    tests never touch the network. Any failure (transport, non-200, malformed
    body) returns None and the caller decides between the ``nonet`` screen and
    staying silent.
    """
    symbols = list(symbols)
    if not symbols:
        return {}
    transport = fetch or _urllib_fetch
    headers = {"Accept": "application/json", "User-Agent": USER_AGENT}
    url = spark_url(symbols)
    try:
        status, body = transport(url, headers, HTTP_TIMEOUT)
    except Exception:  # noqa: BLE001 — the message may quote the URL
        logger.warning("Yahoo spark request failed for %d symbols", len(symbols))
        return None
    if status != 200:
        logger.warning("Yahoo spark returned HTTP %s for %d symbols",
                       status, len(symbols))
        return None
    if isinstance(body, (bytes, bytearray)):
        body = body.decode("utf-8", "replace")
    try:
        results = json.loads(body)["spark"]["result"]
    except (ValueError, TypeError, KeyError):
        logger.warning("Yahoo spark response was not the expected shape")
        return None
    quotes: dict[str, dict] = {}
    for entry in results if isinstance(results, list) else []:
        if not isinstance(entry, dict):
            continue
        symbol = str(entry.get("symbol") or "").upper()
        response = entry.get("response")
        first = response[0] if isinstance(response, list) and response else {}
        meta = first.get("meta") if isinstance(first, dict) else None
        if symbol and isinstance(meta, dict):
            quotes[symbol] = meta
    return quotes


# ---------------------------------------------------------------------------
# quotes -> numbers
# ---------------------------------------------------------------------------

class Quote(NamedTuple):
    """One position priced, with everything the payload and the rows need."""

    symbol: str
    qty: float
    price: int
    change: int      # pesos per share, signed
    pct: int         # day change in hundredths of a percent
    when: float      # regularMarketTime, epoch seconds


def _quote(position: Position, meta: dict) -> Quote | None:
    """Price one position, or None when the quote is unusable.

    ``regularMarketChange`` when Yahoo has it, otherwise derived from the
    percent and the *derived* previous close — never from
    ``chartPreviousClose``, which is the unadjusted number the whole screen
    must not trust.
    """
    price = _as_number(meta.get("regularMarketPrice"))
    pct_raw = _as_number(meta.get("regularMarketChangePercent"))
    when = _as_int(meta.get("regularMarketTime"))
    if not price or price <= 0 or not when:
        return None
    change = _as_number(meta.get("regularMarketChange"))
    if change is None:
        pct_value = (pct_raw or 0.0) / 100.0
        if pct_value <= -1.0:
            return None  # prev <= 0: the derivation is meaningless
        # prev = price / (1 + pct/100)
        change = price - price / (1.0 + pct_value)
    return Quote(
        symbol=position.symbol,
        qty=position.qty,
        price=int(round(price)),
        change=int(round(change)),
        pct=int(round((pct_raw or 0.0) * 100.0)),
        when=when,
    )


def resolve(positions, quotes: dict[str, dict]) -> tuple[list[Quote], list[str]]:
    """``(priced, unresolved)`` — the positions Yahoo could price, and the
    symbols it could not.

    A quote from another exchange is *unresolved*, not priced: it is a
    different company. This is the guard against the silent collision (``EPM``
    alone answers with a US company at USD 3.50), and a screen that shows
    money cannot afford to be wrong quietly about which company that is.
    """
    priced: list[Quote] = []
    unresolved: list[str] = []
    for position in positions:
        meta = quotes.get(position.symbol)
        if meta is None:
            unresolved.append(position.symbol)
            logger.info(
                "portfolio: %s returned no quote — Yahoo may not list it "
                "(EPM%s and GXG%s do not exist)", position.symbol, SUFFIX, SUFFIX,
            )
            continue
        exchange = meta.get("exchangeName")
        if exchange != BVC:
            unresolved.append(position.symbol)
            logger.warning(
                "portfolio: %s quotes on %s, not %s — dropped; a mistyped "
                "ticker answers with another company instead of an error",
                position.symbol, exchange or "no exchange", BVC,
            )
            continue
        quote = _quote(position, meta)
        if quote is None:
            unresolved.append(position.symbol)
            logger.warning(
                "portfolio: %s has no usable price or timestamp — dropped",
                position.symbol,
            )
            continue
        priced.append(quote)
    return priced, unresolved


def session_state(now: float, last_price: float) -> tuple[str, str]:
    """``(session, stamp)`` for SPEC §6's ``s`` and ``t``.

    The session is derived from the data, never from a holiday calendar: a
    Colombian holiday calendar has fixed *and* movable holidays (Semana Santa),
    so any hardcoded list is wrong at least once a year. The provider's own
    timestamp answers it — on a holiday or a weekend the newest price is the
    previous session's, so it is not today and the screen says "cierre <fecha>".

    "Live" therefore needs three things: it is a weekday, it is inside
    10:00-15:00 Colombia time, and the newest price is *today's*. The first
    two come from the clock and the session hours, the third from the
    timestamp, and only the third needs the provider.
    """
    here = _bogota(now)
    latest = _bogota(last_price)
    live = (
        here.weekday() in SESSION_WEEKDAYS
        and SESSION_OPEN <= (here.hour, here.minute) <= SESSION_CLOSE
        and latest.date() == here.date()
    )
    return ("l" if live else "c"), _bogota_stamp(last_price)


# ---------------------------------------------------------------------------
# payload
# ---------------------------------------------------------------------------

def _rows(priced: list[Quote], session_day) -> list[list]:
    """The up-to-4 rows, in SPEC §6 order.

    Two rules that only exist because the data has them:

    * a ticker whose newest price is not from *this* session is out. Its 0.0 %
      means "did not trade today", not "is flat", and calling it a loser would
      be a false row about the user's money.
    * a % of exactly 0 is out, and no slot is ever backfilled.
    """
    ups: list[Quote] = []
    downs: list[Quote] = []
    for quote in priced:
        if _bogota(quote.when).date() != session_day or quote.pct == 0:
            continue
        (ups if quote.pct > 0 else downs).append(quote)
    ups.sort(key=lambda q: -q.pct)
    downs.sort(key=lambda q: q.pct)
    chosen = ups[:ROWS_UP] + downs[:ROWS_DOWN]
    # The ".CL" comes off here and only here: it is 3 characters per row that
    # say nothing on screen. The file keeps it, and so does the BVC check.
    return [
        [quote.symbol.removesuffix(SUFFIX)[:SYM_MAX], quote.price, quote.pct,
         int(round(quote.qty * quote.change))]
        for quote in chosen
    ]


def _payload(priced: list[Quote], unresolved: list[str], warnings: int, now: float) -> dict:
    """The priced-portfolio payload, in SPEC §6's key order and with its
    omissions: ``u``/``ux`` only when something is missing, ``w`` only when
    there is a warning, ``r`` only when there is a row."""
    last_price = max(quote.when for quote in priced)
    session, stamp = session_state(now, last_price)
    market_value = int(round(sum(quote.qty * quote.price for quote in priced)))
    day_change = int(round(sum(quote.qty * quote.change for quote in priced)))
    payload: dict = {
        "k": "pf",
        "ok": True,
        "s": session,
        "t": stamp,
        "mv": market_value,
        "dc": day_change,
        # Day change of the whole portfolio, in hundredths of a percent: the
        # big number is the money, the percentage is the annotation, and both
        # come out of regularMarketChange — never out of chartPreviousClose.
        "dp": int(round(day_change * 10000.0 / market_value)) if market_value else 0,
        "n": len(priced) + len(unresolved),
    }
    if unresolved:
        payload["u"] = len(unresolved)
        payload["ux"] = unresolved[0].removesuffix(SUFFIX)[:SYM_MAX]
    if warnings:
        payload["w"] = warnings
    rows = _rows(priced, _bogota(last_price).date())
    if rows:
        payload["r"] = rows
    return payload


def _error(reason: str, n: int) -> dict:
    """The state payload of SPEC §4: ``k``/``ok``/``e`` plus ``n``, which is
    never omitted because the screen counts positions with it ("0 de 12")."""
    return {"k": "pf", "ok": False, "e": reason, "n": n}


def fit(payload: dict) -> dict:
    """Degrade until the payload is ≤ ``PAYLOAD_MAX`` bytes, in SPEC §6's order:
    ``ux`` first (the count alone still says something), then the row with the
    smallest ``|%|`` — never emptying one sign's group while the other still has
    two, so the screen keeps a comparable pair.

    Numbers and symbols are never shortened: the "…" cut is visual, on the
    device, where the pixel budget is. Trims ``payload`` in place and returns it.
    """
    dropped_rows = 0
    while wire_bytes(payload) > PAYLOAD_MAX:
        if "ux" in payload:
            del payload["ux"]
            logger.warning("Portfolio payload over %d bytes: dropped ux", PAYLOAD_MAX)
            continue
        rows = payload.get("r")
        if not rows:
            logger.warning("Portfolio payload still over %d bytes with nothing "
                           "left to drop", PAYLOAD_MAX)
            break
        ups = [i for i, row in enumerate(rows) if row[2] > 0]
        downs = [i for i, row in enumerate(rows) if row[2] < 0]
        # A lone group survives while the other one still has a pair: two
        # losers in a row is a comparison, one loser against one winner is not.
        if len(ups) == 1 and len(downs) >= 2:
            candidates = downs
        elif len(downs) == 1 and len(ups) >= 2:
            candidates = ups
        else:
            candidates = list(range(len(rows)))
        victim = min(candidates, key=lambda i: abs(rows[i][2]))
        del rows[victim]
        dropped_rows += 1
        if not rows:
            del payload["r"]
    if dropped_rows:
        logger.warning("Portfolio payload over %d bytes: dropped %d row(s)",
                       PAYLOAD_MAX, dropped_rows)
    return payload


# ---------------------------------------------------------------------------
# entry point
# ---------------------------------------------------------------------------

def collect(
    now: float | None = None,
    portfolio_path: Path | str | None = None,
    fetch=None,
    had_data: bool = False,
) -> dict | None:
    """Build one portfolio BLE payload.

    ``fetch(url, headers, timeout) -> (status, body_text)`` is the injection
    point for tests; it defaults to the real urllib call.
    ``portfolio_path`` overrides ``~/.config/claude-usage-monitor/portfolio``.

    Returns a payload in every case except one: Yahoo is unreachable *and* the
    device already holds good numbers (``had_data``). Then it returns None and
    the daemon sends nothing, so the firmware falls into its own "abierto sin
    actualizar" state instead of replacing a real market value with an error
    screen.
    """
    now = time.time() if now is None else float(now)
    path = Path(portfolio_path) if portfolio_path is not None else DEFAULT_PORTFOLIO_PATH

    positions, warnings = read_positions(path)
    if not positions:
        return _error("nopos", 0)

    quotes = fetch_quotes([p.symbol for p in positions], fetch)
    if quotes is None:
        if had_data:
            logger.warning("Yahoo is down and the device already has prices; "
                           "sending nothing this cycle")
            return None
        return _error("nonet", len(positions))

    priced, unresolved = resolve(positions, quotes)
    if not priced:
        return _error("nores", len(positions))
    return fit(_payload(priced, unresolved, warnings, now))


__all__ = [
    "PAYLOAD_MAX",
    "SPARK_URL",
    "Position",
    "Quote",
    "collect",
    "fetch_quotes",
    "fit",
    "read_positions",
    "resolve",
    "session_state",
    "spark_url",
    "wire_bytes",
]
