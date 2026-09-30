#!/usr/bin/env python3
"""Unit tests for the BVC portfolio collector (SPEC §4, §6, §7).

Everything is hermetic: a positions file in tmp_path and a fake transport
standing in for Yahoo. No test touches the network, and the recorded portfolio
below is the one the real verification reconciled against the broker to the
peso (mv 93,959,765 / day -60,405, difference 0.00 on both).

Run: python -m pytest daemon/tests/test_portfolio_collector.py -q
"""
import json
import logging
import time
from datetime import datetime, timezone
from pathlib import Path

import pytest

import daemon.claude_usage_daemon as daemon_mod
from daemon.portfolio_collector import (
    BVC,
    PAYLOAD_MAX,
    SPARK_URL,
    collect,
    fit,
    read_positions,
    resolve,
    session_state,
    spark_url,
    wire_bytes,
)

# Tuesday 2026-09-29, 10:45 Colombia time = 15:45 UTC. Mid-session, so the
# recorded quotes are "live".
SESSION_UTC = datetime(2026, 9, 29, 15, 45, tzinfo=timezone.utc)
NOW = SESSION_UTC.timestamp()
# The newest quote of the recorded poll: 10:42 Colombia.
QUOTE_AT = int(datetime(2026, 9, 29, 15, 42, tzinfo=timezone.utc).timestamp())
# Friday before it — what a ticker that did not trade on Tuesday looks like.
STALE_AT = int(datetime(2026, 9, 25, 20, 0, tzinfo=timezone.utc).timestamp())

# The 12 recorded positions: symbol -> (price, change, pct, adjusted previous close)
RECORDED = {
    "ALPHA": (66260, 840, 1.2840, 65420),
    "BETAGROUP": (14800, 80, 0.5435, 14720),
    "GAMMA": (41820, -80, -0.1909, 41900),
    "DELTACORP": (76820, -360, -0.4664, 77180),
    "THETA": (375, -4, -1.0554, 379),
    "EPSILON": (29815, -85, -0.2843, 29900),
    "ZETA": (16431, -69, -0.4182, 16500),
    "IOTACORP": (15073, 73, 0.4867, 15000),
    "KAPPACO": (19080, -80, -0.4175, 19160),
    "LONGNAMEGRP": (17573, 43, 0.2453, 17530),
    "LONGSURGRP": (5934, -6, -0.1010, 5940),
    "OMEGA": (123040, 40, 0.0325, 123000),
}
QUANTITIES = {
    "ALPHA": 157, "BETAGROUP": 479, "GAMMA": 150, "DELTACORP": 265, "THETA": 24000,
    "EPSILON": 175, "ZETA": 425, "IOTACORP": 380, "KAPPACO": 285,
    "LONGNAMEGRP": 205, "LONGSURGRP": 160, "OMEGA": 105,
}
# The unadjusted closes Yahoo also reports. Research: >0.5 % wrong on 4 of the
# 12, +23.7 % on GAMMA, where it even flips the sign of the day change.
WRONG_CHART_PREVIOUS_CLOSE = {
    "GAMMA": 51820, "OMEGA": 115400, "KAPPACO": 19920, "ALPHA": 67720,
}

# SPEC §6's example payload, byte for byte.
SPEC_WIRE = (
    '{"k":"pf","ok":true,"s":"l","t":"09291042","mv":93959765,"dc":-60405,'
    '"dp":-6,"n":12,"r":[["ALPHA",66260,128,131880],["BETAGROUP",14800,54,38320],'
    '["THETA",375,-106,-96000],["DELTACORP",76820,-47,-95400]]}'
)
PORTFOLIO_FILE = "# the real portfolio\n" + "".join(
    f"{sym}.CL {QUANTITIES[sym]}\n" for sym in RECORDED
)


# ---------------------------------------------------------------------------
# fixtures: the positions file and a recorded spark response
# ---------------------------------------------------------------------------

