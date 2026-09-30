#pragma once
#include <Arduino.h>

// One BVC portfolio payload, specced in the portfolio design spec §6 and sent
// by the daemon on the same RX characteristic, tagged "k":"pf". Copied by value
// into the UI, so it stays small enough to live in internal SRAM on the
// PSRAM-free C6 boards. 240-byte ceiling on the wire.
//
// The payload is identical in private mode: hiding a number is a firmware
// concern, never a transport one (privacy spec §6).
#define PF_ROWS_MAX 4
#define PF_SYM_MAX  10

struct PfRow {
    char sym[PF_SYM_MAX + 1];  // without the .CL suffix, <= 10 chars
    long price;                // COP, per share — never abbreviated on screen
    int  pct;                  // hundredths of a percent, signed
    long contrib;              // COP, signed
};

struct PfData {
    bool valid;      // false until the first successful parse
    bool ok;         // false → error screen; e says which one
    char e[8];       // "nopos" | "nores" | "nonet"
    bool live;       // s == "l" (session open); false = "c" (closed)
    char t[9];       // "MMDDhhmm", Colombia time of the last price
    long mv;         // market value in COP, over the resolved positions
    long dc;         // day change in COP (sum of change x quantity)
    int  dp;         // day change in hundredths of a percent
    int  n;          // valid positions in the file
    int  u;          // positions without a price
    char ux[PF_SYM_MAX + 1];  // first such symbol, no .CL
    int  w;          // config warnings (duplicate, bad quantity)
    int  nr;         // rows actually carried in r
    PfRow r[PF_ROWS_MAX];
};

// Cheap pre-filter so the RX path can tell a portfolio payload from a Claude or
// an OpenCode one without deserializing: it always tags itself with "k":"pf".
bool pf_is_payload(const char *json);
// ArduinoJson 7 parse. False on a malformed document or a k != "pf" payload;
// on success every field of `out` is overwritten.
bool pf_parse(const char *json, PfData *out);
