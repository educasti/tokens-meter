#!/usr/bin/env python3
"""Tests for the optional backend publish (design/backend-wifi/PHASE1-CONTRACT.md §6).

httpx is mocked; nothing touches the network.
Run: python -m pytest daemon/tests/test_backend_publish.py -q
"""
import asyncio
import threading
import time

import httpx
import pytest

import daemon.claude_usage_daemon as d

KEY = "SECRET-USER-KEY-abc123"
URL = "https://203.0.113.10/api/usage"
PAYLOAD = {"s": 45, "sr": 120, "w": 28, "wr": 7200, "st": "allowed",
           "acct": "pro", "ok": True, "c": 1, "t": 12345, "tf": 24}


@pytest.fixture
def cfg(tmp_path, monkeypatch):
    path = tmp_path / "config"
    monkeypatch.setattr(d, "CONFIG_FILE", path)

    def write(text: str) -> None:
        path.write_text(text)

    return write


@pytest.fixture
def posts(monkeypatch):
    calls = []

    def fake_post(url, **kw):
        calls.append((url, kw))
        return httpx.Response(200, json={"ok": True})

    monkeypatch.setattr(d.httpx, "post", fake_post)
    return calls


def test_off_by_default_no_config_file(cfg, posts):
    assert d.read_backend_config() is None
    assert d.publish_backend(PAYLOAD) is False
    assert posts == []


def test_off_when_keys_present_but_no_url(cfg, posts):
    cfg(f"backend_user_key = {KEY}\n")
    assert d.publish_backend(PAYLOAD) is False
    assert posts == []


def test_publish_sends_numbers_with_bearer(cfg, posts):
    cfg(f"backend_url = {URL}\nbackend_user_key = {KEY}  # my key\n")
    before = int(time.time())
    assert d.publish_backend(PAYLOAD) is True
    (url, kw), = posts
    assert url == URL
    assert kw["headers"] == {"Authorization": f"Bearer {KEY}"}
    assert kw["timeout"] == 5.0
    assert kw["verify"] is True  # system CAs
    body = kw["json"]
    assert set(body) == {"s", "sr", "w", "wr", "st", "t"}
    assert (body["s"], body["sr"], body["w"], body["wr"], body["st"]) == (45, 120, 28, 7200, "allowed")
    # t is real epoch now, not the payload's local-clock value
    assert before <= body["t"] <= int(time.time())


def test_key_is_case_preserved(cfg, posts):
    cfg(f"BACKEND_URL = {URL}\nbackend_user_key = AbC\n")
    d.publish_backend(PAYLOAD)
    assert posts[0][1]["headers"]["Authorization"] == "Bearer AbC"


def test_backend_ca_pins_the_context(cfg, posts, monkeypatch):
    seen = {}

    def fake_ctx(cafile=None, **kw):
        seen["cafile"] = cafile
        return "CTX"

    monkeypatch.setattr(d.ssl, "create_default_context", fake_ctx)
    cfg(f"backend_url = {URL}\nbackend_user_key = {KEY}\nbackend_ca = /etc/x/ca.pem\n")
    assert d.publish_backend(PAYLOAD) is True
    assert seen["cafile"] == "/etc/x/ca.pem"
    assert posts[0][1]["verify"] == "CTX"


def test_missing_ca_file_is_a_logged_failure_not_a_crash(cfg, posts, tmp_path):
    cfg(f"backend_url = {URL}\nbackend_user_key = {KEY}\nbackend_ca = {tmp_path}/nope.pem\n")
    assert d.publish_backend(PAYLOAD) is False
    assert posts == []


def test_missing_user_key_skips_without_network(cfg, posts):
    cfg(f"backend_url = {URL}\n")
    assert d.publish_backend(PAYLOAD) is False
    assert posts == []


@pytest.mark.parametrize("exc", [httpx.ConnectError(f"boom {KEY}"), httpx.ReadTimeout("t"),
                                 RuntimeError(KEY)])
