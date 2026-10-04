"""Generate ``firmware/src/certs/pinned_server_pem.h`` from the pinned server cert.

The pull-OTA source is reached by a bare public IP, so the firmware cannot pin
the Let's Encrypt / ISRG roots; it pins the update server's **self-signed**
certificate instead (design/ota-pull/DESIGN.md section 8). The real cert is a
local, untracked secret:

* ``firmware/certs/pinned_server.pem`` -- the frozen pinned certificate. Its
  subjectAltName carries the update host's IP address. It is gitignored and is
  never committed.
* ``firmware/src/certs/pinned_server_pem.h`` -- the generated C string, also
  gitignored. ``ota_pull.cpp`` includes it and passes it to
  ``WiFiClientSecure::setCACert()``.

It also generates the signed-update pin (design/ota-pull/SIGNING.md section 5)
from a second optional, untracked input:

* ``firmware/certs/signing_pubkey.pem`` -- the ECDSA P-256 public key to pin.
* ``firmware/src/certs/signing_pubkey_pem.h`` -- the generated
  ``SIGNING_PUBKEY_PEM`` C string. When the input is absent this string is
  **empty** (never a placeholder that fails the build): the pull flow reads that
  as "signing is not configured" and stays hash-only. When present, the pull
  flow requires a valid ECDSA P-256/SHA-256 signature before activation.

Two roles, one source of truth:

* PlatformIO pre-build extra script (``firmware/platformio.ini``):
  ``extra_scripts = pre:scripts/version.py, pre:scripts/gen_pinned_cert.py``.
  It runs before compilation so the ``#include`` always resolves.
* Standalone CLI, usable with no PlatformIO installed::

      python3 firmware/scripts/gen_pinned_cert.py
      python3 firmware/scripts/gen_pinned_cert.py --pem /path/to/cert.pem \
                                                  --out /tmp/pinned_server_pem.h

Security contract: this script **never** weakens verification and never fails
silently-open. If the real PEM is absent (a clean checkout or CI), it emits a
clearly-marked placeholder whose body is not parseable as a certificate, so
``setCACert()`` rejects it and the TLS handshake fails closed. The build still
succeeds. If the PEM is present but does not look like a PEM certificate, the
script warns and still emits it verbatim -- the device, not the build, is the
thing that must decide whether it trusts the peer.
"""

import argparse
import os
import sys

# Anchor the defaults to this file so the CLI works from any cwd. This file is
# ``firmware/scripts/gen_pinned_cert.py``, so the project dir is its parent.
# PlatformIO runs extra scripts via exec() in a context where ``__file__`` is
# not defined; there it passes PROJECT_DIR explicitly, so fall back to that.
try:
    _HERE = os.path.dirname(os.path.abspath(__file__))
except NameError:
    _HERE = None
_DEFAULT_PROJECT_DIR = (
    os.path.dirname(_HERE)
    if _HERE
    else (os.environ.get("PROJECT_DIR") or os.getcwd())
)

_CERT_REL = os.path.join("certs", "pinned_server.pem")
_OUT_REL = os.path.join("src", "certs", "pinned_server_pem.h")

# Signed-update pin (design/ota-pull/SIGNING.md section 5). A second, optional,
# untracked input: the public half of the ECDSA P-256 signing key. Unlike the
# TLS cert this has NO fail-closed placeholder -- when it is absent the header
# is generated with an EMPTY string, which the pull flow reads as "signing is
# not configured" and falls back to the hash-only path. Absence never fails the
# build (SIGNING.md section 4 rule 2).
_SIGN_REL = os.path.join("certs", "signing_pubkey.pem")
_SIGN_OUT_REL = os.path.join("src", "certs", "signing_pubkey_pem.h")

# The raw-string delimiter. PEM base64 (A-Za-z0-9+/=) and the placeholder text
# can never contain ``)PEM"`, so this terminator is unambiguous.
_DELIM = "PEM"

# A body containing non-base64 characters, so mbedtls_x509_crt_parse() fails and
# the handshake is refused. No BEGIN/END CERTIFICATE markers: parse_der() fails
# immediately. This is deliberate fail-closed behavior, not a disabled check.
_PLACEHOLDER_BODY = (
    "PLACEHOLDER: no pinned server certificate was found at "
    "%s\n"
    "TLS verification is NOT disabled; this placeholder does not parse as a "
    "certificate,\n"
    "so WiFiClientSecure::setCACert() will reject it and every pull-OTA "
    "handshake will fail\n"
    "closed until the operator installs the real cert and rebuilds.\n"
    % _CERT_REL
)


def _warn(msg):
    """Print a loud, greppable warning to stderr."""
    sys.stderr.write("\n" + "!" * 78 + "\n")
    sys.stderr.write("!! gen_pinned_cert.py: %s\n" % msg)
    sys.stderr.write("!" * 78 + "\n\n")


