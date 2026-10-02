#!/usr/bin/env python3
"""Host tests for the P0 pull-OTA tooling.

Two frozen contracts are exercised here, against the real tools in the repo:

* ``firmware/scripts/version.py`` — CLI ``--print-version`` / ``--print-json``
  (DESIGN §5.2). It must work when the CWD is the git checkout it is stamping,
  including a throwaway temp repo.
* ``design/ota-pull/deploy/ota-publish.sh`` — CLI ``--dry-run`` renders the
  manifest JSON as the **only** stdout content (DESIGN §6.2, §6.5), after
  computing sha256 + size, and refuses clearly invalid input.

The tools are shells out to (``sys.executable`` / ``bash``), matching the
repo's host-test style. Nothing here touches the network or the real VM.

Run: python -m pytest daemon/tests/test_ota_publish.py -q
"""
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
VERSION_PY = REPO / "firmware" / "scripts" / "version.py"
PUBLISH_SH = REPO / "design" / "ota-pull" / "deploy" / "ota-publish.sh"

# DESIGN §6.2: required fields (min_from / mandatory / released_at / notes are
# optional). The publish script always emits the optional ones too.
REQUIRED_MANIFEST_FIELDS = (
    "schema_version", "board", "version", "url", "sha256", "size",
)

pytestmark = pytest.mark.skipif(
    shutil.which("git") is None, reason="git is required for the version tests"
)


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _git(repo: Path, *args: str) -> str:
    proc = subprocess.run(
        ["git", *args], cwd=repo, capture_output=True, text=True,
    )
    assert proc.returncode == 0, f"git {' '.join(args)} failed: {proc.stderr}"
    return proc.stdout


def _init_git_repo(repo: Path) -> None:
    """A throwaway checkout with one commit, so a tag is meaningful."""
    repo.mkdir(parents=True, exist_ok=True)
    _git(repo, "init", "-q")
    _git(repo, "config", "user.email", "tests@example.invalid")
    _git(repo, "config", "user.name", "ota-publish tests")
    (repo / "README").write_text("fixture\n")
    _git(repo, "add", "README")
    _git(repo, "commit", "-q", "-m", "fixture")


def _run_version(cwd: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(VERSION_PY), *args],
        cwd=cwd, capture_output=True, text=True,
    )


def _write_bin(tmp_path: Path, size: int = 1000) -> tuple[Path, bytes]:
    """A deterministic fake firmware image of exactly ``size`` bytes."""
    blob = bytes((i * 37 + 11) % 256 for i in range(size))
    path = tmp_path / "firmware.bin"
    path.write_bytes(blob)
    return path, blob


def _run_publish(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["bash", str(PUBLISH_SH), *args],
        cwd=REPO, capture_output=True, text=True,
    )


def _assert_ok(proc: subprocess.CompletedProcess) -> None:
    assert proc.returncode == 0, (
        f"expected exit 0, got {proc.returncode}\n--- stderr ---\n{proc.stderr}"
    )


def _assert_no_manifest(proc: subprocess.CompletedProcess) -> None:
    """A refusal: non-zero exit and nothing manifest-shaped on stdout."""
    assert proc.returncode != 0, (
        f"expected a non-zero exit, got 0\n--- stdout ---\n{proc.stdout}"
    )
    assert proc.stdout.strip() == "", (
        f"refusal leaked JSON to stdout: {proc.stdout!r}"
    )


def _repo_is_dirty() -> bool:
    proc = subprocess.run(
        ["git", "status", "--porcelain"], cwd=REPO,
        capture_output=True, text=True,
    )
    return proc.returncode == 0 and bool(proc.stdout.strip())


def _valid_args(binary: Path) -> list[str]:
    return [
        "--bin", str(binary),
        "--version", "1.2.3",
        "--board", "waveshare_amoled_216",
        "--dest", "user@host:/srv/firmware",
        "--dry-run",
        "--allow-dirty",
    ]


def _drop_flag(args: list[str], flag: str) -> list[str]:
    out = list(args)
    i = out.index(flag)
    del out[i:i + 2]
    return out


def _set_flag(args: list[str], flag: str, value: str) -> list[str]:
    out = list(args)
    out[out.index(flag) + 1] = value
    return out


# ---------------------------------------------------------------------------
# version.py — CLI (DESIGN §5.2)
# ---------------------------------------------------------------------------

def test_print_version_uses_the_nearest_v_tag(tmp_path):
    repo = tmp_path / "repo"
    _init_git_repo(repo)
    _git(repo, "tag", "v1.2.3")

    proc = _run_version(repo, "--print-version")

    _assert_ok(proc)
    assert proc.stdout.strip() == "1.2.3"


def test_print_version_without_a_tag_is_dev(tmp_path):
    repo = tmp_path / "repo"
    _init_git_repo(repo)  # committed, but never tagged

    proc = _run_version(repo, "--print-version")

    _assert_ok(proc)
    assert proc.stdout.strip() == "0.0.0-dev"


