#!/usr/bin/env python3
"""Host OTA helper for the Waveshare ESP32-S3-Touch-AMOLED-2.16 (hybrid OTA).

Implements the host half of design/ota-hybrid/DESIGN.md: it stops the BLE
daemon (so the single physical link is free), provisions WiFi credentials and
triggers OTA mode over the CTRL characteristic, uploads the binary over WiFi
with `espota.py`, switches OTA mode back off, and restarts the daemon.

The BLE link is only the control path — the transfer is WiFi (DESIGN §1, §3).
Usage:

    python daemon/ota_flash.py \\
        --firmware firmware/.pio/build/waveshare_amoled_216/firmware.bin \\
        [--ssid S --pass P] [--ota-password PW] [--keep-daemon] \\
        [--address IP] [--port 3232]

macOS / Linux use launchctl / systemctl --user to stop the daemon; on Windows
the tray app is stopped (or the user is asked to quit it). See
README-windows.md for the Windows OTA section.

No new third-party dependencies: bleak is already the daemon's dependency.
"""

import argparse
import asyncio
import json
import shutil
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path

from bleak import BleakClient
from bleak.exc import BleakError

try:  # normal package import (tests, `python -m daemon.ota_flash`)
    from .claude_usage_daemon import (
        CONFIG_FILE,
        CONNECT_TIMEOUT,
        _is_encryption_error,
        discover_target,
        log,
        unpair_macos,
    )
except ImportError:  # run as a script (`python daemon/ota_flash.py`)
    from claude_usage_daemon import (  # type: ignore[no-redef]
        CONFIG_FILE,
        CONNECT_TIMEOUT,
        _is_encryption_error,
        discover_target,
        log,
        unpair_macos,
    )

# --- BLE protocol (DESIGN §2, frozen) ---------------------------------------

SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
TX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000003"   # device -> host notify
CTRL_CHAR_UUID = "4c41555a-4465-7669-6365-000000000005"  # host -> device write

BOARD_ID = "waveshare_amoled_216"
DEFAULT_OTA_PORT = 3232

INFO_TIMEOUT = 10.0     # identity is local, answers immediately
WIFI_TIMEOUT = 30.0     # NVS write + ack
OTA_READY_TIMEOUT = 90.0  # station-mode WiFi connect can take a while
DAEMON_STOP_SETTLE = 2.0  # let launchd/systemd release the BLE link

# Daemon service identifiers (must match the installers).
MAC_SERVICE_LABEL = "com.user.claude-usage-daemon"
LINUX_SERVICE_NAME = "claude-usage-daemon"
WIN_TRAY_SCRIPT = "tray_windows.py"
WIN_AUTOSTART_VALUE = "Clawdmeter"

# Optional config keys (daemon/config.example). CLI flags always override.
OTA_CONFIG_KEYS = ("ota_ssid", "ota_password", "ota_wifi_password")


class OtaError(Exception):
    """A recoverable protocol/upload failure — reported and turned into a
    non-zero exit code, never a traceback."""


class BoardMismatch(OtaError):
    """The connected device does not identify as the artifact's board."""


# --- CTRL frames (pure; unit-tested) ----------------------------------------


def frame_info() -> dict:
    """`{"cmd":"info"}` -> identity so we can assert the board before flashing."""
    return {"cmd": "info"}


def frame_wifi(ssid: str, password: str) -> dict:
    """`{"cmd":"wifi","ssid":S,"pass":P}` — store credentials in NVS.

    Note the key is ``pass`` (the frozen protocol), not ``password``.
    """
    return {"cmd": "wifi", "ssid": ssid, "pass": password}


def frame_ota_on(password: str | None = None) -> dict:
    """`{"cmd":"ota","mode":"on"[,"pass":PW]}` — connect WiFi + start ArduinoOTA.

    The shared OTA password is only included when one was supplied (DESIGN §3).
    """
    frame = {"cmd": "ota", "mode": "on"}
    if password:
        frame["pass"] = password
    return frame


def frame_ota_off() -> dict:
    """`{"cmd":"ota","mode":"off"}` — stop ArduinoOTA and bring WiFi down."""
    return {"cmd": "ota", "mode": "off"}