def _info(msg):
    sys.stderr.write("gen_pinned_cert: %s\n" % msg)


def _read_pem(path):
    """Return the PEM text, or ``None`` when it is absent or unreadable."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return fh.read()
    except OSError:
        return None


def _looks_like_pem(text):
    return (
        "-----BEGIN CERTIFICATE-----" in text
        and "-----END CERTIFICATE-----" in text
    )


def _looks_like_pubkey_pem(text):
    return (
        "-----BEGIN PUBLIC KEY-----" in text
        and "-----END PUBLIC KEY-----" in text
    )


def render_header(pem_text):
    """Render the C header for ``pem_text`` (``None`` -> placeholder)."""
    if pem_text is None:
        marker = True
        body = _PLACEHOLDER_BODY
    else:
        marker = False
        body = pem_text
        if not body.endswith("\n"):
            body += "\n"

    if marker:
        intro = (
            "// *** PLACEHOLDER -- NOT A REAL PINNED CERTIFICATE ***\n"
            "// certs/pinned_server.pem was not found at build time. This header\n"
            "// exists only so a clean checkout / CI still compiles. It does NOT\n"
            "// disable verification: the body below cannot be parsed as a\n"
            "// certificate, so setCACert() rejects it and TLS fails closed.\n"
            "// Install the real pinned cert and rebuild to enable pull-OTA.\n"
        )
    else:
        intro = (
            "// Real pinned server certificate (self-signed; the SAN carries the\n"
            "// update host's IP address). Source: certs/pinned_server.pem.\n"
            "// Verification stays ON: this PEM is passed to\n"
            "// WiFiClientSecure::setCACert(); setInsecure() is never called.\n"
        )

    return (
        "#pragma once\n"
        "// Generated by firmware/scripts/gen_pinned_cert.py -- do not edit by hand.\n"
        "// See design/ota-pull/DESIGN.md section 8 and src/ota_pull.cpp.\n"
        "//\n"
        + intro
        + "static const char PINNED_SERVER_PEM[] =\n"
        'R"' + _DELIM + "(\n"
        + body
        + ")" + _DELIM + '";\n'
    )


def render_signing_header(pem_text):
    """Render ``src/certs/signing_pubkey_pem.h`` for a public-key PEM.

    ``pem_text`` is ``None`` (absent) or empty -> an EMPTY ``SIGNING_PUBKEY_PEM``
    string, which the pull flow treats as "signing not configured" (hash-only).
    A real PEM is embedded verbatim; the device parses it with mbedTLS.
    """
    if not pem_text or not pem_text.strip():
        intro = (
            "// No signing public key was pinned at build time. SIGNING_PUBKEY_PEM\n"
            "// is empty, so the pull flow keeps its pre-P4 hash-only behavior (no\n"
            "// signature required). Install certs/signing_pubkey.pem and rebuild to\n"
            "// require a valid ECDSA P-256/SHA-256 signature before activation.\n"
            "// Source: certs/signing_pubkey.pem (untracked; see SIGNING.md).\n"
        )
        return (
            "#pragma once\n"
            "// Generated by firmware/scripts/gen_pinned_cert.py -- do not edit by hand.\n"
            "// See design/ota-pull/SIGNING.md section 5 and src/ota_pull.cpp.\n"
            "//\n"
            + intro
            + 'static const char SIGNING_PUBKEY_PEM[] = "";\n'
        )

    body = pem_text if pem_text.endswith("\n") else pem_text + "\n"
    intro = (
        "// Pinned ECDSA P-256 signing public key (SubjectPublicKeyInfo PEM).\n"
        "// Source: certs/signing_pubkey.pem (untracked; only the public half is\n"
        "// committed into the build). The pull flow requires a manifest signature\n"
        "// that verifies under this key before it activates an update.\n"
    )
    return (
        "#pragma once\n"
        "// Generated by firmware/scripts/gen_pinned_cert.py -- do not edit by hand.\n"
        "// See design/ota-pull/SIGNING.md section 5 and src/ota_pull.cpp.\n"
        "//\n"
        + intro
        + "static const char SIGNING_PUBKEY_PEM[] =\n"
        'R"' + _DELIM + "(\n"
        + body
        + ")" + _DELIM + '";\n'
    )


def _write_if_changed(path, content):
    """Write ``content`` unless the file already matches. Returns True if written."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            if fh.read() == content:
                return False
    except OSError:
        pass

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(content)
    os.replace(tmp, path)
    return True


