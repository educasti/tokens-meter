#include "pf_data.h"
#include <ArduinoJson.h>
#include <string.h>

// The daemon sends compact JSON (separators=(",", ":")), so the tag is
// normally the literal "k":"pf". Whitespace is tolerated anyway — the scan is
// still far cheaper than a full deserialize and only decides *routing*.
bool pf_is_payload(const char *json) {
    if (!json) return false;
    const char *p = strstr(json, "\"k\"");
    if (!p) return false;
    p += 3;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p[0] == '"' && p[1] == 'p' && p[2] == 'f' && p[3] == '"';
}

bool pf_parse(const char *json, PfData *out) {
    if (!json || !out) return false;

    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    if (strcmp(doc["k"] | "", "pf") != 0) return false;

    out->ok   = doc["ok"] | false;
    strlcpy(out->e, doc["e"] | "nores", sizeof(out->e));
    // "s" is derived from the timestamp, not from a calendar: the provider's
    // last quote decides whether the session is open or closed.
    out->live = (strcmp(doc["s"] | "c", "l") == 0);
    strlcpy(out->t, doc["t"] | "", sizeof(out->t));
    out->mv = doc["mv"] | 0L;
    out->dc = doc["dc"] | 0L;
    out->dp = doc["dp"] | 0;
    out->n  = doc["n"]  | 0;
    out->u  = doc["u"]  | 0;
    strlcpy(out->ux, doc["ux"] | "", sizeof(out->ux));
    out->w  = doc["w"]  | 0;

    // r: [sym, precio, pct, contrib]. The .CL suffix is stripped for
    // transport only; the file and the BVC check keep the full symbol.
    out->nr = 0;
    for (JsonArrayConst r : doc["r"].as<JsonArrayConst>()) {
        if (out->nr >= PF_ROWS_MAX) break;
        if (r.size() < 4) continue;   // never draw a half-row
        PfRow *row = &out->r[out->nr++];
        strlcpy(row->sym, r[0] | "", sizeof(row->sym));
        row->price   = r[1] | 0L;
        row->pct     = r[2] | 0;
        row->contrib = r[3] | 0L;
    }

    out->valid = true;
    return true;
}
