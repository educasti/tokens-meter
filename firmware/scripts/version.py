"""Build-time version stamping for the firmware.

Two roles, one source of truth (design/ota-pull/DESIGN.md section 5.2):

* PlatformIO pre-build extra script (``firmware/platformio.ini``):
  ``extra_scripts = pre:scripts/version.py``. It appends ``FW_VERSION``,
  ``FW_GIT_SHA`` and ``FW_BUILD_DATE`` to the compile flags so a local
  ``pio run`` and CI stamp the image identically.
* Standalone CLI, usable with no PlatformIO installed, so the publish
  tooling (``design/ota-pull/deploy/ota-publish.sh``) can derive the same
  version the firmware reports::

      python3 firmware/scripts/version.py --print-version
      python3 firmware/scripts/version.py --print-json

The script never fails the build: every git call swallows its errors and a
missing tag / tarball checkout falls back to ``0.0.0-dev``.
"""

import datetime
import json
import os
import re
import subprocess
import sys

# DESIGN section 5.1: strict MAJOR.MINOR.PATCH, optional leading 'v' stripped.
_SEMVER_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
_FALLBACK_VERSION = "0.0.0-dev"

# Directory git runs in. PlatformIO sets it from env["PROJECT_DIR"]; the CLI
# uses $PROJECT_DIR when set, otherwise the current working directory.
_PROJECT_DIR = os.environ.get("PROJECT_DIR") or os.getcwd()


def _set_project_dir(path):
    global _PROJECT_DIR
    if path:
        _PROJECT_DIR = path


def git(*args):
    """Run ``git <args>`` in the project dir.

    Returns the stripped stdout on success, or ``None`` on any failure
    (not a repo, missing binary, no matching tag, non-zero exit). Git's
    stderr is discarded; this must never surface to the build.
    """
    try:
        proc = subprocess.run(
            ["git", *args],
            cwd=_PROJECT_DIR,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except Exception:
        return None
    if proc.returncode != 0:
        return None
    return proc.stdout.decode("utf-8", "replace").strip()


def stamped_version():
    """Return ``(version, sha, build_date)`` per DESIGN section 5.2.

    * ``version`` — nearest ``v*`` tag with the ``v`` stripped and validated
      as strict semver; ``0.0.0-dev`` when there is no usable tag.
    * ``sha`` — ``git rev-parse --short=7 HEAD``, or ``""`` outside a repo.
    * ``build_date`` — current UTC time as ISO-8601 seconds
      (``YYYY-MM-DDTHH:MM:SSZ``).
    """
    version = _FALLBACK_VERSION
    described = git("describe", "--tags", "--match", "v*", "--abbrev=0")
    if described:
        candidate = described[1:] if described.startswith("v") else described
        if _SEMVER_RE.match(candidate):
            version = candidate

    sha = git("rev-parse", "--short=7", "HEAD") or ""
    build_date = datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ"
    )
    return version, sha, build_date


def generate_manifest_defaults():
    """The stamp as a dict, so host tooling cannot drift from the firmware."""
    version, sha, build_date = stamped_version()
    return {"version": version, "sha": sha, "build": build_date}


def _append_build_flags(env):
    """Inject the stamp into a PlatformIO/SCons env. Never raises."""
    try:
        version, sha, build_date = stamped_version()
        env.Append(
            BUILD_FLAGS=[
                '-DFW_VERSION=\\"%s\\"' % version,
                '-DFW_GIT_SHA=\\"%s\\"' % sha,
                '-DFW_BUILD_DATE=\\"%s\\"' % build_date,
            ]
        )
    except Exception:
        # A build-stamp problem must not take the build down.
        pass


# ---- PlatformIO integration -------------------------------------------------
# ``Import`` exists only inside a PlatformIO/SCons extra script; NameError on
# the plain CLI path is expected and handled here.
try:
    Import("env")  # noqa: F821  (injected by PlatformIO)
except NameError:
    env = None
except Exception:
    env = None

if env is not None:
    try:
        _set_project_dir(env["PROJECT_DIR"])
    except Exception:
        pass
    _append_build_flags(env)


# ---- CLI --------------------------------------------------------------------
def _main(argv):
    if "--print-version" in argv:
        print(stamped_version()[0])
        return 0
    if "--print-json" in argv:
        print(json.dumps(generate_manifest_defaults()))
        return 0
    sys.stderr.write(
        "usage: version.py [--print-version | --print-json]\n"
    )
    return 2


if globals().get("__name__") == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
