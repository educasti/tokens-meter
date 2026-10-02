#pragma once
#include <stdbool.h>
#include <limits.h>

// Strict semver MAJOR.MINOR.PATCH comparison, header-only and dependency-free
// so it compiles on the host with plain g++ (IMPL.md section 2.1 /
// DESIGN.md section 5.5). No Arduino or ArduinoJson.
//
// Format is exactly `X.Y.Z` with an optional single leading 'v'; each part is
// one or more ASCII digits and fits in a non-negative int. Pre-release and
// build-metadata suffixes (`1.2.3-rc1`), empty parts, trailing text and
// overflow are all parse failures.

// Negative sentinel returned by semver_cmp() when either operand does not
// parse. It is deliberately not -1 or 1 so a caller can never confuse a parse
// failure with an ordering result; callers must reject, never treat as equal.
#define SEMVER_INVALID (-2)

// Parse "X.Y.Z" (optional leading 'v') into three non-negative ints. Returns
// false on any non-numeric, missing, extra or out-of-range part.
static inline bool semver_parse(const char* s, int out[3]) {
    if (!s || !out) return false;

    const char* p = s;
    if (*p == 'v') p++;
    if (*p == '\0') return false;   // "" or bare "v"

    for (int i = 0; i < 3; i++) {
        if (*p < '0' || *p > '9') return false;   // empty or non-numeric part
        long v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            if (v > (long)INT_MAX) return false;  // overflow
            p++;
        }
        out[i] = (int)v;

        if (i < 2) {
            if (*p != '.') return false;          // too few parts
            p++;
        }
    }
    if (*p != '\0') return false;                 // "1.2.3.4", "1.2.3-rc1", ...
    return true;
}

// -1 / 0 / 1 ordering. A parse failure of either side yields SEMVER_INVALID
// (negative), so an unparsable version is never equal to anything.
static inline int semver_cmp(const char* a, const char* b) {
    int va[3], vb[3];
    if (!semver_parse(a, va) || !semver_parse(b, vb)) return SEMVER_INVALID;

    for (int i = 0; i < 3; i++) {
        if (va[i] < vb[i]) return -1;
        if (va[i] > vb[i]) return 1;
    }
    return 0;
}
