# BVC portfolio private mode: specification (v0.1)

**Status:** implemented. Reconstructed from `firmware/src/pf_privacy.{h,cpp}`,
`firmware/src/ui_portfolio.{h,cpp}`, `firmware/src/pf_data.h`,
`firmware/src/ui.cpp`, `firmware/src/main.cpp`,
`firmware/src/brightness.cpp`, `daemon/portfolio_collector.py` and the simulator
scenario. Companion of `design/portfolio-screen/SPEC.md`.

**Notation.** `<value>` stands for a concrete portfolio figure that has been
removed.

---

## 1. Objective

The portfolio screen shows the value of the user's holdings. In public, a
single glance can reveal how much money is being held. **Private mode is a
one-gesture visibility switch** that hides the size of the portfolio while still
showing the market's movement.

It is deliberately **not** encryption and **not** access control: it is a
screen-level display toggle over numbers the device already holds.

---

## 2. The switch

### 2.1 Storage, default and lifetime

| Item | Value |
|---|---|
| State | one `bool` |
| NVS namespace | `"clawdmeter"` |
| NVS key | `"pf_priv"` |
| Default | **off** (`getUChar(key, 0) != 0`; the static starts `false`) |
| Read | `pf_privacy_init()`, once at boot, read-only |
| Written | `pf_privacy_toggle()`, immediately on each toggle |

The namespace and key style are the same as brightness's `brt_idx`, so both
survive a screen change, the idle fade and a reboot. The default is off because
the user enabled the screen precisely to see their numbers; the first boot has
to look like every other boot.

### 2.2 Toggle gesture and notice

- `main.cpp` calls `pf_privacy_toggle()` on a **short PWR press while the
  portfolio screen is visible**. (On other screens PWR is brightness, on the
  splash it advances the scene; pairing is unchanged.)
- `pf_privacy_toggle()` inverts the flag, writes NVS, and arms a transient
  notice by stamping `notice_ms = millis()`.
- `pf_privacy_notice()` is true for `PF_NOTICE_MS = 1500 ms`. While it is true,
  the status line shows `Modo privado` / `Modo normal`, with priority over every
  other status text (disconnected, live, stale, closed). The window is the same
  length as the page indicator, because the user has a "PWR = brightness" habit
  and this is what explains the change in the act.
- The mode is re-read from the flag, not from the notice: `pf_usage_tick()`
  redraws when the current mode differs from the one last applied.

Edge case: because `notice_ms` starts at `0`, `pf_privacy_notice()` also reports
true for the first ~1.5 s after boot, before any toggle. The portfolio screen is
not in the cycle until a payload has arrived, so this is not normally visible.
*(See §9.)*

---

## 3. What is hidden

Private mode hides the numbers whose meaning depends on **how much is held**.
Per-share quotes are not hidden because they are public and identical for every
holder. The **day change percent** (`dp`) is also derived from the holdings, but
it is a ratio, so it carries no scale and is the value that replaces the hero
rather than being hidden.

### 3.1 Amounts derived from holdings

| Number | Payload field | Treatment | Why |
|---|---|---|---|
| Market value | `mv` | **hidden** | `Σ qty · price` — directly the size of the portfolio |
| Day change in COP | `dc` | **hidden** | `Σ qty · change` — scales with holdings |
| Row contribution | `r[].contrib` | **hidden** | `qty · change` per position |
| Day change in % | `dp` | **kept, as the hero** | same information as `dc`, but a scale-free ratio |

### 3.2 The position count

`n` — the accepted positions in the file — is private: it says how many shares
are held. It is hidden in **two** places:

- card 2's title line, normally `Mayores variaciones` + `de <n>`;
- the `nores` empty state, normally `0 de <n> con precio`, which would leak `n`
  even though the payload is `ok:false`.

`nores` is the one empty state that reacts to the mode: the card box itself
stays the normal one (there is nothing else to hide), but the text becomes
`Ninguno con precio`.

---

## 4. Layout in private mode

Card 1 loses the two day-change columns and the `COP` unit, so it gets shorter;
card 2 moves up as a whole, keeping its own height. The leftover gap at the
bottom is the visible sign of the mode and is never filled.

| Board | Normal card 1 `h` → private | Normal card 2 `y` → private |
|---|---|---|
| Large (480×480) | `172` → `109` | `256` → `193` |
| Compact (368×448 / 410×502) | `150` → `98` | `224` → `172` |
| Small (240×240, breakpoint currently unused) | `63` → `42` | `99` → `78` |

Inside card 1, the hero becomes the day's percent, the label changes from
`Valor total` to `Cambio hoy`, and the `COP` unit and both columns are hidden.
Inside card 2, the per-row contribution column is dropped where it existed
(the large board; the compact and small boards already have no contribution
column), and the `de <n>` count is hidden. The symbol and percent columns stay;
the per-share price stays where the board has one.

---

## 5. States that react to the mode

| State | Private mode applied? | What changes |
|---|---|---|
| `ok:true` (priced payload) | yes | §3 and §4 in full |
| `nopos` / `nonet` (`ok:false`) | no card change | nothing private to hide; text is the same |
| `nores` (`ok:false`) | **text only** | `0 de <n> con precio` → `Ninguno con precio` (§3.2) |
| BLE disconnected | dimming is independent | the two cards dim to 40 % in **both** modes; the mode does not alter the link warning |
| Fresh / stale (`s:"l"`) | no | the status line is identical in both modes |

