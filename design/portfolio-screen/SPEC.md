# BVC portfolio screen: payload, data and collector specification (v0.1)

**Status:** implemented. This document is reconstructed from the shipped source
(`daemon/portfolio_collector.py`, `daemon/claude_usage_daemon.py`,
`firmware/src/pf_data.{h,cpp}`, `firmware/src/pf_privacy.{h,cpp}`,
`firmware/src/ui_portfolio.{h,cpp}`, `firmware/src/main.cpp`,
`daemon/tests/test_portfolio_collector.py`,
`firmware/test/test_pf_format/test_pf_format.cpp`,
`firmware/sim/scenario-portfolio.jsonl`, `daemon/config.example`). It is the
companion of `design/portfolio-screen/PRIVACY.md`.

**Notation.** `<value>` stands for a concrete portfolio figure that has been
removed. Every numeric example below is either a protocol constant (a byte
ceiling, a count limit, a time) or a placeholder; symbols are synthetic.

**Scope.** This spec covers the wire payload, the collector's data pipeline
(source, positions file, row selection, session state) and the integration
points. Device layout, colours and typography live in the screen implementation
(`ui_portfolio.cpp`); they are not re-specified here.

---

## 1. Objective

Add a **BVC (Bolsa de Valores de Colombia) portfolio screen** to the
Clawdmeter. A daemon collector turns a user-maintained positions file into
**one BLE payload per poll**; the firmware renders a total, the day's change and
the day's biggest movers.

Design constraints:

- **No credentials.** Prices come from one public, anonymous Yahoo Finance
  request per poll.
- **Independent of Claude.** The collector runs on its own cadence and keeps
  working when the Claude token is missing, expired or failing.
- **Off by default.** Until `portfolio = on` is written, the screen never
  joins the cycle.
- **One payload, one consumer.** The daemon never learns which screen is
  visible, and the private mode is a firmware-side visibility switch over the
  exact same numbers (`ui.cpp:ui_update_portfolio`, `PRIVACY.md`).

---

## 2. Data source

### 2.1 Request

| Item | Value |
|---|---|
| Endpoint | `GET https://query1.finance.yahoo.com/v7/finance/spark` |
| Query | `symbols=<s1>,<s2>,…&range=1d&interval=1d` |
| Symbols | all accepted positions, one batched request |
| Comma encoding | literal `,`; each symbol is percent-quoted with `.` kept safe |
| Headers | `Accept: application/json`, `User-Agent: Mozilla/5.0` |
| Timeout | `10.0 s` (`HTTP_TIMEOUT`) |
| Credentials | none — no API key, crumb, cookie or `Authorization` |

Rules, all traceable to `portfolio_collector.py`:

- **One request for the whole portfolio.** `spark_url()` joins every accepted
  symbol and `fetch_quotes()` issues it once; the test
  `test_one_request_for_the_whole_portfolio` pins a single call. The module
  docstring records this as the reason the collector is viable at all.
- **Only `v7/finance/spark` answers.** The docstring records that
  `v7/finance/quote` is `Unauthorized`, `v8/finance/chart` is `Not Found` on a
  comma list and `v6/finance/quote` is `404`. *(Recorded verification, not
  re-checked here — see §11.)*
- **The `User-Agent` must be the bare `Mozilla/5.0`.** The docstring records
  that urllib's stock agent gets a `429` and a full Chrome string also gets a
  `429`, while `Mozilla/5.0` gets a `200` on the same URL/headers. *(Recorded
  verification — see §11.)*
- No request is made when the file yields no accepted positions: `collect()`
  returns the `nopos` error before calling the network.

### 2.2 Response parsing

`fetch_quotes()` walks `spark.result[]`; each entry contributes
`symbol` and `response[0].meta` to a `{SYMBOL: meta}` dict keyed by the
upper-cased symbol.

| Outcome | Result |
|---|---|
| Transport exception (DNS/TLS/timeout) | `None` |
| HTTP status ≠ 200 | `None` |
| Body not `json["spark"]["result"]` | `None` |
| Entry not a dict, or no `meta` | skipped |
| Empty symbol list | `{}` (never reached from `collect()`) |

`None` means "the source failed" and is handled by §6.4; `{}` means "the source
answered but nothing resolved".

### 2.3 Fields read, and fields deliberately not read

