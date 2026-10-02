#!/usr/bin/env bash
#
# Host test for firmware/src/ota_sig.{h,cpp} (P4; contract in
# design/ota-pull/SIGNING.md).
#
# Compiles ota_sig.cpp against the host mbedTLS and runs the known P-256 vector
# when <mbedtls/pk.h> is on the default include path (e.g. libmbedtls-dev).
# Otherwise it compiles only the harness — a compile-check of ota_sig.h — and
# the program prints SKIP and exits 0. CI-safe: no network, no pio.
#
# Usage: firmware/test/test_ota_sig/run_test.sh
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../../src"
out="${TMPDIR:-/tmp}/test_ota_sig"

# Probe for mbedTLS that can actually be linked (headers + libraries), matching
# test_main.cpp's __has_include(<mbedtls/pk.h>) check when it succeeds. If the
# link probe fails, force the harness into its SKIP path so the header-only case
# still compile-checks.
if printf '#include <mbedtls/pk.h>\nint main(void){return 0;}\n' \
	| g++ -std=c++17 -x c++ - -lmbedtls -lmbedcrypto -lmbedx509 -o /dev/null >/dev/null 2>&1; then
	g++ -std=c++17 -I "$src" "$here/test_main.cpp" "$src/ota_sig.cpp" \
		-lmbedtls -lmbedcrypto -lmbedx509 -o "$out"
else
	printf 'run_test: mbedTLS unavailable; compile-check only (SKIP)\n' >&2
	g++ -std=c++17 -I "$src" -DTEST_OTA_SIG_FORCE_SKIP \
		"$here/test_main.cpp" -o "$out"
fi

"$out"
