#pragma once
#include <Arduino.h>

// One OpenCode payload, specced in SPEC.md §8 and sent by
// daemon/opencode_collector.py. Copied by value into the UI, so it stays small
// enough to live in internal SRAM on the PSRAM-free C6 boards.
struct OcData {
    bool valid;          // false until first successful parse
    bool ok;
    char src[5];         // "api" | "est" | "none"
    int  p5, r5;         // 5h percent, minutes to reset (-1 = no active window)
    int  pw, rw;         // weekly
    int  pm, rm;         // monthly
    bool limited;        // "st":"limited"
    long t7;             // 7-day tokens, thousands
    long tk; float cd;   // src=="none": today tokens (thousands), today USD
    float c7;            // src=="none": 7-day USD
    char m[16]; int ms;  // top model (ASCII) + share %
    int  a; char ag[12]; long la;  // active sessions, agent, seconds since last activity
};

// Cheap pre-filter so the RX path can tell an OpenCode payload from a Claude
// one without deserializing: OpenCode always tags itself with "k":"oc".
bool oc_is_payload(const char *json);
// ArduinoJson 7 parse. False on a malformed document or a k != "oc" payload;
// on success every field of `out` is overwritten.
bool oc_parse(const char *json, OcData *out);