def generate(project_dir, pem_path=None, out_path=None, quiet=False,
             sign_pem_path=None, sign_out_path=None):
    """Generate both headers. Returns the pinned-server header path.

    Raises ``OSError`` only if a header cannot be written -- a missing PINNED
    SERVER cert is handled with the fail-closed placeholder, and a missing
    SIGNING key is handled with an empty string, never an exception. Absence of
    either input never fails the build.
    """
    pem_path = pem_path or os.path.join(project_dir, _CERT_REL)
    out_path = out_path or os.path.join(project_dir, _OUT_REL)
    sign_pem_path = sign_pem_path or os.path.join(project_dir, _SIGN_REL)
    sign_out_path = sign_out_path or os.path.join(project_dir, _SIGN_OUT_REL)

    pem_text = _read_pem(pem_path)
    if pem_text is None:
        _warn(
            "%s not found -- writing a FAIL-CLOSED placeholder to %s. "
            "Pull-OTA will compile but every TLS handshake will fail until the "
            "real pinned certificate is installed." % (pem_path, out_path)
        )
    elif not _looks_like_pem(pem_text):
        _warn(
            "%s does not contain a PEM certificate block -- embedding it "
            "verbatim; the device will reject it at handshake time." % pem_path
        )
    elif not quiet:
        _info("pinning %s -> %s" % (pem_path, out_path))

    written = _write_if_changed(out_path, render_header(pem_text))
    if not quiet:
        _info("%s %s" % ("wrote" if written else "unchanged", out_path))

    # Signed-update pin: optional, hash-only when absent. Never fail-closed and
    # never fatal -- an absent key just leaves SIGNING_PUBKEY_PEM empty.
    sign_text = _read_pem(sign_pem_path)
    if not sign_text or not sign_text.strip():
        if not quiet:
            _info("no signing key at %s -- emitting hash-only firmware" % sign_pem_path)
    elif not _looks_like_pubkey_pem(sign_text):
        _warn(
            "%s does not contain a PUBLIC KEY PEM block -- embedding it "
            "verbatim; the device will reject every signature." % sign_pem_path
        )
    elif not quiet:
        _info("pinning signing key %s -> %s" % (sign_pem_path, sign_out_path))

    sign_written = _write_if_changed(sign_out_path, render_signing_header(sign_text))
    if not quiet:
        _info("%s %s" % ("wrote" if sign_written else "unchanged", sign_out_path))
    return out_path


# ---- PlatformIO integration -------------------------------------------------
# ``Import`` exists only inside a PlatformIO/SCons extra script; NameError on
# the plain CLI path is expected and handled here.
#
# IMPORTANT: SCons's ``Import("env")`` does NOT return the environment. Like
# ``Export``, it injects the name into this module's globals and returns None,
# so it must be called at module scope and the injected ``env`` global read
# afterwards -- exactly as version.py and lvgl_psram.py do. (Relying on the
# return value here made this pre-build step a silent no-op under PlatformIO,
# so the generated headers only ever appeared when the CLI was run by hand.)
try:
    Import("env")  # noqa: F821  (injected by PlatformIO)
except NameError:
    env = None
except Exception:
    env = None


def _run_for_platformio(pio_env):
    project_dir = None
    try:
        project_dir = pio_env["PROJECT_DIR"]
    except Exception:
        pass
    try:
        generate(project_dir or _DEFAULT_PROJECT_DIR)
    except OSError as exc:
        # The build will fail on the missing include anyway; surface why.
        _warn("could not write the generated header: %s" % exc)
        raise


if env is not None:
    _run_for_platformio(env)


# ---- CLI --------------------------------------------------------------------
def _main(argv):
    parser = argparse.ArgumentParser(
        description=(
            "Generate src/certs/pinned_server_pem.h from the pinned, untracked "
            "server certificate (certs/pinned_server.pem)."
        )
    )
    parser.add_argument(
        "--project",
        default=os.environ.get("PROJECT_DIR") or _DEFAULT_PROJECT_DIR,
        help="firmware project dir (default: the parent of this script)",
    )
    parser.add_argument(
        "--pem", default=None,
        help="pinned PEM input (default: <project>/certs/pinned_server.pem)",
    )
    parser.add_argument(
        "--out", default=None,
        help="generated header output (default: <project>/src/certs/pinned_server_pem.h)",
    )
    parser.add_argument(
        "--sign-pem", default=None,
        help="signing public key input (default: <project>/certs/signing_pubkey.pem)",
    )
    parser.add_argument(
        "--sign-out", default=None,
        help="generated signing header output "
             "(default: <project>/src/certs/signing_pubkey_pem.h)",
    )
    parser.add_argument(
        "-q", "--quiet", action="store_true",
        help="suppress the informational lines (warnings still print)",
    )
    args = parser.parse_args(argv)

    try:
        out = generate(args.project, args.pem, args.out, args.quiet,
                       args.sign_pem, args.sign_out)
    except OSError as exc:
        sys.stderr.write("gen_pinned_cert: failed to write %s: %s\n"
                         % (args.out or args.project, exc))
        return 1
    if args.quiet is False:
        print(out)
    return 0


if env is None and globals().get("__name__") == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