def meta(symbol, price, change, pct, when, exchange=BVC, previous=None,
         chart_previous=None, **extra):
    """One ``spark`` ``meta`` object, in the shape Yahoo actually returns."""
    out = {
        "currency": "COP",
        "symbol": symbol,
        "exchangeName": exchange,
        "instrumentType": "EQUITY",
        "firstTradeDate": 1166035200,
        # Wrong metadata for a BVC asset (research): the collector must ignore
        # it and shift to Colombia time by hand.
        "exchangeTimezoneName": "America/New_York",
        "gmtoffset": -18000,
        "timezone": "EST",
        "regularMarketTime": when,
        "regularMarketPrice": float(price),
        "regularMarketChange": float(change),
        "regularMarketChangePercent": pct,
        "regularMarketDayHigh": float(price),
        "regularMarketDayLow": float(price),
        "regularMarketVolume": 123456,
        "fiftyTwoWeekHigh": float(price) * 1.2,
        "fiftyTwoWeekLow": float(price) * 0.8,
        "priceHint": 0.0,
        "dataGranularity": "1d",
        "range": "1d",
    }
    if previous is not None:
        out["previousClose"] = float(previous)
    if chart_previous is not None:
        out["chartPreviousClose"] = float(chart_previous)
    out.update(extra)
    return out


def spark_body(entries: dict) -> str:
    """``{symbol: meta}`` -> the recorded response body."""
    return json.dumps({"spark": {"error": None, "result": [
        {"symbol": sym, "response": [{"meta": m}]} for sym, m in entries.items()
    ]}})


def recorded_metas(**overrides) -> dict:
    """The 12 recorded quotes, with the *unadjusted* closes Yahoo also reports."""
    entries = {
        f"{sym}.CL": meta(
            f"{sym}.CL", price, change, pct, QUOTE_AT,
            previous=prev,
            chart_previous=WRONG_CHART_PREVIOUS_CLOSE.get(sym, prev),
        )
        for sym, (price, change, pct, prev) in RECORDED.items()
    }
    entries.update(overrides)
    return entries


class FakeFetch:
    """Stand-in for the HTTP transport.

    Callable as ``fetch(url, headers, timeout) -> (status, body)``, records every
    call in ``.calls`` so tests can assert the URL and the headers, and can be
    told to fail a specific way.
    """

    def __init__(self, status: int = 200, body: str | None = None,
                 raises: Exception | None = None) -> None:
        self.status = status
        self.body = spark_body(recorded_metas()) if body is None else body
        self.raises = raises
        self.calls: list[dict] = []

    def __call__(self, url, headers, timeout):
        self.calls.append({"url": url, "headers": dict(headers), "timeout": timeout})
        if self.raises is not None:
            raise self.raises
        return self.status, self.body


@pytest.fixture
def portfolio(tmp_path: Path) -> Path:
    path = tmp_path / "portfolio"
    path.write_text(PORTFOLIO_FILE)
    return path


@pytest.fixture
def fetch():
    """Default transport: HTTP 200 with the recorded response."""
    return FakeFetch(200)


def run(path, fetch=None, now=NOW, **kwargs):
    return collect(now=now, portfolio_path=path, fetch=fetch or FakeFetch(200),
                   **kwargs)


# ---------------------------------------------------------------------------
# the acceptance case: the real portfolio, the real payload
# ---------------------------------------------------------------------------

def test_real_payload_is_the_spec_example_to_the_byte(portfolio, fetch):
    payload = run(portfolio, fetch)
    wire = json.dumps(payload, separators=(",", ":"))
    assert wire == SPEC_WIRE
    assert len(wire.encode()) == 203
    assert wire_bytes(payload) == 203 <= PAYLOAD_MAX


def test_real_totals_reconcile_with_the_broker(portfolio, fetch):
    """93,959,765 and -60,405 are what the broker showed: difference 0.00."""
    payload = run(portfolio, fetch)
    assert (payload["mv"], payload["dc"], payload["dp"], payload["n"]) == (
        93959765, -60405, -6, 12,
    )
    assert payload["s"] == "l" and payload["t"] == "09291042"
    # Nothing is missing, so the warning fields are omitted entirely.
    assert "u" not in payload and "ux" not in payload and "w" not in payload


def test_rows_are_two_up_then_two_down_in_percent_order(portfolio, fetch):
    rows = run(portfolio, fetch)["r"]
    assert rows == [
        ["ALPHA", 66260, 128, 131880],
        ["BETAGROUP", 14800, 54, 38320],
        ["THETA", 375, -106, -96000],
        ["DELTACORP", 76820, -47, -95400],
    ]
    # Winners first, best first; then losers, worst first; symbols have no .CL.
    assert [row[2] > 0 for row in rows] == [True, True, False, False]
    assert [row[2] for row in rows[:2]] == sorted((row[2] for row in rows[:2]), reverse=True)
    assert [row[2] for row in rows[2:]] == sorted(row[2] for row in rows[2:])
    assert all(".CL" not in row[0] for row in rows)