The mode switch is defined as `want_private() = pf_privacy_get() && cur.ok`:
private behaviour only applies to a payload that actually has data. The `nores`
text is the sole exception, because it is read directly from
`pf_privacy_get()`.

---

## 6. Transport is untouched

**The payload is identical in both modes.** This is a firmware-only visibility
switch:

- `ui_update_portfolio()` stores the parsed `PfData` by value and redraws; it
  never rewrites, strips or re-serialises a field.
- `pf_data.h` states the contract explicitly: *"The payload is identical in
  private mode: hiding a number is a firmware concern, never a transport one."*
- The daemon has no privacy concept at all: `portfolio_collector.py` sends the
  same `mv`, `dc` and `contrib` whether or not the user has ever pressed PWR.
- The mode is therefore **not** a confidentiality boundary on the BLE link. The
  shared RX characteristic is bonded/owner-only (`ble.cpp` drops writes from a
  non-owner or an unencrypted link), but a party that can already read the
  payload sees the same numbers in both modes, and so does anything reading the
  device's stored values. Private mode protects the *screen* when the device is
  unplugged, rebooted and woken up in public — the risk named in `pf_privacy.h`.

---

## 7. Field-by-field visibility

| Payload field | Screen element | Normal | Private |
|---|---|---|---|
| `mv` | card 1 hero | shown | **hidden** |
| `dc` | card 1 left column | shown | **hidden** (still used for the hero's colour/sign) |
| `dp` | card 1 right column | shown | **shown as the hero** |
| `n` | card 2 `de <n>`; `nores` text | shown | **hidden** |
| `r[].contrib` | card 2 column (large board) | shown | **hidden** |
| `r[].price` | card 2 price column | shown | shown (§7.1) |
| `r[].pct` | card 2 percent column | shown | shown |
| `r[].sym` | card 2 symbol column | shown | shown |
| `u` | header `! <u> sin precio` | shown | shown |
| `ux` | header first missing symbol | shown | shown |
| `w` | header `! <w> aviso, ver log` | shown | shown |

The hour, the session state (`En vivo` / `Cierre` / `Sin actualizar`) and the
Bluetooth warning are not portfolio figures and do not change with the mode.

### 7.1 The per-share price exception

The price column is the one peso figure that stays. It is a public quote:
identical for anyone holding one share or a hundred thousand, so it reveals no
holdings. It is also the figure the user compares against the broker. In private
mode the price keeps its muted styling and still has no sign, exactly as in
normal mode.

---

## 8. Design intent

- **Hide scale, show movement.** `mv`, `dc` and `contrib` are the peso amounts
  that say how much money is at stake; `dp` is their ratio. Private mode drops
  the peso amounts and trades the total for the day's percent.
- **One gesture, reversible, remembered.** A short PWR press toggles and
  persists; a 1.5 s on-screen notice explains it. There is no PIN, because the
  threat is a glance, not an attacker: anyone holding the device can toggle it
  back.
- **Default visible.** Off by default, since the screen exists to show the
  numbers and privacy is opt-in.
- **Screen-only.** The data path is left alone (§6), which keeps the daemon
  simple and the behaviour of every other screen unchanged.

---

## 9. Traceability and unverified claims

### Traceability

| Claim | Source |
|---|---|
| NVS namespace/key, default off, persistence | `pf_privacy.cpp`, `brightness.cpp` |
| Toggle gesture on PWR | `main.cpp` |
| 1.5 s notice and its priority | `PF_NOTICE_MS`, `pf_privacy_notice()`, `draw_status()` |
| `want_private() = flag && ok`; `nores` exception | `want_private()`, `draw_panel1()` |
| Card 1/2 geometry deltas | `compute_layout()` (`pv_c1_h`, `pv_c2_y`), `apply_mode()`, `draw_panel1()` |
| Which fields are hidden | `draw_panel1()`, `draw_panel2()`, `draw_header()`, `L.f_priv` |
| Price stays; intent | `fmt_price()` comment, §7.1 |
| Transport unchanged | `pf_data.h`, `ui.cpp:ui_update_portfolio`, `portfolio_collector.py` |
| BLE ownership/encryption gate | `ble.cpp` |

### Claims that could not be fully verified

1. **The original section numbering of the uncommitted privacy spec.** Code
   comments cite §1–§5, §3.2, §4, §6 and §7.1; the document itself is not in
   the repo, so this reconstruction chooses its own numbering and only the
   claims are cross-checked.
2. **The boot-time notice edge.** `notice_ms` is `0` at boot, so
   `pf_privacy_notice()` is true for the first ~1.5 s. No code suppresses this;
   it is masked in practice because the portfolio screen is out of the cycle
   until a payload arrives. Whether the original spec intended this is unknown.
3. **Whether `u`, `ux` and `w` were meant to be hidden.** The code does not
   hide them (`draw_header()` ignores the mode). They are documented here as
   observed behaviour; the design intent behind leaving them visible is inferred.
4. **The design-intent wording (§8).** Derived from the code comments in
   `pf_privacy.{h,cpp}` and `ui_portfolio.cpp`; no separate rationale document
   exists in the repo.
5. **The exact original phrase set for the hidden-number table.** Rebuilt from
   the draw functions, not from a committed spec.
