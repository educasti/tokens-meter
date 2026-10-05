"""Inject the backend-WiFi usage-pull config as build macros.

The device pulls Claude usage numbers from the owner's backend over HTTPS
(design/backend-wifi/PHASE1-CONTRACT.md section 7). The endpoint and the
per-device bearer token are local secrets and are never committed:

* ``firmware/certs/usage_backend.json`` -- untracked, e.g.::

      {"url": "https://<vm-ip>/api/usage", "token": "<minted device token>"}

* or the environment variables ``USAGE_BACKEND_URL`` / ``USAGE_DEVICE_TOKEN``
  (they win over the file, handy for CI and one-off builds).

Both values present -> ``-DUSAGE_BACKEND_URL="..."`` and
``-DUSAGE_DEVICE_TOKEN="..."`` (and optionally ``-DUSAGE_POLL_S=<n>`` from a
``"poll_s"`` key / ``USAGE_POLL_S`` env) are appended to the compile flags.
Anything missing or malformed -> no macros are defined and ``usage_pull.cpp``
compiles to a no-op, so the BLE path is untouched. The script never fails the
build and never prints the token.

Mirrors ``gen_pinned_cert.py``: a PlatformIO pre-build extra script
(``extra_scripts = pre:scripts/gen_usage_config.py``) and a standalone check::

    python3 firmware/scripts/gen_usage_config.py
"""

import json
import os
import re
import sys

try:
    _HERE = os.path.dirname(os.path.abspath(__file__))
except NameError:  # PlatformIO exec()s extra scripts without __file__
    _HERE = None
_DEFAULT_PROJECT_DIR = (
    os.path.dirname(_HERE)
    if _HERE
    else (os.environ.get("PROJECT_DIR") or os.getcwd())
)

_CONFIG_REL = os.path.join("certs", "usage_backend.json")

# Bearer tokens are base64url; allow the standard base64 / JWT-ish alphabet.
_TOKEN_RE = re.compile(r"^[A-Za-z0-9._~+/=-]{8,256}$")
# https only, no quotes/spaces/backslashes that would break the C string.
_URL_RE = re.compile(r"^https://[^\s\"'\\]{3,200}$")

POLL_MIN_S = 30
POLL_MAX_S = 86400


def _warn(msg):
    sys.stderr.write("gen_usage_config: WARNING: %s\n" % msg)


def _info(msg):
    sys.stderr.write("gen_usage_config: %s\n" % msg)


def _read_file(path):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except FileNotFoundError:
        return {}
    except (OSError, ValueError) as exc:
        _warn("could not read %s: %s" % (path, exc.__class__.__name__))
        return {}
    return data if isinstance(data, dict) else {}


def load_config(project_dir, environ=None):
    """Return ``{"url", "token", "poll_s"}`` (strings / int / None), validated.

    Returns ``None`` when the feature is off (nothing set) or the config is
    unusable; a half-set or malformed config warns (never echoing the token).
    """
    environ = os.environ if environ is None else environ
    file_cfg = _read_file(os.path.join(project_dir, _CONFIG_REL))

    url = (environ.get("USAGE_BACKEND_URL") or file_cfg.get("url") or "").strip()
    token = (environ.get("USAGE_DEVICE_TOKEN") or file_cfg.get("token") or "").strip()
    poll_raw = environ.get("USAGE_POLL_S") or file_cfg.get("poll_s")

    if not url and not token:
        return None
    if not url or not token:
        _warn("only one of url/token is set -- usage pull stays OFF")
        return None
    if not _URL_RE.match(url):
        _warn("url must be https://... without spaces or quotes -- usage pull stays OFF")
        return None
    if not _TOKEN_RE.match(token):
        _warn("token has an unexpected format -- usage pull stays OFF")
        return None

    poll_s = None
    if poll_raw not in (None, ""):
        try:
            poll_s = int(poll_raw)
        except (TypeError, ValueError):
            poll_s = None
        if poll_s is None or not (POLL_MIN_S <= poll_s <= POLL_MAX_S):
            _warn("poll_s must be %d..%d -- using the firmware default"
                  % (POLL_MIN_S, POLL_MAX_S))
            poll_s = None
    return {"url": url, "token": token, "poll_s": poll_s}


def _apply(pio_env, project_dir):
    cfg = load_config(project_dir)
    if cfg is None:
        _info("usage pull not configured -- building with the feature OFF")
        return
    defines = [
        ("USAGE_BACKEND_URL", pio_env.StringifyMacro(cfg["url"])),
        ("USAGE_DEVICE_TOKEN", pio_env.StringifyMacro(cfg["token"])),
    ]
    if cfg["poll_s"] is not None:
        defines.append(("USAGE_POLL_S", cfg["poll_s"]))
    pio_env.Append(CPPDEFINES=defines)
    _info("usage pull ON -> %s (token not shown)" % cfg["url"])


# ``Import`` exists only inside a PlatformIO/SCons extra script. As in
# gen_pinned_cert.py it injects ``env`` into this module's globals.
try:
    Import("env")  # noqa: F821  (injected by PlatformIO)
except NameError:
    env = None
except Exception:
    env = None

if env is not None:
    try:
        _project_dir = env["PROJECT_DIR"]
    except Exception:
        _project_dir = _DEFAULT_PROJECT_DIR
    try:
        _apply(env, _project_dir)
    except Exception as exc:  # never fail the build over an optional feature
        _warn("skipped (%s)" % exc.__class__.__name__)

if env is None and globals().get("__name__") == "__main__":
    _cfg = load_config(os.environ.get("PROJECT_DIR") or _DEFAULT_PROJECT_DIR)
    if _cfg is None:
        print("usage pull: OFF")
    else:
        print("usage pull: ON url=%s poll_s=%s" % (_cfg["url"], _cfg["poll_s"] or "default"))