def parse_tx(data) -> dict | None:
    """Decode one TX notification into a dict, or None if it isn't JSON object."""
    try:
        if isinstance(data, (bytes, bytearray)):
            data = bytes(data).decode("utf-8", "replace")
        msg = json.loads(data)
    except (ValueError, TypeError):
        return None
    return msg if isinstance(msg, dict) else None


def is_error(msg: dict | None) -> bool:
    """True for the frozen error shape `{"ok":false,"err":...}`."""
    return isinstance(msg, dict) and msg.get("ok") is False


def error_reason(msg: dict | None) -> str:
    """Human-readable `err` from an error reply (never raises)."""
    if not isinstance(msg, dict):
        return "malformed reply"
    reason = msg.get("err") or msg.get("error") or "unknown"
    return str(reason)


def is_info_reply(msg: dict | None) -> bool:
    """The identity reply carries a ``board`` field; errors do not."""
    return isinstance(msg, dict) and "board" in msg


def assert_board(msg: dict | None) -> str:
    """Guarantee the device is the board we are about to flash, else raise.

    Returns the board string on success (DESIGN §2 rule, §4 anti-brick rule).
    """
    board = msg.get("board") if isinstance(msg, dict) else None
    if board != BOARD_ID:
        raise BoardMismatch(
            f"refusing to flash: board {board!r} != expected {BOARD_ID!r}"
        )
    return board


def _redact(frame: dict) -> dict:
    """Copy a frame with secret-bearing values masked, for logging."""
    out = dict(frame)
    for key in ("pass", "password"):
        if key in out:
            out[key] = "***"
    return out


# --- optional config (CLI flags override; DESIGN §6) ------------------------


def read_ota_config(path: Path | None = None) -> dict[str, str]:
    """Read the optional ota_* keys from the daemon config file.

    Returns only the keys that are present. Missing file / unreadable file is
    not an error — the CLI flags are the primary interface.
    """
    path = path or CONFIG_FILE
    out: dict[str, str] = {}
    try:
        if path.exists():
            for line in path.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                key = key.strip().lower()
                if key in OTA_CONFIG_KEYS:
                    out[key] = val.strip()
    except OSError:
        pass
    return out


# --- daemon stop / restore --------------------------------------------------


def _run(cmd: list[str]) -> bool:
    """Run a service/config command, logging and swallowing failures.

    The helper must never crash on a missing service manager or a system that
    never installed the daemon; a failed stop is a warning, not a dead end.
    """
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError) as e:
        log(f"{' '.join(cmd)} failed: {e}")
        return False
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout).strip()
        log(f"{' '.join(cmd)} rc={proc.returncode}"
            + (f": {detail}" if detail else ""))
        return False
    return True


def stop_daemon(keep: bool) -> Callable[[], None] | None:
    """Stop the BLE daemon and return a restore callback, or None.

    ``keep=True`` (--keep-daemon) skips this entirely for manual runs where
    the daemon is already stopped. The returned callback restarts the service
    and is meant to be called from a ``finally`` block.
    """
    if keep:
        log("--keep-daemon: leaving the daemon running")
        return None
    if sys.platform == "darwin":
        return _stop_macos()
    if sys.platform == "win32":
        return _stop_windows()
    return _stop_linux()


def _stop_macos():
    plist = Path.home() / "Library" / "LaunchAgents" / f"{MAC_SERVICE_LABEL}.plist"
    if not plist.exists():
        log(f"Daemon plist not found at {plist}; assuming the daemon is not installed")
        return None
    if not shutil.which("launchctl"):
        log("launchctl not available; stop the daemon manually to free BLE")
        return None
    if not _run(["launchctl", "unload", str(plist)]):
        log("Could not unload the daemon (already stopped?)")
    time.sleep(DAEMON_STOP_SETTLE)
    _warn_if_still_running()

    def restore() -> None:
        if _run(["launchctl", "load", "-w", str(plist)]):
            log("Daemon restarted (launchctl load -w)")

    return restore


def _stop_linux():
    if not shutil.which("systemctl"):
        log("systemctl not available; stop the daemon manually to free BLE")
        return None
    if not _run(["systemctl", "--user", "stop", LINUX_SERVICE_NAME]):
        log("Could not stop the daemon (not installed or already stopped?)")
    time.sleep(DAEMON_STOP_SETTLE)
    _warn_if_still_running()

    def restore() -> None:
        if _run(["systemctl", "--user", "start", LINUX_SERVICE_NAME]):
            log("Daemon restarted (systemctl --user start)")

    return restore


