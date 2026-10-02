#!/usr/bin/env python3
"""Host tests for tools/sign_firmware.py (P4, design/ota-pull/SIGNING.md).

Round-trip against real OpenSSL, no network and no repo private key:

* generate a throwaway P-256 key in a temp dir;
* sign a temp ``.bin`` with ``sign_firmware.py``;
* verify the base64 DER signature with ``openssl dgst -verify``;
* check ``--pubkey`` matches ``openssl pkey -pubout``;
* confirm tampered input / the wrong key / a non-P-256 key are refused.

Run: python -m pytest daemon/tests/test_sign_firmware.py -q
"""
import base64
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SIGN_PY = REPO / "tools" / "sign_firmware.py"

pytestmark = pytest.mark.skipif(
    shutil.which("openssl") is None, reason="openssl is required for signing"
)


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _openssl(*args: str) -> subprocess.CompletedProcess:
    proc = subprocess.run(["openssl", *args], capture_output=True)
    assert proc.returncode == 0, (
        f"openssl {' '.join(args)} failed: {proc.stderr.decode(errors='replace')}"
    )
    return proc


def _gen_p256_key(tmp_path: Path, name: str = "key.pem") -> Path:
    key = tmp_path / name
    _openssl("genpkey", "-algorithm", "EC",
             "-pkeyopt", "ec_paramgen_curve:P-256", "-out", str(key))
    return key


def _write_bin(tmp_path: Path, name: str = "firmware.bin", size: int = 4096) -> Path:
    blob = bytes((i * 37 + 11) % 256 for i in range(size))
    path = tmp_path / name
    path.write_bytes(blob)
    return path


def _pubkey_pem(key: Path) -> bytes:
    return _openssl("pkey", "-in", str(key), "-pubout").stdout


def _run_sign(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SIGN_PY), *args], capture_output=True, text=True,
    )


def _verify(bin_path: Path, pubkey: Path, sig_der: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["openssl", "dgst", "-sha256", "-verify", str(pubkey),
         "-signature", str(sig_der), str(bin_path)],
        capture_output=True, text=True,
    )


# ---------------------------------------------------------------------------
# signing round trip
# ---------------------------------------------------------------------------

def test_signed_bin_verifies_with_openssl(tmp_path):
    key = _gen_p256_key(tmp_path)
    binary = _write_bin(tmp_path)

    proc = _run_sign("--bin", str(binary), "--key", str(key))
    assert proc.returncode == 0, proc.stderr
    sig_b64 = proc.stdout.strip()
    assert sig_b64 and "\n" not in sig_b64

    sig_der = tmp_path / "sig.der"
    sig_der.write_bytes(base64.b64decode(sig_b64))

    pubkey = tmp_path / "pub.pem"
    pubkey.write_bytes(_pubkey_pem(key))

    verified = _verify(binary, pubkey, sig_der)
    assert verified.returncode == 0, verified.stderr
    assert "Verified OK" in verified.stdout


def test_out_file_matches_stdout(tmp_path):
    key = _gen_p256_key(tmp_path)
    binary = _write_bin(tmp_path)
    out = tmp_path / "sig.b64"

    proc = _run_sign("--bin", str(binary), "--key", str(key), "--out", str(out))

    assert proc.returncode == 0, proc.stderr
    assert out.read_text() == proc.stdout
    # It is the base64 of a DER ECDSA signature: 0x30 (SEQUENCE) first.
    assert base64.b64decode(proc.stdout.strip())[0] == 0x30


def test_pubkey_matches_openssl_pubout(tmp_path):
    key = _gen_p256_key(tmp_path)

    proc = _run_sign("--key", str(key), "--pubkey")

    assert proc.returncode == 0, proc.stderr
    assert proc.stdout == _pubkey_pem(key).decode()
    assert proc.stdout.startswith("-----BEGIN PUBLIC KEY-----")
    pub = tmp_path / "pub.pem"
    pub.write_text(proc.stdout)
    assert "prime256v1" in _openssl(
        "pkey", "-pubin", "-in", str(pub), "-text", "-noout",
    ).stdout.decode()
    assert "P-256" in _openssl(
        "pkey", "-pubin", "-in", str(pub), "-text", "-noout",
    ).stdout.decode()


# ---------------------------------------------------------------------------
# refusals / negative cases
# ---------------------------------------------------------------------------