def test_one_request_for_the_whole_portfolio(portfolio, fetch):
    """The batch endpoint: 12 positions, 1 request. Not 12."""
    run(portfolio, fetch)
    assert len(fetch.calls) == 1
    call = fetch.calls[0]
    assert call["url"] == (
        "https://query1.finance.yahoo.com/v7/finance/spark"
        "?symbols=ALPHA.CL,BETAGROUP.CL,GAMMA.CL,DELTACORP.CL,THETA.CL,EPSILON.CL,"
        "ZETA.CL,IOTACORP.CL,KAPPACO.CL,LONGNAMEGRP.CL,LONGSURGRP.CL,OMEGA.CL"
        "&range=1d&interval=1d"
    )
    assert call["url"].startswith(SPARK_URL) and "," in call["url"]


def test_request_is_anonymous_with_a_browser_user_agent(portfolio, fetch):
    """No key, no crumb, no cookie — Yahoo's stock agent gets a 429."""
    run(portfolio, fetch)
    headers = fetch.calls[0]["headers"]
    assert "Mozilla/5.0" in headers["User-Agent"]
    assert "Cookie" not in headers and "Authorization" not in headers
    assert fetch.calls[0]["timeout"] == 10.0


def test_payload_is_compact_json_in_spec_key_order(portfolio, fetch):
    payload = run(portfolio, fetch)
    wire = json.dumps(payload, separators=(",", ":"))
    assert ", " not in wire and ": " not in wire
    assert list(payload) == ["k", "ok", "s", "t", "mv", "dc", "dp", "n", "r"]
    assert wire.startswith('{"k":"pf"')


# ---------------------------------------------------------------------------
# the two findings from the real verification
# ---------------------------------------------------------------------------

def test_chart_previous_close_is_never_used(portfolio, fetch):
    """SPEC: the day change comes from regularMarketChange(Percent).

    The fixture carries the *unadjusted* close for four symbols — GAMMA's is
    23.7 % high, enough to invert the sign of its day change. The payload must
    be identical to the broker's either way.
    """
    baseline = run(portfolio, fetch)
    assert baseline["dp"] == -6 and baseline["dc"] == -60405

    sabotaged = {
        f"{sym}.CL": meta(
            f"{sym}.CL", price, change, pct, QUOTE_AT,
            previous=prev,
            chart_previous=999999.0 if sym == "GAMMA" else prev * 1.23,
        )
        for sym, (price, change, pct, prev) in RECORDED.items()
    }
    other = run(portfolio, FakeFetch(200, spark_body(sabotaged)))
    assert other == baseline
    # The broker's own -0.19 % for GAMMA, not the -19.57 % the chart close gives.
    gamma_row = [row for row in other["r"] if row[0] == "GAMMA"]
    assert gamma_row == [] or gamma_row[0][2] == -19


def test_day_change_falls_back_to_a_derived_previous_close(portfolio):
    """regularMarketChange absent -> prev = price / (1 + pct/100)."""
    without = {f"{s}.CL": meta(f"{s}.CL", p, 0, pct, QUOTE_AT, previous=prev)
               for s, (p, _c, pct, prev) in RECORDED.items()}
    for entry in without.values():
        entry.pop("regularMarketChange")
    payload = run(portfolio, FakeFetch(200, spark_body(without)))
    assert (payload["mv"], payload["dc"], payload["dp"]) == (93959765, -60405, -6)
    assert payload["r"][0] == ["ALPHA", 66260, 128, 131880]


def test_a_quote_from_another_exchange_is_dropped_not_shown(tmp_path, fetch):
    """The silent collision: "EPM" alone is a US company at USD 3.50.

    It must cost the screen a position, not gain it a wrong number in pesos.
    """
    entries = recorded_metas()
    entries["EPM"] = meta("EPM", 3.5, 0.25, 7.6904, QUOTE_AT,
                          exchange="ASE", currency="USD")
    path = tmp_path / "portfolio"
    path.write_text("ALPHA.CL 157\nEPM 40\nEPM.CL 10\n")
    payload = run(path, FakeFetch(200, spark_body(entries)))
    assert payload["mv"] == 157 * 66260          # EPM contributed nothing
    assert payload["u"] == 2                    # both EPM lines are unresolved
    assert payload["ux"] == "EPM"               # .CL stripped for the header
    assert payload["n"] == 3                    # three lines in the file
    assert all(row[0] != "EPM" for row in payload["r"])