def _warn_if_still_running() -> None:
    """Warn if a manually-started daemon still holds the BLE link."""
    try:
        out = subprocess.run(
            ["pgrep", "-f", "claude_usage_daemon"],
            capture_output=True, text=True, timeout=5,
        ).stdout.split()
    except (OSError, subprocess.SubprocessError):
        return
    if out:
        log("Warning: claude_usage_daemon is still running (manual start?). "
            "Stop it so the BLE connection is free, or re-run with --keep-daemon.")


def _parse_tray_pids(stdout: str) -> list[int]:
    """Parse one PID per line (the PowerShell tray lookup) — pure, testable."""
    pids: list[int] = []
    for line in stdout.splitlines():
        line = line.strip()
        if line.isdigit():
            pids.append(int(line))
    return pids


def _find_tray_pids() -> list[int]:
    """PIDs of the running Windows Clawdmeter tray app (pythonw + tray_windows.py)."""
    shell = shutil.which("powershell") or shutil.which("pwsh")
    if not shell:
        return []
    script = (
        "Get-CimInstance Win32_Process -Filter \"Name='pythonw.exe'\" | "
        f"Where-Object {{ $_.CommandLine -like '*{WIN_TRAY_SCRIPT}*' }} | "
        "ForEach-Object { $_.ProcessId }"
    )
    try:
        proc = subprocess.run(
            [shell, "-NoProfile", "-Command", script],
            capture_output=True, text=True, timeout=20,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except (OSError, subprocess.SubprocessError) as e:
        log(f"Tray lookup failed: {e}")
        return []
    return _parse_tray_pids(proc.stdout)


def _relaunch_win_tray() -> None:
    """Best-effort relaunch of the tray via its HKCU Run autostart command."""
    try:
        import winreg  # type: ignore[import]
    except ImportError:
        return
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Run",
        ) as key:
            cmd, _ = winreg.QueryValueEx(key, WIN_AUTOSTART_VALUE)
    except OSError:
        log("Tray autostart entry not found; start the Clawdmeter tray manually")
        return
    try:
        subprocess.Popen(cmd, shell=True)
        log("Clawdmeter tray relaunched")
    except OSError as e:
        log(f"Could not relaunch the tray: {e}")


def _stop_windows():
    pids = _find_tray_pids()
    if not pids:
        log("Clawdmeter tray app not detected. If it is running, quit it from "
            "the tray icon (right-click > Quit) before flashing.")
        return None
    for pid in pids:
        _run(["taskkill", "/PID", str(pid), "/F"])
    log(f"Stopped Clawdmeter tray app (pid {', '.join(map(str, pids))})")
    time.sleep(DAEMON_STOP_SETTLE)

    def restore() -> None:
        _relaunch_win_tray()

    return restore


# --- BLE session (control path) ---------------------------------------------


class OtaSession:
    """CTRL writes + TX notification waiting over one BleakClient."""

    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self._queue: asyncio.Queue[dict] = asyncio.Queue()

    def _on_tx(self, _char, data: bytearray) -> None:
        msg = parse_tx(data)
        if msg is None:
            log(f"TX (non-JSON): {bytes(data)!r}")
            return
        log(f"TX  <- {msg}")
        self._queue.put_nowait(msg)

    async def subscribe(self) -> None:
        """Subscribe to TX so every command's reply has one place to land."""
        try:
            await asyncio.wait_for(
                self.client.start_notify(TX_CHAR_UUID, self._on_tx), timeout=10
            )
        except (BleakError, ValueError) as e:
            raise OtaError(f"could not subscribe to TX {TX_CHAR_UUID}: {e}") from e
        except asyncio.TimeoutError as e:
            raise OtaError("timed out subscribing to the TX characteristic") from e

    async def send(self, frame: dict) -> None:
        payload = json.dumps(frame, separators=(",", ":")).encode()
        log(f"CTRL -> {json.dumps(_redact(frame), separators=(',', ':'))}")
        try:
            await self.client.write_gatt_char(CTRL_CHAR_UUID, payload, response=True)
        except BleakError as e:
            raise OtaError(f"CTRL write failed: {e}") from e

    async def wait_for(self, predicate, timeout: float, what: str) -> dict:
        """Wait for the first TX reply satisfying ``predicate``; skip others."""
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while True:
            remaining = deadline - loop.time()
            if remaining <= 0:
                raise OtaError(f"timed out waiting for {what} ({timeout:.0f}s)")
            try:
                msg = await asyncio.wait_for(self._queue.get(), timeout=remaining)
            except asyncio.TimeoutError as e:
                raise OtaError(f"timed out waiting for {what} ({timeout:.0f}s)") from e
            if predicate(msg):
                return msg
            log(f"Ignoring unrelated TX while waiting for {what}: {msg}")