Only five `meta` keys are read (`_quote()`):

| Key | Use |
|---|---|
| `exchangeName` | must equal `"BVC"` (see below) |
| `regularMarketPrice` | per-share price |
| `regularMarketChange` | day change per share, when present |
| `regularMarketChangePercent` | day change percent, and the fallback for change |
| `regularMarketTime` | epoch seconds of the quote; session and `t` |

**`chartPreviousClose` is never read.** The day change comes only from
`regularMarketChange` / `regularMarketChangePercent`; when a previous close is
needed for the fallback it is *derived*, never taken from the payload. The test
`test_chart_previous_close_is_never_used` sabotages `chartPreviousClose` with
absurd values and asserts the payload is byte-identical. `previousClose` is
likewise not read, and `exchangeTimezoneName` is ignored entirely (§5).

### 2.4 Exchange and symbol rules

| Rule | Behaviour |
|---|---|
| Exchange gate | a quote is usable only if `meta["exchangeName"] == "BVC"` exactly; any other exchange is **unresolved** and dropped with a warning |
| File symbol | written by the user **verbatim**; the daemon never appends a suffix |
| `.CL` marker | required on BVC tickers by convention, but no suffix guessing: a bare symbol is logged as a warning and still sent to Yahoo |
| `.CL` stripping | only for the wire (`r[].sym`, `ux`); the file and the BVC check keep the full symbol |
| Symbol shape | `^[A-Z][A-Z0-9.\-]{0,19}$` on the file |

The exchange gate is the guard against a silent collision: a mistyped bare
ticker can answer with a different company on a different exchange and no error
at all. A non-`BVC` quote is a *wrong company*, so it costs the screen a
position instead of gaining it a wrong number.

### 2.5 Day change derivation

For a quote to be usable at all:

1. `regularMarketPrice` must be a finite number `> 0`.
2. `regularMarketTime` must be a non-zero integer.

Then:

- if `regularMarketChange` is a finite number, `change = regularMarketChange`;
- otherwise `pct_value = regularMarketChangePercent / 100`; if
  `pct_value <= -1` the quote is dropped (the derived previous close would be
  non-positive); else
  `change = regularMarketPrice - regularMarketPrice / (1 + pct_value)`.

Stored values (`Quote`): `price = round(price)` COP, `change = round(change)`
COP signed, `pct = round(regularMarketChangePercent * 100)` **hundredths of a
percent** signed, `when = regularMarketTime`.

---

## 3. Positions file

| Item | Rule |
|---|---|
| Default path | `~/.config/claude-usage-monitor/portfolio` |
| Override | `portfolio_path = <path>` (with `~` expanded) in the daemon config |
| Line format | `SIMBOLO.CL cantidad` — two whitespace-separated fields |
| Comments | `#` starts a comment to end of line |
| Blank lines | ignored |
| Case | the symbol is upper-cased before validation |
| Quantity | a float; commas are stripped; must be `> 0` |
| Duplicates | the **first** occurrence wins; the later one warns |
| Re-read | on **every** poll, so an edit lands within ~60 s without a restart |

Rejected lines (`read_positions()`), all logged:

| Case | Effect | Counts in `w`? |
|---|---|---|
| Field count ≠ 2 | line ignored | yes |
| Symbol fails `SYMBOL_RE` | line ignored | yes |
| Duplicate symbol | later line ignored | yes |
| Quantity missing, non-numeric or `<= 0` | line ignored | yes |
| Missing `.CL` suffix | **kept**, only a warning is logged | **no** |

A missing or unreadable file yields no positions and zero warnings → the
`nopos` state. `w` is the number of *accepted-position* warnings and is only
carried on the wire when `> 0`.

---

## 4. Row selection

The daemon sends at most **two gainers and two losers** — never a filler row.
Reference day is the calendar date of the **newest quote** (`max(when)`), i.e.
the current/last session, not necessarily the wall-clock day.

For each priced quote (`_rows()`):

1. Drop it if `date(when) != reference day` — a 0.0 % on a stale date means "did
   not trade in this session", not "flat", and must not become a row.
2. Drop it if `pct == 0` — a flat quote is not a variation, and a dropped slot
   is never backfilled.
3. Positive `pct` goes to the winners, negative to the losers.

Then:

| List | Sort | Take |
|---|---|---|
| winners (`pct > 0`) | descending `pct` (best first) | first `ROWS_UP = 2` |
| losers (`pct < 0`) | ascending `pct` (most negative first) | first `ROWS_DOWN = 2` |

Wire row: `[sym, price, pct, contrib]`, where

- `sym` = file symbol with `.CL` removed, truncated to `SYM_MAX = 10` chars;
- `price` = per-share price (COP, integer, no sign);
- `pct` = signed hundredths of a percent;
- `contrib` = `round(qty * change)` COP, signed.

Unresolved positions are never rows. The whole list is omitted when empty.

---

## 5. Session state and time

Yahoo reports `exchangeTimezoneName: America/New_York` for BVC assets. That
metadata is wrong, so Colombia time is computed **by hand at a fixed UTC-5**
(`COT_OFFSET_S = -5 * 3600`); Colombia has no DST, so the offset is exact and no
timezone database is needed.

`session_state(now, last_price)` returns `(s, t)`:

| Field | Rule |
|---|---|
| `s` | `"l"` (live) iff all three: the clock is a weekday Mon–Fri **and** `(hour, minute)` is within `(10,00) … (15,00)` inclusive **and** the newest price's date equals the clock's date. Otherwise `"c"` (closed). |
| `t` | always `MMDDhhmm` Colombia time of `last_price` (the newest quote), independent of `s`. |

- **No holiday calendar.** Holidays are not a table: on a Colombian holiday or
  a weekend the newest price is the previous session's, so `latest.date() !=
  here.date()` and the screen says "closed". A hardcoded holiday list would be
  wrong at least once a year (fixed *and* movable holidays).
- The session is derived from **both** the clock (weekday, hours) and the
  provider's timestamp (the "today" test); only the last one needs Yahoo.
- The device renders `t` as a clock (`10:42`) when live and as a date
  (`29 sep`) when closed.

---

## 6. Payload and protocol

### 6.1 Tag and transport

| Item | Value |
|---|---|
| Tag | `"k":"pf"` (`k` is the first key) |
| Characteristic | the shared RX characteristic, `4c41555a-4465-7669-6365-000000000002` |
| Service | `4c41555a-4465-7669-6365-000000000001` |
| Distinguishing | OpenCode uses `"k":"oc"`; a Claude payload carries no `k` |
| Encoding | compact JSON, separators `(",", ":")`, UTF-8 |
| Firmware route | `pf_is_payload()` scans for `"k"`/`"pf"` before parsing; whitespace is tolerated |
| Ack | `ble_send_ack()` on a successful `pf_parse()`; `ble_send_nack()` otherwise |
| RX buffer | a 2-slot FIFO in `ble.cpp`, so the Claude + portfolio (+ OpenCode) burst is absorbed; a third write drops the oldest |
| OTA | a valid portfolio payload also calls `ota_confirm()`, like the other owner payloads |

`main.cpp` routes OpenCode first, then portfolio, then Claude; a portfolio beat
never touches the Claude path (no usage-rate sample, no chime, no
`ui_update`).

### 6.2 Priced payload field list

Key order is fixed (`_payload()`):

```json
{"k":"pf","ok":true,"s":"l","t":"MMDDhhmm","mv":<value>,"dc":<value>,"dp":<value>,"n":<value>,
 "u":<value>,"ux":"SYM","w":<value>,"r":[["SYM",<value>,<value>,<value>]]}