# ---------------------------------------------------------------------------
# rows: what is excluded, and why
# ---------------------------------------------------------------------------

def test_stale_zero_change_ticker_is_not_a_loser(portfolio, fetch, caplog):
    """GRUPOAVAL-style: chg 0.0 with an old timestamp is "did not trade today"."""
    entries = recorded_metas(**{
        "GRUPOAVAL.CL": meta("GRUPOAVAL.CL", 817, 0, 0.0, STALE_AT, previous=817),
    })
    path = portfolio.read_text() + "GRUPOAVAL.CL 1000\n"
    portfolio.write_text(path)
    with caplog.at_level(logging.INFO):
        payload = run(portfolio, FakeFetch(200, spark_body(entries)))
    assert all(row[0] != "GRUPOAVAL" for row in payload["r"])
    # A 0.0 % never counts as a baja, and never as a row of its own.
    assert [row[0] for row in payload["r"]] == ["ALPHA", "BETAGROUP", "THETA", "DELTACORP"]
    # It still has a last price, so it is in the market value, like the broker's.
    assert payload["mv"] == 93959765 + 1000 * 817
    assert payload["dc"] == -60405
    assert payload["n"] == 13


def test_a_flat_ticker_from_today_is_also_excluded(portfolio, fetch):
    """0.00 % on today's timestamp is "is flat", and flat is not a row."""
    entries = recorded_metas(**{
        "IOTACORP.CL": meta("IOTACORP.CL", 15073, 0, 0.0, QUOTE_AT,
                            previous=15073, chart_previous=15073),
    })
    payload = run(portfolio, FakeFetch(200, spark_body(entries)))
    assert all(row[0] != "IOTACORP" for row in payload["r"])
    assert payload["mv"] == 93959765          # the price is still counted
    # ...and so is its change, which is now zero instead of +73 a share.
    assert payload["dc"] == -60405 - 380 * 73
    assert payload["dp"] == -9


def test_no_filler_rows_when_one_side_is_empty(portfolio, fetch):
    """A day where everything rises shows the winners from the top, no padding."""
    entries = {
        f"{sym}.CL": meta(f"{sym}.CL", price, abs(change), abs(pct), QUOTE_AT,
                          previous=prev,
                          chart_previous=WRONG_CHART_PREVIOUS_CLOSE.get(sym, prev))
        for sym, (price, change, pct, prev) in RECORDED.items()
    }
    payload = run(portfolio, FakeFetch(200, spark_body(entries)))
    rows = payload["r"]
    assert len(rows) == 2
    assert all(row[2] > 0 for row in rows)
    assert payload["dc"] > 0


def test_losers_are_ordered_most_negative_first(portfolio, fetch):
    """A third loser slots in ahead of the shallower one, never behind."""
    entries = recorded_metas()
    entries["OMEGA.CL"] = meta("OMEGA.CL", 123040, -800, -0.6481, QUOTE_AT,
                               previous=123840, chart_previous=123840)
    payload = run(portfolio, FakeFetch(200, spark_body(entries)))
    rows = payload["r"]
    assert [row[0] for row in rows] == ["ALPHA", "BETAGROUP", "THETA", "OMEGA"]
    assert [row[2] for row in rows[2:]] == [-106, -65]


# ---------------------------------------------------------------------------
# session and clock
# ---------------------------------------------------------------------------

def test_time_is_colombia_time_not_york(portfolio, fetch):
    """Yahoo says America/New_York; 15:42 UTC is 10:42 in Bogotá."""
    assert run(portfolio, fetch)["t"] == "09291042"
    # Same instant read as New York would be 11:42 -> a whole hour wrong.
    assert run(portfolio, fetch)["t"] != "09291142"