async def connect(target) -> BleakClient:
    """Connect exactly like the daemon does, including the stale-bond self-heal."""
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        await asyncio.wait_for(client.connect(), timeout=CONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError) as e:
        if sys.platform == "darwin" and _is_encryption_error(e):
            log("Encryption failed — likely a stale macOS bond; self-healing")
            unpair_macos()
        raise OtaError(f"connection failed: {e}") from e
    if not client.is_connected:
        raise OtaError("connection failed (no error but not connected)")
    log("Connected")
    return client


# --- espota / upload --------------------------------------------------------


def find_espota() -> Path | None:
    """Resolve PlatformIO's espota.py, or None to fall back to `pio`."""
    packages = Path.home() / ".platformio" / "packages"
    candidates: list[Path] = []
    candidates += sorted(packages.glob("framework-*/tools/espota.py"))
    candidates.append(packages / "tool-esptoolpy" / "espota.py")
    candidates += sorted(packages.glob("*/espota.py"))
    candidates += sorted(packages.glob("*/tools/espota.py"))
    for path in candidates:
        if path.is_file():
            return path
    return None


def _pio_bin() -> str | None:
    found = shutil.which("pio")
    if found:
        return found
    candidate = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    return str(candidate) if candidate.is_file() else None


def _firmware_dir() -> Path:
    return Path(__file__).resolve().parent.parent / "firmware"


def _env_from_firmware_path(firmware: Path) -> str:
    """PLATFORMIO env from `…/.pio/build/<env>/firmware.bin` (fallback default)."""
    parts = firmware.resolve().parts
    if ".pio" in parts:
        i = parts.index(".pio")
        if i + 2 < len(parts) and parts[i + 1] == "build":
            return parts[i + 2]
    return BOARD_ID


def build_upload_command(
    firmware: Path, ip: str, port: int, password: str | None
) -> list[str]:
    """The exact espota (or `pio` fallback) command, with the password masked.

    Returns the argv to execute; the display form masks ``-a <pw>``.
    """
    espota = find_espota()
    if espota is not None:
        cmd = [
            sys.executable, str(espota),
            "-i", ip, "-p", str(port), "-f", str(firmware),
        ]
        if password:
            cmd += ["-a", password]
        return cmd
    pio = _pio_bin()
    if not pio:
        raise OtaError(
            "espota.py not found under ~/.platformio/packages and `pio` is not "
            "installed; install PlatformIO or pass a resolvable firmware path"
        )
    return [
        pio, "run", "-d", str(_firmware_dir()),
        "-e", _env_from_firmware_path(firmware),
        "-t", "upload", "--upload-port", ip,
    ]


def _display_command(cmd: list[str]) -> str:
    """Render argv for logging with any `-a <password>` masked."""
    out: list[str] = []
    mask_next = False
    for arg in cmd:
        if mask_next:
            out.append("***")
            mask_next = False
            continue
        out.append(arg)
        if arg == "-a":
            mask_next = True
    return " ".join(out)


def run_upload(cmd: list[str]) -> int:
    """Run espota/pio, streaming its output through the daemon's log()."""
    log(f"Uploading: {_display_command(cmd)}")
    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
    except OSError as e:
        raise OtaError(f"could not launch uploader: {e}") from e
    assert proc.stdout is not None
    for line in proc.stdout:
        line = line.rstrip()
        if line:
            log(line)
    return proc.wait()


# --- orchestration ----------------------------------------------------------