def test_tampered_bin_fails_verification(tmp_path):
    key = _gen_p256_key(tmp_path)
    binary = _write_bin(tmp_path)
    sig_der = tmp_path / "sig.der"
    sig_der.write_bytes(base64.b64decode(
        _run_sign("--bin", str(binary), "--key", str(key)).stdout.strip()))
    pubkey = tmp_path / "pub.pem"
    pubkey.write_bytes(_pubkey_pem(key))

    tampered = tmp_path / "tampered.bin"
    data = bytearray(binary.read_bytes())
    data[-1] ^= 0xFF
    tampered.write_bytes(bytes(data))

    verified = _verify(tampered, pubkey, sig_der)
    assert verified.returncode != 0
    assert "Verified OK" not in verified.stdout


def test_wrong_key_fails_verification(tmp_path):
    key = _gen_p256_key(tmp_path, "key.pem")
    other = _gen_p256_key(tmp_path, "other.pem")
    binary = _write_bin(tmp_path)

    sig_der = tmp_path / "sig.der"
    sig_der.write_bytes(base64.b64decode(
        _run_sign("--bin", str(binary), "--key", str(key)).stdout.strip()))

    pubkey = tmp_path / "other.pub.pem"
    pubkey.write_bytes(_pubkey_pem(other))

    verified = _verify(binary, pubkey, sig_der)
    assert verified.returncode != 0
    assert "Verified OK" not in verified.stdout


def test_non_p256_key_is_refused(tmp_path):
    key = tmp_path / "ed25519.pem"
    _openssl("genpkey", "-algorithm", "ED25519", "-out", str(key))
    binary = _write_bin(tmp_path)

    proc = _run_sign("--bin", str(binary), "--key", str(key))

    assert proc.returncode != 0
    assert proc.stdout == ""
    assert "P-256" in proc.stderr


def test_bin_is_required_without_pubkey(tmp_path):
    key = _gen_p256_key(tmp_path)

    proc = _run_sign("--key", str(key))

    assert proc.returncode != 0
    assert proc.stdout == ""


def test_missing_key_is_refused(tmp_path):
    binary = _write_bin(tmp_path)

    proc = _run_sign("--bin", str(binary), "--key", str(tmp_path / "nope.pem"))

    assert proc.returncode != 0
    assert proc.stdout == ""


# ---------------------------------------------------------------------------
# ota-publish.sh --sign-key integration (manifest sig/sig_alg/key_id)
# ---------------------------------------------------------------------------

PUBLISH_SH = REPO / "design" / "ota-pull" / "deploy" / "ota-publish.sh"


def _publish(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["bash", str(PUBLISH_SH), *args], capture_output=True, text=True,
    )


def _valid_publish_args(binary: Path) -> list[str]:
    return [
        "--bin", str(binary),
        "--version", "1.2.3",
        "--board", "waveshare_amoled_216",
        "--dest", "user@host:/srv/firmware",
        "--dry-run",
        "--allow-dirty",
    ]


def test_publish_without_sign_key_has_no_signature_fields(tmp_path):
    import json

    binary = _write_bin(tmp_path)

    proc = _publish(*_valid_publish_args(binary))

    assert proc.returncode == 0, proc.stderr
    manifest = json.loads(proc.stdout)
    assert "sig" not in manifest
    assert "sig_alg" not in manifest
    assert "key_id" not in manifest


def test_publish_sign_key_embeds_a_verifiable_signature(tmp_path):
    import hashlib
    import json

    key = _gen_p256_key(tmp_path)
    binary = _write_bin(tmp_path)

    proc = _publish(*_valid_publish_args(binary), "--sign-key", str(key))

    assert proc.returncode == 0, proc.stderr
    manifest = json.loads(proc.stdout)
    assert manifest["sig_alg"] == "ecdsa-p256-sha256"
    assert manifest["sig"]

    # The signature is over the exact .bin and binds to this public key.
    sig_der = tmp_path / "sig.der"
    sig_der.write_bytes(base64.b64decode(manifest["sig"]))
    pubkey = tmp_path / "pub.pem"
    pubkey.write_bytes(_pubkey_pem(key))
    assert _verify(binary, pubkey, sig_der).returncode == 0

    # Default key_id is the first 16 hex of the SHA-256 of the public key DER.
    der = _openssl("pkey", "-in", str(key), "-pubout", "-outform", "DER").stdout
    assert manifest["key_id"] == hashlib.sha256(der).hexdigest()[:16]


def test_publish_explicit_key_id_is_recorded(tmp_path):
    import json

    key = _gen_p256_key(tmp_path)
    binary = _write_bin(tmp_path)

    proc = _publish(*_valid_publish_args(binary),
                    "--sign-key", str(key), "--key-id", "field-key-2026")

    assert proc.returncode == 0, proc.stderr
    assert json.loads(proc.stdout)["key_id"] == "field-key-2026"