def test_failure_is_one_generic_line_without_secrets(cfg, monkeypatch, capsys, exc):
    def boom(url, **kw):
        raise exc

    monkeypatch.setattr(d.httpx, "post", boom)
    cfg(f"backend_url = {URL}\nbackend_user_key = {KEY}\n")
    assert d.publish_backend(PAYLOAD) is False
    out = capsys.readouterr()
    text = out.out + out.err
    assert text.count("Backend publish failed") == 1
    assert len(text.strip().splitlines()) == 1
    assert KEY not in text and "Authorization" not in text and "Bearer" not in text


def test_http_error_status_is_generic_failure(cfg, monkeypatch, capsys):
    monkeypatch.setattr(d.httpx, "post", lambda url, **kw: httpx.Response(401, json={"ok": False}))
    cfg(f"backend_url = {URL}\nbackend_user_key = {KEY}\n")
    assert d.publish_backend(PAYLOAD) is False
    out = capsys.readouterr()
    assert KEY not in out.out + out.err


def test_background_is_noop_when_off(cfg, posts):
    async def run():
        return d.publish_backend_background(PAYLOAD)

    assert asyncio.run(run()) is None
    assert posts == []


def test_background_runs_off_the_event_loop(cfg, monkeypatch):
    cfg(f"backend_url = {URL}\nbackend_user_key = {KEY}\n")
    seen = {}

    def slow_post(url, **kw):
        seen["thread"] = threading.current_thread()
        time.sleep(0.3)
        return httpx.Response(200, json={"ok": True})

    monkeypatch.setattr(d.httpx, "post", slow_post)

    async def run():
        t0 = time.monotonic()
        task = d.publish_backend_background(PAYLOAD)
        assert task is not None
        spawn = time.monotonic() - t0
        await asyncio.sleep(0)  # loop stays responsive while the POST sleeps
        assert not task.done()
        assert await task is True
        return spawn

    assert asyncio.run(run()) < 0.1
    assert seen["thread"] is not threading.main_thread()


# --- no-device beat: the standalone pipe must run without BLE ---------------

LIVE = {"s": 10, "sr": 60, "w": 20, "wr": 600, "st": "allowed", "ok": True}


def test_backend_only_beat_polls_and_posts(cfg, posts, monkeypatch, capsys):
    cfg(f"backend_url = {URL}\nbackend_user_key = {KEY}\n")

    async def fake_poll(selector=None):
        return dict(LIVE), False

    monkeypatch.setattr(d, "poll_active", fake_poll)

    async def run():
        task = await d.backend_only_beat()
        assert task is not None
        return await task

    assert asyncio.run(run()) is True
    (url, kw), = posts
    assert url == URL and kw["json"]["s"] == 10 and kw["json"]["w"] == 20
    out = capsys.readouterr()
    assert KEY not in out.out + out.err


def test_backend_only_beat_dead_token_posts_nothing(cfg, posts, monkeypatch):
    cfg(f"backend_url = {URL}\nbackend_user_key = {KEY}\n")

    async def fake_poll(selector=None):
        return None, True

    monkeypatch.setattr(d, "poll_active", fake_poll)
    assert asyncio.run(d.backend_only_beat()) is None
    assert posts == []


def _run_main_without_device(monkeypatch, polls):
    """Run main() with discovery finding nothing until a poll happened."""
    async def fake_poll(selector=None):
        polls.append(1)
        return dict(LIVE), False

    async def no_device(skip_addr=None):
        return None

    monkeypatch.setattr(d, "poll_active", fake_poll)
    monkeypatch.setattr(d, "discover_target", no_device)

    async def run(until):
        task = asyncio.ensure_future(d.main())
        for _ in range(200):
            await asyncio.sleep(0.01)
            if until():
                break
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    return run


def test_main_publishes_when_no_device_found(cfg, posts, monkeypatch):
    cfg(f"backend_url = {URL}\nbackend_user_key = {KEY}\n")
    polls = []
    run = _run_main_without_device(monkeypatch, polls)
    asyncio.run(run(lambda: posts))
    assert len(polls) == 1 and len(posts) == 1
    assert posts[0][0] == URL


def test_main_without_backend_url_does_not_poll_when_no_device(cfg, posts, monkeypatch):
    cfg("")
    polls = []
    run = _run_main_without_device(monkeypatch, polls)
    asyncio.run(run(lambda: False))
    assert polls == [] and posts == []