@pytest.mark.parametrize("now,expected", [
    # Inside the session on a Tuesday.
    (datetime(2026, 9, 29, 15, 45, tzinfo=timezone.utc), "l"),
    # 09:55 Bogotá: the newest price is still yesterday's close.
    (datetime(2026, 9, 29, 15, 55, tzinfo=timezone.utc), "l"),
    (datetime(2026, 9, 29, 14, 59, tzinfo=timezone.utc), "c"),
    # 15:30 Bogotá: today's close, market gone.
    (datetime(2026, 9, 29, 20, 30, tzinfo=timezone.utc), "c"),
    # Saturday: the newest price is Friday's, so "cierre 25 sep".
    (datetime(2026, 10, 3, 15, 0, tzinfo=timezone.utc), "c"),
    # Monday 12 Oct is a Colombian holiday: newest price is Friday 9 Oct.
    (datetime(2026, 10, 12, 15, 0, tzinfo=timezone.utc), "c"),
])
def test_session_state_comes_from_the_timestamp(portfolio, fetch, now, expected):
    payload = run(portfolio, fetch, now=now.timestamp())
    assert payload["s"] == expected
    # Whatever the state, t is the newest price's Colombia time.
    assert payload["t"] == "09291042"


def test_session_state_helper_is_pure():
    """A holiday is not a table: it is a timestamp that is not from today."""
    last_price = QUOTE_AT
    assert session_state(NOW, last_price) == ("l", "09291042")
    # Same price, read on a Sunday morning in Bogotá.
    sunday = datetime(2026, 9, 27, 15, 0, tzinfo=timezone.utc).timestamp()
    assert session_state(sunday, last_price) == ("c", "09291042")


# ---------------------------------------------------------------------------
# the positions file
# ---------------------------------------------------------------------------

def test_missing_file_is_the_nopos_state(portfolio, caplog):
    del portfolio
    with caplog.at_level(logging.WARNING):
        payload = run(Path("/nonexistent/portfolio"))
    assert payload == {"k": "pf", "ok": False, "e": "nopos", "n": 0}
    assert "portfolio" in caplog.text


def test_empty_file_is_also_nopos(tmp_path):
    path = tmp_path / "portfolio"
    path.write_text("# nothing here yet\n\n")
    assert run(path) == {"k": "pf", "ok": False, "e": "nopos", "n": 0}


def test_duplicate_symbol_keeps_the_first_and_warns(tmp_path, caplog):
    path = tmp_path / "portfolio"
    path.write_text("ALPHA.CL 157\nBETAGROUP.CL 479\nALPHA.CL 999\n")
    with caplog.at_level(logging.WARNING):
        payload = run(path)
    # The first quantity, not the sum and not the last one.
    assert payload["mv"] == 157 * 66260 + 479 * 14800
    assert payload["n"] == 2
    assert payload["w"] == 1
    assert "ALPHA.CL" in caplog.text and "first" in caplog.text


def test_invalid_quantities_and_lines_warn_and_do_not_count(tmp_path, caplog):
    path = tmp_path / "portfolio"
    path.write_text(
        "ALPHA.CL 157\n"
        "BETAGROUP.CL 0\n"            # zero
        "GAMMA.CL many\n"        # not a number
        "ZETA.CL -5\n"           # negative
        "DELTACORP.CL\n"         # no quantity
        "GRUPOAVAL.CL 183 extra\n"  # too many fields
        "IOTACORP.CL 380\n"
    )
    with caplog.at_level(logging.WARNING):
        payload = run(path)
    assert payload["n"] == 2 and payload["w"] == 5
    assert payload["mv"] == 157 * 66260 + 380 * 15073
    for line in ("BETAGROUP.CL", "GAMMA.CL", "ZETA.CL", "DELTACORP.CL", "GRUPOAVAL.CL"):
        assert line in caplog.text


def test_comments_blank_lines_and_case_are_fine(tmp_path):
    path = tmp_path / "portfolio"
    path.write_text("\n# holdings  # trailing comment\n  alpha.cl   157  \n\nBETAGROUP.CL 479\n")
    positions, warnings = read_positions(path)
    assert [p.symbol for p in positions] == ["ALPHA.CL", "BETAGROUP.CL"]
    assert [p.qty for p in positions] == [157, 479]
    assert warnings == 0


def test_the_file_is_reread_on_every_poll(portfolio, fetch):
    """Like the config: an edit lands within a poll, no restart."""
    first = run(portfolio, fetch)
    assert first["n"] == 12
    portfolio.write_text(PORTFOLIO_FILE + "EPM.CL 500\n")
    second = run(portfolio, fetch)
    assert second["n"] == 13 and second["u"] == 1 and second["ux"] == "EPM"
    assert second["mv"] == first["mv"]      # the unresolved one adds nothing
    portfolio.write_text("ALPHA.CL 157\n")
    third = run(portfolio, fetch)
    assert third["n"] == 1 and third["mv"] == 157 * 66260
    assert [row[0] for row in third["r"]] == ["ALPHA"]