async def run_ota(args: argparse.Namespace) -> int:
    cfg = read_ota_config()
    ssid = args.ssid or cfg.get("ota_ssid")
    wifi_pass = args.pass_ if args.pass_ is not None else cfg.get("ota_wifi_password")
    ota_password = (
        args.ota_password if args.ota_password is not None else cfg.get("ota_password")
    )

    firmware = Path(args.firmware).expanduser()
    if not firmware.is_file():
        log(f"Firmware not found: {firmware}")
        return 1

    restore = stop_daemon(args.keep_daemon)
    client: BleakClient | None = None
    try:
        target = await discover_target()
        if not target:
            log("Device not found (not held/pinned); cannot start OTA")
            return 2

        client = await connect(target)
        session = OtaSession(client)
        await session.subscribe()

        # 1. Identity + board guard.
        await session.send(frame_info())
        info = await session.wait_for(
            lambda m: is_info_reply(m) or is_error(m), INFO_TIMEOUT, "info"
        )
        if is_error(info):
            raise OtaError(f"info failed: {error_reason(info)}")
        assert_board(info)
        log(f"Board confirmed: {info.get('board')} fw={info.get('fw')!r}")

        # 2. Optional provisioning.
        if ssid:
            await session.send(frame_wifi(ssid, wifi_pass or ""))
            ack = await session.wait_for(
                lambda m: m.get("cmd") == "wifi" or is_error(m), WIFI_TIMEOUT, "wifi"
            )
            if is_error(ack):
                raise OtaError(f"wifi provisioning failed: {error_reason(ack)}")
            log("WiFi credentials stored")

        # 3. Trigger OTA mode and wait for the ArduinoOTA endpoint.
        await session.send(frame_ota_on(ota_password))
        reply = await session.wait_for(
            lambda m: m.get("cmd") == "ota" or is_error(m),
            OTA_READY_TIMEOUT, "ota ready",
        )
        if is_error(reply):
            raise OtaError(f"ota on failed: {error_reason(reply)}")
        if reply.get("state") != "ready":
            raise OtaError(f"ota on unexpected state: {reply.get('state')!r}")

        ip = args.address or reply.get("ip")
        port = args.port or reply.get("port") or DEFAULT_OTA_PORT
        if not ip:
            raise OtaError("device did not report an OTA IP address")
        log(f"OTA ready at {ip}:{port}")

        # 4. Upload over WiFi.
        cmd = build_upload_command(firmware, str(ip), int(port), ota_password)
        rc = run_upload(cmd)
        if rc != 0:
            log(f"Upload failed (rc={rc})")
            return 1
        log("Upload complete")

        # 5. Best-effort stop; the device may already be rebooting into the
        #    new slot, so a failure here is not fatal.
        try:
            await session.send(frame_ota_off())
        except OtaError as e:
            log(f"ota off skipped: {e}")
        return 0
    except OtaError as e:
        log(f"OTA failed: {e}")
        return 1
    finally:
        if client is not None:
            try:
                await client.disconnect()
            except BleakError:
                pass
        if restore is not None:
            restore()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ota_flash.py",
        description="Hybrid OTA: trigger OTA mode over BLE, then upload firmware "
                    "over WiFi (design/ota-hybrid/DESIGN.md).",
    )
    parser.add_argument("--firmware", required=True,
                        help="path to firmware.bin (from `pio run`)")
    parser.add_argument("--ssid", help="WiFi SSID to provision (optional)")
    parser.add_argument("--pass", dest="pass_", metavar="PASS",
                        help="WiFi password for --ssid")
    parser.add_argument("--ota-password", metavar="PW",
                        help="shared ArduinoOTA password (optional)")
    parser.add_argument("--keep-daemon", action="store_true",
                        help="do not stop/restart the daemon (manual runs)")
    parser.add_argument("--address", metavar="IP",
                        help="override the OTA IP reported over BLE")
    parser.add_argument("--port", type=int,
                        help=f"override the OTA port (default {DEFAULT_OTA_PORT})")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    log("=== Clawdmeter hybrid OTA ===")
    try:
        return asyncio.run(run_ota(args))
    except KeyboardInterrupt:
        log("Interrupted")
        return 130


if __name__ == "__main__":
    sys.exit(main())
