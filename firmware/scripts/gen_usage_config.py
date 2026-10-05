"""Inject the backend-WiFi usage-pull config as build macros.

The device pulls Claude usage numbers from the owner's backend over HTTPS
(design/backend-wifi/PHASE1-CONTRACT.md section 7) and, when it has no device
token yet, pairs with a short code (PHASE2-CONTRACT.md section 6). Only the
endpoint base is injected at build time; the device token is NOT a build macro
anymore -- it is issued at runtime and stored in NVS by ``usage_pair``.

* ``firmware/certs/usage_backend.json`` -- untracked, e.g.::

      {"base": "https://<vm-ip>/api"}

  The legacy ``"url": "https://<vm-ip>/api/usage"`` key is still accepted; a
  trailing ``/usage`` is stripped to recover the base.

* or the environment variable ``USAGE_BACKEND_BASE`` (it wins over the file,
  handy for CI and one-off builds). The legacy ``USAGE_BACKEND_URL`` is also
  accepted.

A base URL present -> ``-DUSAGE_BACKEND_BASE="..."`` (and optionally
``-DUSAGE_POLL_S=<n>`` from a ``"poll_s"`` key / ``USAGE_POLL_S`` env) is
appended to the compile flags. Anything missing or malformed -> no macros are
defined and ``usage_pull.cpp`` / ``usage_pair.cpp`` compile to a no-op, so the
BLE path is untouched. A ``"token"`` key is ignored (with a warning) and never
printed: the token is no longer compiled in.

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


def _to_base(value):
    """Normalize an endpoint to the base (no trailing ``/usage``, no slash)."""
    base = (value or "").strip().rstrip("/")
    if base.endswith("/usage"):
        base = base[: -len("/usage")].rstrip("/")
    return base


def load_config(project_dir, environ=None):
    """Return ``{"base", "poll_s"}`` (string / int / None), validated.

    Returns ``None`` when the feature is off (nothing set) or the config is
    unusable; a half-set or malformed config warns. The device token is never a
    macro here and is never printed.
    """
    environ = os.environ if environ is None else environ
    file_cfg = _read_file(os.path.join(project_dir, _CONFIG_REL))

    base = _to_base(environ.get("USAGE_BACKEND_BASE")
                    or environ.get("USAGE_BACKEND_URL")
                    or file_cfg.get("base")
                    or file_cfg.get("url"))
    poll_raw = environ.get("USAGE_POLL_S") or file_cfg.get("poll_s")

    if file_cfg.get("token") or environ.get("USAGE_DEVICE_TOKEN"):
        _warn("token key ignored -- the device token is issued at runtime, not compiled in")

    if not base:
        return None
    if not _URL_RE.match(base):
        _warn("base must be https://... without spaces or quotes -- usage pull stays OFF")
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
    return {"base": base, "poll_s": poll_s}


def _apply(pio_env, project_dir):
    cfg = load_config(project_dir)
    if cfg is None:
        _info("usage pull not configured -- building with the feature OFF")
        return
    defines = [
        ("USAGE_BACKEND_BASE", pio_env.StringifyMacro(cfg["base"])),
    ]
    if cfg["poll_s"] is not None:
        defines.append(("USAGE_POLL_S", cfg["poll_s"]))
    pio_env.Append(CPPDEFINES=defines)
    _info("usage pull ON -> %s (token not compiled in)" % cfg["base"])


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
        print("usage pull: ON base=%s poll_s=%s" % (_cfg["base"], _cfg["poll_s"] or "default"))