def test_ux_is_the_first_missing_symbol_without_the_suffix(portfolio, fetch):
    portfolio.write_text("NOPE1.CL 10\nNOPE2.CL 20\nALPHA.CL 157\n")
    payload = run(portfolio, fetch)
    assert payload["u"] == 2 and payload["ux"] == "NOPE1"
    # A symbol written without the suffix keeps it verbatim in ux.
    portfolio.write_text("GONE 10\nALPHA.CL 157\n")
    payload = run(portfolio, fetch)
    assert payload["u"] == 1 and payload["ux"] == "GONE"


# ---------------------------------------------------------------------------
# transport failures
# ---------------------------------------------------------------------------

def test_yahoo_down_with_no_data_yet_is_nonet(portfolio):
    for transport in (FakeFetch(503, ""), FakeFetch(200, "<html>nope</html>"),
                      FakeFetch(raises=OSError("network unreachable"))):
        payload = run(portfolio, transport)
        assert payload == {"k": "pf", "ok": False, "e": "nonet", "n": 12}


def test_yahoo_down_after_data_means_send_nothing(portfolio, caplog):
    """The device keeps its market value; the firmware ages it on its own."""
    with caplog.at_level(logging.WARNING):
        assert run(portfolio, FakeFetch(503, ""), had_data=True) is None
    assert "sending nothing" in caplog.text


def test_nothing_resolves_is_nores(portfolio, fetch):
    assert run(portfolio, FakeFetch(200, spark_body({}))) == {
        "k": "pf", "ok": False, "e": "nores", "n": 12,
    }
    # Every quote is from another exchange: same state, same reason.
    foreign = {f"{s}.CL": meta(f"{s}.CL", p, c, pct, QUOTE_AT, exchange="ASE")
               for s, (p, c, pct, _prev) in RECORDED.items()}
    assert run(portfolio, FakeFetch(200, spark_body(foreign)))["e"] == "nores"


def test_a_malformed_entry_does_not_take_the_payload_down(portfolio, fetch):
    body = json.dumps({"spark": {"result": [
        {"symbol": "ALPHA.CL", "response": [{"meta": meta(
            "ALPHA.CL", 66260, 840, 1.284, QUOTE_AT, previous=65420)}]},
        "not a dict", {"symbol": "BETAGROUP.CL", "response": []},
    ], "error": None}})
    payload = run(portfolio, FakeFetch(200, body))
    assert payload["mv"] == 157 * 66260
    assert payload["u"] == 11


# ---------------------------------------------------------------------------
# the 240-byte budget
# ---------------------------------------------------------------------------

# The fattest payload the firmware's column math allows: a 10-digit market
# value, a 9-digit day change, the longest ux and four maximum-length rows.
# SPEC §6 measures this one at 274 bytes.
def _fat_payload(rows=None):
    return {
        "k": "pf", "ok": True, "s": "l", "t": "09291042",
        "mv": 1234567890, "dc": -987654321, "dp": -1025, "n": 12,
        "u": 2, "ux": "LONGNAMEGP", "w": 12,
        "r": rows if rows is not None else [
            ["LONGNAMEGP", 122960, 1042, 1240000],
            ["EPSILON", 122960, 1000, 1240000],
            ["LONGSURGRP", 122960, -1025, -1240000],
            ["GRUPOAVAL", 122960, -1042, -1240000],
        ],
    }


def test_worst_case_fits_the_budget_by_dropping_ux_then_the_smallest_row():
    payload = _fat_payload()
    # SPEC §6 measures 274 for this shape; this reconstruction lands on 270.
    assert wire_bytes(payload) == 270 > PAYLOAD_MAX
    fitted = fit(payload)
    assert wire_bytes(fitted) <= PAYLOAD_MAX
    # Step 1 of the degradation: ux goes first, the count stays.
    assert "ux" not in fitted and fitted["u"] == 2 and fitted["w"] == 12
    # Step 2: the row with the smallest |%| (+10.00 %) goes — the sign plays no
    # part in that choice while both groups still have two.
    assert [row[0] for row in fitted["r"]] == [
        "LONGNAMEGP", "LONGSURGRP", "GRUPOAVAL",
    ]
    assert [row[2] for row in fitted["r"]] == [1042, -1025, -1042]
    # Nothing that was kept was touched.
    assert fitted["mv"] == 1234567890 and fitted["dc"] == -987654321


