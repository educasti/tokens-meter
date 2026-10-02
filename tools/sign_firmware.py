#!/usr/bin/env python3
"""Sign a firmware image with an ECDSA P-256 private key (SHA-256).

P4 of design/ota-pull/IMPL.md section 7; the manifest/verification contract is
design/ota-pull/SIGNING.md. The detached signature is ECDSA over SHA-256 of the
exact ``.bin`` bytes, DER-encoded and base64-encoded for the manifest's ``sig``
field (``sig_alg`` = ``ecdsa-p256-sha256``).

The private key never enters the repository: it is passed as a path at publish
time (CI secret or the VM's keystore) and only its public key is pinned in
firmware. ``--pubkey`` prints that public key PEM.

Usage:
  tools/sign_firmware.py --bin firmware.bin --key key.pem
      # base64 DER signature on stdout (the manifest's `sig`)
  tools/sign_firmware.py --bin firmware.bin --key key.pem --out sig.b64
      # same, also written to sig.b64
  tools/sign_firmware.py --key key.pem --pubkey
      # the public key PEM to pin in firmware, on stdout

Requires ``openssl`` on PATH. No Python crypto dependency.
"""
from __future__ import annotations

import argparse
import base64
import subprocess
import sys
from pathlib import Path
from typing import NoReturn

# `openssl pkey -text` prints both of these for a P-256 key (OpenSSL 3.x).
P256_MARKERS = ("prime256v1", "P-256")


def die(msg: str) -> NoReturn:
    print(f"sign_firmware: error: {msg}", file=sys.stderr)
    raise SystemExit(2)


def openssl(*args: str) -> subprocess.CompletedProcess:
    """Run ``openssl`` capturing output; die if the binary is missing."""
    try:
        return subprocess.run(["openssl", *args], capture_output=True, check=False)
    except FileNotFoundError:
        die("openssl not found on PATH")


def assert_p256_private_key(key: Path) -> None:
    """Refuse anything that is not an ECDSA P-256 private key.

    The firmware verifier is pinned to P-256, so a mis-typed key (RSA, Ed25519,
    P-384) would produce a manifest the device can never accept. Fail early.
    """
    proc = openssl("pkey", "-in", str(key), "-text", "-noout")
    if proc.returncode != 0:
        die(f"cannot read private key {key}: {proc.stderr.decode(errors='replace').strip()}")
    text = proc.stdout.decode(errors="replace")
    if not any(marker in text for marker in P256_MARKERS):
        die(f"{key} is not an ECDSA P-256 key (expected P-256 / prime256v1)")


def sign_bin(bin_path: Path, key: Path) -> bytes:
    """Return the DER-encoded ECDSA P-256/SHA-256 signature over ``bin_path``."""
    proc = openssl("dgst", "-sha256", "-sign", str(key), str(bin_path))
    if proc.returncode != 0:
        die(f"openssl signing failed: {proc.stderr.decode(errors='replace').strip()}")
    if not proc.stdout:
        die("openssl produced an empty signature")
    return proc.stdout


def public_key_pem(key: Path) -> str:
    """Return the SubjectPublicKeyInfo PEM for ``key``."""
    proc = openssl("pkey", "-in", str(key), "-pubout")
    if proc.returncode != 0:
        die(f"could not export public key from {key}: "
            f"{proc.stderr.decode(errors='replace').strip()}")
    return proc.stdout.decode("ascii")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="sign_firmware.py",
        description="Sign a firmware .bin with ECDSA P-256 + SHA-256 (detached, base64 DER).",
    )
    parser.add_argument("--bin", type=Path, help="built firmware .bin to sign")
    parser.add_argument("--key", type=Path, required=True,
                        help="ECDSA P-256 private key PEM (never committed)")
    parser.add_argument("--out", type=Path,
                        help="also write the base64 signature to this file")
    parser.add_argument("--pubkey", action="store_true",
                        help="print the public key PEM (for pinning) and exit")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    if not args.key.is_file():
        die(f"private key not found: {args.key}")
    assert_p256_private_key(args.key)

    if args.pubkey:
        sys.stdout.write(public_key_pem(args.key))
        return 0

    if args.bin is None:
        die("--bin is required unless --pubkey is given")
    if not args.bin.is_file():
        die(f"firmware binary not found: {args.bin}")

    signature_b64 = base64.b64encode(sign_bin(args.bin, args.key)).decode("ascii")
    if args.out is not None:
        args.out.write_text(signature_b64 + "\n", encoding="ascii")
    sys.stdout.write(signature_b64 + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
