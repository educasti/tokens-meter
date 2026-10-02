#!/usr/bin/env bash
#
# ota-publish.sh — stage and atomically publish one firmware binary + manifest
# for pull-based automatic OTA.
#
# Design source of truth: design/ota-pull/DESIGN.md section 6 (manifest, URL
# layout, caching) and section 6.5 (publish guarantee).
#
# STATUS: the P0 publisher (design/ota-pull/IMPL.md section 3). It is safe to
# run by hand and is intended to be called from CI in P2. It is the only writer
# of the firmware directory (DESIGN section 6.5).
#
# Usage:
#   design/ota-pull/deploy/ota-publish.sh \
#       --bin firmware/.pio/build/waveshare_amoled_216/firmware.bin \
#       --version 0.2.0 \
#       [--board waveshare_amoled_216] \
#       [--dest user@vm.example.com:/srv/firmware] \
#       [--keep 5] [--min-from 0.1.0] [--mandatory] [--notes "..."] \
#       [--sign-key /path/to/p256.pem] [--key-id ID] \
#       [--allow-dirty] [--dry-run]
#
# Environment overrides:
#   OTA_DEST   default --dest (keep credentials out of the command line)
#   OTA_KEEP   default --keep
#
# Requires: sha256sum (or shasum), stat, python3, ssh, rsync. --sign-key also
# requires openssl; the private key stays outside the repo (design/ota-pull/
# SIGNING.md) and only its public counterpart is pinned in firmware.
#
# Output contract:
#   --dry-run writes the rendered manifest JSON to stdout and nothing else on
#   stdout, so `... --dry-run | python3 -c 'import json,sys; json.load(sys.stdin)'`
#   works; every human/log line goes to stderr.
#
# What it does (DESIGN section 6.5):
#   1. computes sha256 + size of the built .bin;
#   2. renders the manifest (same version the firmware stamps via
#      firmware/scripts/version.py);
#   3. uploads to <dest>/<board>/.tmp/ and renames into place, binary first,
#      manifest last, so a reader never sees a manifest pointing at a missing
#      binary;
#   4. keeps the last N versions in <dest>/<board>/ and prunes older ones;
#   5. symlinks the flat alias for the single-board convenience path.
#
# shellcheck shell=bash
set -euo pipefail

die() { printf 'ota-publish: error: %s\n' "$*" >&2; exit 1; }
info() { printf 'ota-publish: %s\n' "$*" >&2; }

# tools/sign_firmware.py, resolved relative to this script so the caller can be
# in any directory. SIG_ALG is the manifest's sig_alg (design/ota-pull/SIGNING.md).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIGN_FIRMWARE="$SCRIPT_DIR/../../../tools/sign_firmware.py"
SIG_ALG="ecdsa-p256-sha256"

# sha256sum with a shasum fallback (DESIGN section 6.2); also used for the
# public-key fingerprint that becomes the default key_id.
sha256_file() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum -- "$1" | awk '{print $1}'
	else
		shasum -a 256 -- "$1" | awk '{print $1}'
	fi
}

usage() {
	cat <<'EOF'
ota-publish.sh — stage and atomically publish one firmware binary + manifest
for pull-based automatic OTA.

Usage:
  design/ota-pull/deploy/ota-publish.sh \
      --bin PATH --version X.Y.Z \
      [--board NAME] [--dest user@host:/path] \
      [--keep N] [--min-from X.Y.Z] [--mandatory] \
      [--notes TEXT] [--sign-key PATH] [--key-id ID] \
      [--allow-dirty] [--dry-run] [-h|--help]

Options:
  --bin PATH        built .bin to publish (required)
  --version X.Y.Z   semver version to stamp into the manifest (required)
  --board NAME      board id (default: waveshare_amoled_216)
  --dest DEST       user@host:/path remote target (default: $OTA_DEST or a
                    placeholder; keep credentials out of the command line)
  --keep N          number of versions to retain per board (default: $OTA_KEEP or 5)
  --min-from X.Y.Z  manifest min_from (optional; omitted when unset)
  --mandatory       mark the manifest mandatory
  --notes TEXT      manifest notes (optional; omitted when empty)
  --sign-key PATH   sign the binary with this ECDSA P-256 private key and add
                    sig / sig_alg / key_id to the manifest (key stays out of
                    the repo; see design/ota-pull/SIGNING.md)
  --key-id ID       key_id to record with the signature (default: first 16 hex
                    of the SHA-256 of the public key); requires --sign-key
  --allow-dirty     publish even when the git tree is dirty
  --dry-run         render the manifest to stdout without touching the remote
                    (logs go to stderr; stdout is JSON only)
  -h, --help        show this help and exit

Environment:
  OTA_DEST          default for --dest
  OTA_KEEP          default for --keep
EOF
}