def test_degradation_never_empties_a_group_while_the_other_has_a_pair():
    """1 winner and 3 losers, all with the same |%|: the winner is safe.

    Without the rule the tie would drop index 0 — the winner — and leave a
    screen with three losers and no winner.
    """
    payload = _fat_payload(rows=[
        ["LONGNAMEGP", 122960, 1025, 1240000],
        ["EPSILON", 122960, -1025, -1240000],
        ["LONGSURGRP", 122960, -1025, -1240000],
        ["GRUPOAVAL", 122960, -1025, -1240000],
    ])
    assert wire_bytes(payload) > PAYLOAD_MAX
    fitted = fit(payload)
    assert wire_bytes(fitted) <= PAYLOAD_MAX
    # ux first, then one loser — the tie is what a naive "smallest |%|" would
    # resolve against the winner at index 0.
    assert [row[0] for row in fitted["r"]] == [
        "LONGNAMEGP", "LONGSURGRP", "GRUPOAVAL",
    ]
    assert [row[2] for row in fitted["r"]] == [1025, -1025, -1025]


def test_degradation_keeps_the_numbers_it_does_send_whole():
    """No rounding, no "…" in the payload: that cut is visual, on the device."""
    payload = _fat_payload()
    assert wire_bytes(payload) > PAYLOAD_MAX
    fitted = fit(payload)
    for row in fitted["r"]:
        assert all(isinstance(value, int) for value in row[1:])
        assert len(row[0]) <= 10
    assert "…" not in json.dumps(fitted)


def test_an_empty_row_list_is_omitted_not_sent_as_empty():
    payload = _fat_payload(rows=[])
    del payload["r"]
    assert fit(dict(payload))["ok"] is True


def test_fitted_payload_stays_valid_json_in_spec_order(portfolio, fetch):
    payload = fit(_fat_payload())
    wire = json.dumps(payload, separators=(",", ":"))
    assert list(json.loads(wire)) == [
        "k", "ok", "s", "t", "mv", "dc", "dp", "n", "u", "w", "r",
    ]


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def test_spark_url_keeps_the_commas_literal():
    url = spark_url(["ALPHA.CL", "BETAGROUP.CL"])
    assert url == f"{SPARK_URL}?symbols=ALPHA.CL,BETAGROUP.CL&range=1d&interval=1d"
    assert "%2C" not in url


def test_resolve_reports_every_unresolved_symbol():
    from daemon.portfolio_collector import Position
    positions = [Position("ALPHA.CL", 157), Position("GONE.CL", 10)]
    priced, unresolved = resolve(positions, {})
    assert priced == [] and unresolved == ["ALPHA.CL", "GONE.CL"]


def test_quote_needs_a_price_and_a_timestamp():
    from daemon.portfolio_collector import Position, resolve as _resolve
    position = Position("ALPHA.CL", 157)
    priced, unresolved = _resolve([position], {"ALPHA.CL": {
        "exchangeName": BVC, "regularMarketPrice": 0.0, "regularMarketTime": 0,
    }})
    assert priced == [] and unresolved == ["ALPHA.CL"]


# ---------------------------------------------------------------------------
# daemon wiring
# ---------------------------------------------------------------------------

