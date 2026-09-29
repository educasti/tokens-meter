#include "oc_data.h"
#include <ArduinoJson.h>
#include <string.h>

// The daemon sends compact JSON (separators=(",", ":")), so the tag is
// normally the literal "k":"oc". Whitespace is tolerated anyway — the scan is
// still far cheaper than a full deserialize and only decides *routing*.
bool oc_is_payload(const char *json) {
    if (!json) return false;
    const char *p = strstr(json, "\"k\"");
    if (!p) return false;
    p += 3;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p[0] == '"' && p[1] == 'o' && p[2] == 'c' && p[3] == '"';
}

bool oc_parse(const char *json, OcData *out) {
    if (!json || !out) return false;

    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    if (strcmp(doc["k"] | "", "oc") != 0) return false;

    out->ok = doc["ok"] | false;
    strlcpy(out->src, doc["src"] | "none", sizeof(out->src));
    out->p5 = doc["p5"] | 0;
    out->r5 = doc["r5"] | -1;     // -1 = no active 5h window
    out->pw = doc["pw"] | 0;
    out->rw = doc["rw"] | 0;
    out->pm = doc["pm"] | 0;
    out->rm = doc["rm"] | 0;
    out->limited = strcmp(doc["st"] | "ok", "limited") == 0;

    out->t7 = doc["t7"] | 0L;
    out->tk = doc["tk"] | 0L;     // src=="none" only
    out->cd = doc["cd"] | 0.0f;
    out->c7 = doc["c7"] | 0.0f;
    strlcpy(out->m, doc["m"] | "", sizeof(out->m));
    out->ms = doc["ms"] | 0;
    out->a = doc["a"] | 0;
    strlcpy(out->ag, doc["ag"] | "", sizeof(out->ag));
    out->la = doc["la"] | 0L;

    out->valid = true;
    return true;
}
