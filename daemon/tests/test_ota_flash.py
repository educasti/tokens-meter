#!/usr/bin/env python3
"""Unit tests for the host OTA helper (daemon/ota_flash.py).

Covers the frozen CTRL JSON shapes (info / wifi / ota on / ota off / error) and
the board-id guard — the two contracts DESIGN §2 makes binding — plus the
optional-config and parsing helpers.

Run: python -m pytest daemon/tests/test_ota_flash.py -x -q
"""
import json
from pathlib import Path

import pytest

from daemon import ota_flash
from daemon.ota_flash import (
    BOARD_ID,
    BoardMismatch,
    assert_board,
    error_reason,
    frame_info,
    frame_ota_off,
    frame_ota_on,
    frame_wifi,
    is_error,
    is_info_reply,
    parse_tx,
    read_ota_config,
)


# ---------------------------------------------------------------------------
# CTRL JSON shapes (DESIGN §2, frozen)
# ---------------------------------------------------------------------------

def test_info_frame_shape():
    assert frame_info() == {"cmd": "info"}


def test_wifi_frame_shape_uses_pass_key():
    # The protocol key is "pass" (not "password").
    assert frame_wifi("HomeNet", "s3cret") == {
        "cmd": "wifi", "ssid": "HomeNet", "pass": "s3cret",
    }


def test_ota_on_frame_without_password_omits_pass():
    assert frame_ota_on() == {"cmd": "ota", "mode": "on"}
    assert "pass" not in frame_ota_on()


def test_ota_on_frame_with_password_includes_pass():
    assert frame_ota_on("pw") == {"cmd": "ota", "mode": "on", "pass": "pw"}


def test_ota_off_frame_shape():
    assert frame_ota_off() == {"cmd": "ota", "mode": "off"}


def test_frames_are_json_serializable_and_round_trip():
    for frame in (frame_info(), frame_wifi("S", "P"), frame_ota_on("PW"),
                  frame_ota_off()):
        blob = json.dumps(frame, separators=(",", ":"))
        assert json.loads(blob) == frame


# ---------------------------------------------------------------------------
# Error shape
# ---------------------------------------------------------------------------

def test_error_shape_is_detected():
    err = {"ok": False, "err": "no_wifi"}
    assert is_error(err) is True
    assert error_reason(err) == "no_wifi"


def test_error_reason_falls_back_when_field_missing():
    assert error_reason({"ok": False}) == "unknown"
    assert error_reason(None) == "malformed reply"


def test_success_shapes_are_not_errors():
    assert is_error({"ok": True, "cmd": "wifi"}) is False
    assert is_error({"ok": True, "cmd": "ota", "state": "ready", "ip": "1.2.3.4"}) is False
    assert is_error(None) is False


def test_ota_ready_reply_is_not_error_and_state_is_ready():
    reply = {"ok": True, "cmd": "ota", "state": "ready", "ip": "192.168.1.9",
             "port": 3232}
    assert is_error(reply) is False
    assert reply.get("state") == "ready"
    assert reply.get("ip") == "192.168.1.9"


# ---------------------------------------------------------------------------
# board-id guard (DESIGN §2 rule / §4 anti-brick rule)
# ---------------------------------------------------------------------------

def test_assert_board_accepts_expected_board():
    msg = {"ok": True, "board": BOARD_ID, "fw": "1.2.3", "id": "AA:BB:CC:DD:EE:FF"}
    assert assert_board(msg) == BOARD_ID


def test_assert_board_rejects_other_board():
    with pytest.raises(BoardMismatch):
        assert_board({"ok": True, "board": "some_other_board"})


def test_assert_board_rejects_missing_board():
    with pytest.raises(BoardMismatch):
        assert_board({"ok": True})


def test_assert_board_rejects_error_reply():
    # An error reply has no board and must never be treated as a match.
    with pytest.raises(BoardMismatch):
        assert_board({"ok": False, "err": "not_owner"})


def test_is_info_reply_distinguishes_identity_from_errors():
    assert is_info_reply({"ok": True, "board": BOARD_ID}) is True
    assert is_info_reply({"ok": False, "err": "not_owner"}) is False


# ---------------------------------------------------------------------------
# parse_tx
# ---------------------------------------------------------------------------

def test_parse_tx_decodes_bytes_and_str():
    assert parse_tx(b'{"ok":true,"cmd":"wifi"}') == {"ok": True, "cmd": "wifi"}
    assert parse_tx('{"ok":true}') == {"ok": True}


def test_parse_tx_rejects_non_json_and_non_objects():
    assert parse_tx(b"not json") is None
    assert parse_tx(b"[1,2,3]") is None
    assert parse_tx(b"") is None


# ---------------------------------------------------------------------------
# optional config (DESIGN §6) — CLI flags override, config supplies defaults
# ---------------------------------------------------------------------------

def test_read_ota_config_reads_the_three_keys(tmp_path):
    cfg = tmp_path / "config"
    cfg.write_text(
        "# comment\n"
        "ota_ssid = MyNet\n"
        "ota_password = otaPW   # inline comment\n"
        "ota_wifi_password = wifiPW\n"
        "clock = auto\n"
    )
    assert read_ota_config(cfg) == {
        "ota_ssid": "MyNet",
        "ota_password": "otaPW",
        "ota_wifi_password": "wifiPW",
    }


def test_read_ota_config_missing_file_is_empty(tmp_path):
    assert read_ota_config(tmp_path / "absent") == {}


# ---------------------------------------------------------------------------
# upload command / small pure helpers
# ---------------------------------------------------------------------------

def test_env_from_firmware_path():
    fw = "/repo/firmware/.pio/build/waveshare_amoled_216/firmware.bin"
    assert ota_flash._env_from_firmware_path(Path(fw)) == "waveshare_amoled_216"


def test_env_from_firmware_path_falls_back_to_board():
    assert ota_flash._env_from_firmware_path(Path("/tmp/fw.bin")) == BOARD_ID


def test_parse_tray_pids_ignores_noise():
    stdout = " 1234 \nnot-a-pid\n\n5678\n"
    assert ota_flash._parse_tray_pids(stdout) == [1234, 5678]


def test_display_command_masks_ota_password():
    cmd = ["python", "espota.py", "-i", "1.2.3.4", "-p", "3232",
           "-f", "fw.bin", "-a", "hunter2"]
    shown = ota_flash._display_command(cmd)
    assert "hunter2" not in shown
    assert shown.endswith("-a ***")