def test_read_portfolio_setting_defaults_to_off(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    monkeypatch.setattr(daemon_mod, "CONFIG_FILE", cfg)
    assert daemon_mod.read_portfolio_setting() == "off"        # absent file
    cfg.write_text("opencode = on\n")
    assert daemon_mod.read_portfolio_setting() == "off"        # key absent
    cfg.write_text("portfolio = ON   # the BVC screen\n")
    assert daemon_mod.read_portfolio_setting() == "on"
    cfg.write_text("portfolio = maybe\n")
    assert daemon_mod.read_portfolio_setting() == "off"        # bad value


def test_read_portfolio_path_is_optional(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    monkeypatch.setattr(daemon_mod, "CONFIG_FILE", cfg)
    assert daemon_mod.read_portfolio_path() is None
    cfg.write_text("portfolio_path = ~/pf/holdings   # a synced folder\n")
    assert daemon_mod.read_portfolio_path() == Path.home() / "pf/holdings"
    cfg.write_text("portfolio_path =\n")
    assert daemon_mod.read_portfolio_path() is None


def test_config_example_documents_the_option():
    text = (Path(daemon_mod.__file__).parent / "config.example").read_text()
    assert "portfolio = off" in text
    assert "portfolio_path" in text


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


PF_PAYLOAD = json.loads(SPEC_WIRE)
OC_PAYLOAD = {"k": "oc", "ok": True, "src": "api", "p5": 37, "r5": 134, "pw": 22,
              "rw": 5040, "pm": 3, "rm": 20354, "st": "ok", "t7": 106969,
              "m": "ds-v4.1-flash", "ms": 93, "a": 1, "ag": "build", "la": 35}


def _wire_cycle(tmp_path, monkeypatch, config, stop_after, claude_dead=False):
    cfg = tmp_path / "config"
    cfg.write_text(config)
    monkeypatch.setattr(daemon_mod, "CONFIG_FILE", cfg)
    monkeypatch.setattr(daemon_mod, "POLL_INTERVAL", 0)
    monkeypatch.setattr(daemon_mod, "OPENCODE_INTERVAL", 0)
    monkeypatch.setattr(daemon_mod, "PORTFOLIO_INTERVAL", 0)
    monkeypatch.setattr(daemon_mod, "TICK", 0.01)
    monkeypatch.setattr(daemon_mod, "OPENCODE_WRITE_DELAY", 0.25)
    monkeypatch.setattr(daemon_mod, "PORTFOLIO_WRITE_DELAY", 0.25)

    client = _FakeClient("FAKE", stop_after=stop_after)
    monkeypatch.setattr(daemon_mod, "BleakClient", lambda target: client)
    monkeypatch.setattr(daemon_mod, "collect", lambda *a, **k: OC_PAYLOAD)
    monkeypatch.setattr(daemon_mod, "collect_portfolio", lambda *a, **k: PF_PAYLOAD)

    async def fake_poll_active():
        if claude_dead:
            return None, True
        return {"s": 45, "sr": 120, "w": 28, "wr": 7200, "st": "allowed", "ok": True}, False

    monkeypatch.setattr(daemon_mod, "poll_active", fake_poll_active)

    import asyncio

    async def run():
        return await daemon_mod.connect_and_run("FAKE", asyncio.Event())

    # The return value means "a Claude payload made it through", which is False
    # in the no-token test on purpose; the writes are what these tests are about.
    asyncio.run(run())
    return client


def _keys(client):
    return [json.loads(data).get("k") for _t, data in client.writes]


def test_portfolio_payload_follows_the_claude_one_by_250ms(tmp_path, monkeypatch):
    """SPEC §6: same RX characteristic, same 2-slot buffer, 250 ms apart."""
    client = _wire_cycle(tmp_path, monkeypatch, "portfolio = on\n", 2)
    assert len(client.writes) == 2
    (t1, first), (t2, second) = client.writes
    assert '"k":"pf"' not in first                      # Claude first
    assert json.loads(second) == PF_PAYLOAD             # then the portfolio
    assert 0.25 <= t2 - t1 < 1.0


def test_portfolio_lands_between_claude_and_opencode(tmp_path, monkeypatch):
    client = _wire_cycle(tmp_path, monkeypatch,
                         "portfolio = on\nopencode = on\n", 3)
    assert _keys(client) == [None, "pf", "oc"]
    times = [t for t, _ in client.writes]
    assert 0.25 <= times[1] - times[0] < 1.0
    assert 0.25 <= times[2] - times[1] < 1.0


def test_no_portfolio_payload_when_the_config_is_off(tmp_path, monkeypatch):
    client = _wire_cycle(tmp_path, monkeypatch, "opencode = on\n", 2)
    assert _keys(client) == [None, "oc"]


def test_the_portfolio_beat_survives_a_dead_claude_token(tmp_path, monkeypatch):
    """The screen must work with no Claude token at all (SPEC §4)."""
    client = _wire_cycle(tmp_path, monkeypatch, "portfolio = on\n", 2,
                         claude_dead=True)
    (t1, first), (t2, second) = client.writes
    assert json.loads(first) == {"ok": False}      # "no data" for Claude
    assert json.loads(second) == PF_PAYLOAD
    assert 0.25 <= t2 - t1 < 1.0