# --- defaults (placeholders; never commit a real host or credential) ---------
BIN=""
VERSION=""
BOARD="waveshare_amoled_216"
DEST="${OTA_DEST:-user@vm.example.com:/srv/firmware}"
KEEP="${OTA_KEEP:-5}"
MIN_FROM=""
MANDATORY="false"
NOTES=""
SIGN_KEY=""
KEY_ID=""
SIGN_B64=""
ALLOW_DIRTY=0
DRY_RUN=0

# --- argument parsing --------------------------------------------------------
while [ $# -gt 0 ]; do
	case "$1" in
		--bin)         [ $# -ge 2 ] || die "$1 requires a value"; BIN="$2"; shift 2 ;;
		--version)     [ $# -ge 2 ] || die "$1 requires a value"; VERSION="$2"; shift 2 ;;
		--board)       [ $# -ge 2 ] || die "$1 requires a value"; BOARD="$2"; shift 2 ;;
		--dest)        [ $# -ge 2 ] || die "$1 requires a value"; DEST="$2"; shift 2 ;;
		--keep)        [ $# -ge 2 ] || die "$1 requires a value"; KEEP="$2"; shift 2 ;;
		--min-from)    [ $# -ge 2 ] || die "$1 requires a value"; MIN_FROM="$2"; shift 2 ;;
		--mandatory)   MANDATORY="true"; shift ;;
		--notes)       [ $# -ge 2 ] || die "$1 requires a value"; NOTES="$2"; shift 2 ;;
		--sign-key)    [ $# -ge 2 ] || die "$1 requires a value"; SIGN_KEY="$2"; shift 2 ;;
		--key-id)      [ $# -ge 2 ] || die "$1 requires a value"; KEY_ID="$2"; shift 2 ;;
		--allow-dirty) ALLOW_DIRTY=1; shift ;;
		--dry-run)     DRY_RUN=1; shift ;;
		-h|--help)     usage; exit 0 ;;
		--*)           die "unknown argument: $1" ;;
		*)             die "unexpected argument: $1" ;;
	esac
done

# --- validation --------------------------------------------------------------
[ -n "$BIN" ]     || die "--bin is required"
[ -n "$VERSION" ] || die "--version is required"
[ -f "$BIN" ]     || die "binary not found: $BIN"
# Keep a leading dash from being read as an option by sha256sum/stat/cp.
case "$BIN" in -*) BIN="./$BIN" ;; esac
printf '%s' "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
	|| die "version must be semver X.Y.Z (got: $VERSION)"
printf '%s' "$BOARD" | grep -Eq '^[A-Za-z0-9_-]+$' \
	|| die "board must match [A-Za-z0-9_-]+ (got: $BOARD)"
printf '%s' "$KEEP" | grep -Eq '^[1-9][0-9]*$' || die "--keep must be >= 1"
if [ -n "$MIN_FROM" ]; then
	printf '%s' "$MIN_FROM" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
		|| die "--min-from must be semver X.Y.Z (got: $MIN_FROM)"
fi
printf '%s' "$DEST" | grep -Eq '^[^:]+:[^:]+$' \
	|| die "--dest must look like user@host:/path (got: $DEST)"
if [ -n "$KEY_ID" ] && [ -z "$SIGN_KEY" ]; then
	die "--key-id requires --sign-key"
fi
if [ -n "$SIGN_KEY" ]; then
	[ -f "$SIGN_KEY" ] || die "signing key not found: $SIGN_KEY"
	command -v openssl >/dev/null 2>&1 || die "--sign-key requires openssl on PATH"
	[ -f "$SIGN_FIRMWARE" ] || die "signer not found: $SIGN_FIRMWARE"
fi

# --- refuse to publish a dirty tree unless told otherwise --------------------
if [ "$ALLOW_DIRTY" -ne 1 ] && command -v git >/dev/null 2>&1 \
	&& git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
	if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
		die "working tree is dirty; commit, or pass --allow-dirty"
	fi
fi

# --- hash + size (DESIGN section 6.2) ----------------------------------------
SHA256="$(sha256_file "$BIN")"
SIZE="$(stat -c '%s' "$BIN" 2>/dev/null || stat -f '%z' "$BIN")"
[ "${#SHA256}" -eq 64 ] || die "unexpected sha256 length: ${#SHA256}"
printf '%s' "$SIZE" | grep -Eq '^[0-9]+$' || die "unexpected size: $SIZE"

BINNAME="clawdmeter-${VERSION}.bin"
RELEASED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# --- stage locally -----------------------------------------------------------
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/ota-publish.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
cp "$BIN" "$STAGE/$BINNAME"

# --- sign after hashing (design/ota-pull/SIGNING.md) -------------------------
# Detached ECDSA P-256/SHA-256 over the exact .bin bytes. The private key is
# supplied by path (CI secret / VM keystore) and never enters the repo; the
# captured base64 DER becomes the manifest's `sig`. Without --sign-key this is
# skipped and the manifest is byte-for-byte what it was before.
if [ -n "$SIGN_KEY" ]; then
	SIGN_B64="$(python3 "$SIGN_FIRMWARE" --bin "$BIN" --key "$SIGN_KEY")" \
		|| die "signing failed"
	[ -n "$SIGN_B64" ] || die "signing produced an empty signature"
	if [ -z "$KEY_ID" ]; then
		openssl pkey -in "$SIGN_KEY" -pubout -outform DER \
			-out "$STAGE/ota_pubkey.der" 2>/dev/null \
			|| die "could not export the public key for key_id"
		KEY_ID="$(sha256_file "$STAGE/ota_pubkey.der" | cut -c1-16)"
	fi
	info "signed $BINNAME ($SIG_ALG, key_id $KEY_ID)"