```

| Key | JSON type | Firmware (`PfData`) | Meaning |
|---|---|---|---|
| `k` | string | — | always `"pf"` |
| `ok` | bool | `ok` | `true` on this payload; `false` on an error payload (§6.4) |
| `s` | string | `live` | `"l"` live / `"c"` closed (§5) |
| `t` | string `MMDDhhmm` | `t[9]` | Colombia time (fixed UTC-5) of the newest price |
| `mv` | int (COP) | `long mv` | market value = `round(Σ qty·price)` over resolved positions |
| `dc` | int (COP, signed) | `long dc` | day change = `round(Σ qty·change)` over resolved positions |
| `dp` | int | `int dp` | day change in hundredths of a percent: `round(dc·10000/mv)`, or `0` if `mv == 0` |
| `n` | int | `int n` | accepted positions from the file (resolved + unresolved) |
| `u` | int, optional | `int u` | positions that did not resolve to a usable BVC price; omitted when 0 |
| `ux` | string, optional | `ux[11]` | first unresolved symbol, `.CL` stripped, ≤ 10 chars; omitted when there is none |
| `w` | int, optional | `int w` | config warnings (§3); omitted when 0 |
| `r` | array, optional | `PfRow r[4]` + `nr` | up to four rows (§6.3); omitted when empty |

`mv`, `dc`, `n`, `u` and `w` are JSON integers; `mv`/`dc` are read into `long`.
`u`, `ux` and `w` are **omitted entirely** when empty — absence is the "all
resolved / no warnings" signal, not a zero.

### 6.3 Rows

`r` is an array of 0–4 rows, each an array of exactly four values:

| Position | Type | Meaning |
|---|---|---|
| `[0]` | string | symbol, `.CL` removed, ≤ `SYM_MAX` (10) chars |
| `[1]` | int | per-share price (COP), no sign |
| `[2]` | int | signed hundredths of a percent |
| `[3]` | int | signed contribution `round(qty·change)` COP |

Firmware (`pf_parse()`): at most `PF_ROWS_MAX = 4` rows; a row with fewer than
four elements is skipped rather than drawn half; `sym` is capped at
`PF_SYM_MAX = 10`. Any visual truncation (a trailing `.`) happens later, on the
device, never in the payload.

### 6.4 Error payloads

When `ok` is `false`, `e` and `n` replace the priced fields:

```json
{"k":"pf","ok":false,"e":"nopos","n":0}
{"k":"pf","ok":false,"e":"nonet","n":<value>}
{"k":"pf","ok":false,"e":"nores","n":<value>}
```

| `e` | Condition | `n` |
|---|---|---|
| `nopos` | file missing/empty, or no position was accepted | `0` |
| `nonet` | Yahoo is unreachable / non-200 / malformed **and** the device has no good data yet | accepted positions |
| `nores` | positions exist but **none** resolved (no quote, wrong exchange, usable-price failure) | accepted positions |

`n` is never omitted: the screen counts positions with it. When Yahoo fails but
the daemon has already sent a good payload (`had_data == true`, tracked as
`pf_seen`), `collect()` returns **`None`** and sends nothing, so the firmware
falls into its own "open, not updated" state instead of replacing real numbers
with an error screen. `nonet` is therefore only ever shown before the first good
payload.

### 6.5 The 240-byte ceiling and `fit()`

`PAYLOAD_MAX = 240` bytes, identical to the OpenCode ceiling. The size is
`len(json.dumps(payload, separators=(",", ":")).encode("utf-8"))`
(`wire_bytes()`). `fit()` is applied to priced payloads only and trims in place.

Degradation order:

1. **Drop `ux` first.** The count `u` stays; a count alone still says something,
   the first symbol is the dispensable part.
2. **Then drop the row with the smallest `|pct|`.** Numbers and symbols are
   **never** shortened: the `.` cut is visual, on the device.
3. **Repeat** step 2 until the payload fits or there are no rows left; when the
   last row goes, the `r` key is removed rather than sent as `[]`.

Candidate guard while dropping rows — this is what keeps the screen comparable:

| Winners (`pct>0`) | Losers (`pct<0`) | Eligible rows |
|---|---|---|
| exactly 1 | ≥ 2 | **losers only** (the lone winner is protected) |
| ≥ 2 | exactly 1 | **winners only** (the lone loser is protected) |
| ≥ 2 | ≥ 2 | all rows |
| exactly 1 | exactly 1 | all rows |
| any | 0 | all rows in the only group |
| 0 | any | all rows in the only group |

So a sign group is never emptied while the other still has two. Ties in `|pct|`
are broken by position: `min()` keeps the first candidate, and candidates are
scanned winners-first then losers.

Pseudo-code, exactly as implemented:

```python
while wire_bytes(payload) > 240:
    if "ux" in payload:
        del payload["ux"]
        continue
    rows = payload.get("r")
    if not rows:
        break                       # nothing left to drop
    ups   = [i for i, row in enumerate(rows) if row[2] > 0]
    downs = [i for i, row in enumerate(rows) if row[2] < 0]
    if len(ups) == 1 and len(downs) >= 2:
        candidates = downs
    elif len(downs) == 1 and len(ups) >= 2:
        candidates = ups
    else:
        candidates = list(range(len(rows)))
    victim = min(candidates, key=lambda i: abs(rows[i][2]))
    del rows[victim]
    if not rows:
        del payload["r"]