def test_print_json_carries_version_sha_and_build(tmp_path):
    repo = tmp_path / "repo"
    _init_git_repo(repo)
    _git(repo, "tag", "v1.2.3")
    short_sha = _git(repo, "rev-parse", "--short=7", "HEAD").strip()

    proc = _run_version(repo, "--print-json")

    _assert_ok(proc)
    data = json.loads(proc.stdout)
    assert {"version", "sha", "build"} <= set(data)
    assert data["version"] == "1.2.3"
    # DESIGN §5.2: FW_GIT_SHA is the short HEAD commit.
    assert data["sha"] == short_sha
    assert isinstance(data["build"], str) and data["build"]


# ---------------------------------------------------------------------------
# ota-publish.sh --dry-run — the manifest, byte-exact (DESIGN §6.2/§6.5)
# ---------------------------------------------------------------------------

def test_dry_run_emits_the_manifest_on_stdout(tmp_path):
    binary, blob = _write_bin(tmp_path, size=1000)

    proc = _run_publish(*_valid_args(binary))

    _assert_ok(proc)
    # The ONLY stdout content is the rendered manifest: json.loads of the whole
    # stream must succeed (logs belong on stderr).
    manifest = json.loads(proc.stdout)
    assert manifest["version"] == "1.2.3"
    assert manifest["board"] == "waveshare_amoled_216"
    assert manifest["url"] == "clawdmeter-1.2.3.bin"
    assert manifest["sha256"] == hashlib.sha256(blob).hexdigest()
    assert manifest["size"] == len(blob) == 1000
    assert manifest["sha256"] == manifest["sha256"].lower()


def test_dry_run_manifest_has_the_design_6_2_fields(tmp_path):
    binary, _ = _write_bin(tmp_path, size=1000)

    proc = _run_publish(*_valid_args(binary))

    _assert_ok(proc)
    manifest = json.loads(proc.stdout)
    for field in REQUIRED_MANIFEST_FIELDS:
        assert field in manifest, f"missing required field: {field}"
    assert manifest["schema_version"] == 1
    assert isinstance(manifest["size"], int) and manifest["size"] > 0
    assert len(manifest["sha256"]) == 64
    # Optional fields the script always writes, with the right types.
    assert isinstance(manifest["mandatory"], bool)
    assert isinstance(manifest["released_at"], str) and manifest["released_at"]


def test_dry_run_passes_min_from_mandatory_and_notes_through(tmp_path):
    binary, _ = _write_bin(tmp_path, size=1000)

    proc = _run_publish(
        *_valid_args(binary), "--min-from", "1.0.0", "--mandatory",
        "--notes", 'fix "quotes" on reconnect',
    )

    _assert_ok(proc)
    manifest = json.loads(proc.stdout)
    assert manifest["min_from"] == "1.0.0"
    assert manifest["mandatory"] is True
    assert manifest["notes"] == 'fix "quotes" on reconnect'


# ---------------------------------------------------------------------------
# refusals — non-zero exit, no manifest on stdout
# ---------------------------------------------------------------------------

_NEGATIVE_CASES = {
    "missing_bin": lambda binary, tmp: _drop_flag(_valid_args(binary), "--bin"),
    "non_semver_version": lambda binary, tmp: _set_flag(
        _valid_args(binary), "--version", "1.2"),
    "keep_zero": lambda binary, tmp: _valid_args(binary) + ["--keep", "0"],
    "dest_without_colon": lambda binary, tmp: _set_flag(
        _valid_args(binary), "--dest", "/srv/firmware"),
    "nonexistent_bin": lambda binary, tmp: _set_flag(
        _valid_args(binary), "--bin", str(tmp / "does-not-exist.bin")),
}


@pytest.mark.parametrize("case", sorted(_NEGATIVE_CASES), ids=sorted(_NEGATIVE_CASES))
def test_invalid_input_is_refused_without_a_manifest(tmp_path, case):
    binary, _ = _write_bin(tmp_path, size=1000)

    proc = _run_publish(*_NEGATIVE_CASES[case](binary, tmp_path))

    _assert_no_manifest(proc)


# ---------------------------------------------------------------------------
# dirty-tree guard (DESIGN §5.2 / IMPL P0)
# ---------------------------------------------------------------------------

def test_dirty_tree_is_refused_unless_allow_dirty(tmp_path):
    if not _repo_is_dirty():
        pytest.skip("repo working tree is clean; nothing to guard against")

    binary, blob = _write_bin(tmp_path, size=1000)
    clean_args = _drop_flag(_valid_args(binary), "--allow-dirty")

    refused = _run_publish(*clean_args)
    _assert_no_manifest(refused)

    allowed = _run_publish(*clean_args, "--allow-dirty")
    _assert_ok(allowed)
    manifest = json.loads(allowed.stdout)
    assert manifest["sha256"] == hashlib.sha256(blob).hexdigest()