fi

# Render the manifest with python3 so JSON escaping of --notes is correct.
# The field set is exactly design/ota-pull/DESIGN.md section 6.1/6.2; the
# signature fields (6.1 + SIGNING.md) are added only when --sign-key is set.
python3 - "$STAGE/manifest.json" \
	"$BOARD" "$VERSION" "$BINNAME" "$SHA256" "$SIZE" \
	"$MIN_FROM" "$MANDATORY" "$RELEASED_AT" "$NOTES" \
	"$SIGN_B64" "$SIG_ALG" "$KEY_ID" <<'PY'
import json, sys
(out, board, version, url, sha, size, min_from, mandatory, released, notes,
 sig, sig_alg, key_id) = sys.argv[1:14]
manifest = {
    "schema_version": 1,
    "board": board,
    "version": version,
    "url": url,
    "sha256": sha,
    "size": int(size),
    "mandatory": mandatory == "true",
    "released_at": released,
}
if min_from:
    manifest["min_from"] = min_from
if notes:
    manifest["notes"] = notes
if sig:
    manifest["sig"] = sig
    manifest["sig_alg"] = sig_alg
    manifest["key_id"] = key_id
with open(out, "w", encoding="utf-8") as fh:
    json.dump(manifest, fh, indent=2, sort_keys=True)
    fh.write("\n")
PY

info "staged $BINNAME ($SIZE bytes, sha256 $SHA256)"
info "manifest rendered at $STAGE/manifest.json"

# --- publish -----------------------------------------------------------------
HOST="${DEST%%:*}"
DIR="${DEST#*:}"
REMOTE_BOARD="$DIR/$BOARD"
REMOTE_TMP="$REMOTE_BOARD/.tmp"

# Quote a value for a remote bash script. `printf %q` emits bash-safe quoting,
# so remote paths containing spaces (or quotes) survive the ssh hop intact.
q() { printf '%q' "$1"; }

if [ "$DRY_RUN" -eq 1 ]; then
	info "dry-run: would upload to $HOST:$REMOTE_BOARD and keep last $KEEP"
	info "dry-run: would update the flat alias under $HOST:$DIR/"
	cat "$STAGE/manifest.json"
	exit 0
fi

# Create the remote directories before rsync (the atomic swap runs afterwards).
printf 'mkdir -p %s %s\n' "$(q "$REMOTE_TMP")" "$(q "$REMOTE_BOARD")" \
	| ssh "$HOST" bash -s

# Upload to the temp dir first; the remote script then renames within the same
# filesystem so the swap is atomic. Binary first, manifest last: the manifest
# is the pointer. `--protect-args` keeps spaces in REMOTE_TMP from being
# re-parsed by the remote shell.
rsync -a --partial --protect-args "$STAGE/$BINNAME"     "$HOST:$REMOTE_TMP/"
rsync -a --partial --protect-args "$STAGE/manifest.json" "$HOST:$REMOTE_TMP/"

# The remote swap script is generated with every literal shell-quoted, so the
# remote side never re-interprets a path with spaces or metacharacters.
REMOTE_SCRIPT="$(cat <<REMOTE
set -e
remote_board=$(q "$REMOTE_BOARD")
remote_dir=$(q "$DIR")
remote_tmp=$(q "$REMOTE_TMP")
binname=$(q "$BINNAME")
board=$(q "$BOARD")
keep=$KEEP
mv -- "\$remote_tmp/\$binname"      "\$remote_board/\$binname"
mv -- "\$remote_tmp/manifest.json" "\$remote_board/manifest.json"
# Flat alias for the single-board convenience path (Caddy also rewrites
# /firmware/manifest.json -> the canonical path; the symlink covers tools
# and the flat binary URL).
ln -sfn -- "\$board/\$binname"      "\$remote_dir/\$binname"
ln -sfn -- "\$board/manifest.json" "\$remote_dir/manifest.json"
# Prune: keep the newest N binaries and their flat symlinks.
cd "\$remote_board"
ls -1t -- *.bin 2>/dev/null | tail -n +\$((keep + 1)) | while IFS= read -r old; do
	rm -f -- "\$old" "\$remote_dir/\$old"
done
REMOTE
)"
printf '%s\n' "$REMOTE_SCRIPT" | ssh "$HOST" bash -s

info "published https://<IP>/firmware/$BOARD/manifest.json (version $VERSION)"
info "flat alias https://<IP>/firmware/manifest.json -> $BOARD/manifest.json"