```

The fields `k`, `ok`, `s`, `t`, `mv`, `dc`, `dp`, `n`, `u` and `w` are never
dropped. The recorded real payload is well under the ceiling; the reconstructed
worst case in the unit tests starts above it and is trimmed to fit (the exact
pre-fit byte count is discussed in §11).

---

## 7. Device rendering (informative)

`ui_portfolio.cpp` formats the payload; `test_pf_format.cpp` pins the formats.
Comma for thousands, dot for decimals; `+`/ASCII `-`/nothing for zero; rounding
always happens before the unit is chosen, so a value just under a unit can never
print as the next one.

| Value | Format |
|---|---|
| market value `mv` | plain level, no sign, never coloured; `< 1M` full, `< 999.95M` as `X.XM`, else `X.XXB` |
| day change `dc`, row `contrib` | signed COP; `< 1M` full with commas, else signed `X.XXM` |
| day percent `dp` | signed `X.XX%`; the sign comes from `dc`, never from `dp`, so a day that rounds to zero still shows its true sign (`-0.00%`) |
| row percent `pct` | signed `X.XX%` |
| price `price` | full, never abbreviated, no sign — it is the figure compared against the broker |
| symbol | `.CL` already gone; cut on screen to the measured pixel budget with an **ASCII `.`** (never `…`), and not drawn at all below three letters |
| session | `t` as `10:42` live, `29 sep` closed |

Colour: green above zero, red below, plain text at zero, and only ever on a
signed number. The strings are Spanish (the screen shows `Valor total`,
`Cambio hoy`, `Cierre`, `Sin actualizar`, `En vivo`, the empty states, etc.).

---

## 8. Daemon integration

| Item | Value |
|---|---|
| Config | `portfolio = off \| on` (default **off**); `portfolio_path` optional, `~` expanded |
| Cadence | `PORTFOLIO_INTERVAL = 60 s`, its own beat, independent of the Claude token |
| Execution | `asyncio.to_thread(collect_portfolio, …)` so the blocking I/O never stalls the BLE loop |
| Order | the portfolio write sleeps `PORTFOLIO_WRITE_DELAY = 0.25 s`, so it lands **between** Claude and OpenCode; the tests assert `[None, "pf", "oc"]` |
| Config re-read | on each 60 s beat, so flipping `portfolio = on` takes effect within ~60 s |
| `had_data` | a `pf_seen` flag, set only after a good payload is written; passed as `collect(..., had_data=pf_seen)` |
| Log | one line per payload: market value, day change and percent, row count, session; or the error reason and position count |

The screen must keep working with **no Claude token at all**; the daemon tests
cover that path.

---

## 9. Firmware integration

| Layer | Behaviour |
|---|---|
| `pf_data.h` | `PfData` is copied **by value** into the UI and lives in internal SRAM (the C6 boards have no PSRAM) |
| `pf_is_payload()` | cheap substring scan (tolerates whitespace), decides routing without deserialising |
| `pf_parse()` | ArduinoJson 7; rejects malformed JSON or `k != "pf"`; on success overwrites every field; defaults: `e="nores"`, `s`→closed, numbers `0` |
| `ui_portfolio.cpp` | owns the container, palette and text; `ui.cpp` owns navigation, the page indicator and the battery |
| `pf_has_data()` | sticky "a valid payload has arrived since boot"; the screen only joins the cycle once true |
| `pf_usage_set_ble()` | a dropped link dims the two cards and shows `! Bluetooth desconectado`; freshness (`PF_FRESH_MS = 300000`) is independent and only changes the status text when live |
| PWR | short press on this screen toggles private mode (`main.cpp`), not brightness (`PRIVACY.md`) |

---

## 10. Simulator

`firmware/sim/scenario-portfolio.jsonl` plays the **17 portfolio states** as
`{"k":"pf"}` beats, interleaved with one Claude and one OpenCode beat so
`SIM_START_SCREEN=portfolio` finds the screen. States, in order:

| # | State | `ok` / `s` / `e` |
|---|---|---|
| 01 | market open | `true`, `l` |
| 02 | market closed | `true`, `c` |
| 03 | holiday (previous close) | `true`, `c` |
| 04 | not updated | `true`, `l` |
| 05 | all up (no divider) | `true`, `l` |
| 06 | all down (no divider) | `true`, `l` |
| 07 | a single winner | `true`, `l` |
| 08 | no movers | `true`, `l` |
| 09 | day change 0 | `true`, `l` |
| 10 | long symbol | `true`, `l` |
| 11 | one ticker without a price | `true`, `l`, `u=1` |
| 12 | two tickers without a price | `true`, `l`, `u=2` |
| 13 | one config warning | `true`, `l`, `w=1` |
| 14 | no positions | `false`, `nopos` |
| 15 | nothing resolved | `false`, `nores` |
| 16 | Yahoo down | `false`, `nonet` |
| 17 | BLE dropped (payload real; link toggled with `d`) | `true`, `l` |

Play with `SIM_SCENARIO=sim/scenario-portfolio.jsonl SIM_START_SCREEN=portfolio`.

---

## 11. Traceability and unverified claims

### Traceability

| Claim | Source |
|---|---|
| Tag, shared characteristic, compact JSON, ack/nack | `portfolio_collector.py` (§6), `pf_data.cpp`, `main.cpp`, `ble.cpp` |
| Field list, omissions, key order | `_payload()`, `_error()`, `pf_data.h`, unit tests |
| 240-byte ceiling, `fit()` order and guard | `PAYLOAD_MAX`, `fit()`, `test_worst_case_fits_…`, `test_degradation_…` |
| Spark request, headers, timeout, one batch | `SPARK_URL`, `USER_AGENT`, `spark_url()`, `fetch_quotes()`, `test_one_request_…`, `test_request_is_anonymous_…` |
| `exchangeName == "BVC"`, no suffix guessing | `BVC`, `SUFFIX`, `resolve()`, `config.example`, `test_a_quote_from_another_exchange_…` |
| `chartPreviousClose` never read; change derivation | `_quote()`, `test_chart_previous_close_is_never_used`, `test_day_change_falls_back_…` |
| Positions file rules and `w` accounting | `read_positions()`, `SYMBOL_RE`, unit tests |
| Row selection (2+2, no filler, unresolved dropped) | `_rows()`, `ROWS_UP/DOWN`, unit tests |
| Session rule and fixed UTC-5 | `COT_OFFSET_S`, `session_state()`, `_bogota()`, `test_session_state_…` |
| Daemon cadence, order, `had_data` | `claude_usage_daemon.py`, `test_portfolio_lands_between_…`, `test_yahoo_down_after_data_…` |
| Formats | `ui_portfolio.cpp`, `test_pf_format.cpp` |
| 17 sim states | `scenario-portfolio.jsonl` |

### Claims that could not be fully verified

1. **The original section numbering of the uncommitted portfolio spec.** The
   code comments cite §2.1/§2.2/§2.3/§2.3.1/§6.2/§6.2.3/§7, but those sections
   are not in the repo; this reconstruction uses its own numbering.
2. **Yahoo endpoint behaviour and the `User-Agent` finding.** The docstring
   records `2026-09-30` verification (which endpoints answer, the `429` for a
   full browser UA vs `200` for `Mozilla/5.0`). Not re-checked here (no network
   call was made).
3. **`chartPreviousClose` inaccuracy and broker reconciliation.** The docstring
   and test comments record >0.5 % wrong closes and a sign flip, and a
   peso-exact match to the broker. These rest on the recorded verification and
   cannot be re-derived without the real quotes.
4. **The uncommitted spec's worst-case byte measurement (274 bytes).** The unit
   test reconstructs the same shape at 270 bytes. The 274 figure only appears in
   a test comment; the 270 is the value the code actually produces.
5. **`exchangeTimezoneName == America/New_York` for BVC assets.** Recorded in
   the module docstring and the test fixtures; not independently confirmed.
6. **The exact pre-fit count of the "recorded real payload"** was deliberately
   replaced with prose here, because it was derived from a real portfolio.

**One inconsistency worth flagging:** a comment in
`test_session_state_comes_from_the_timestamp` labels the 15:55 UTC case
"09:55 Bogotá", but 15:55 UTC is 10:55 COT (inside the session, correctly `l`).
The rule stated in §5 is taken from the code, not the comment.
