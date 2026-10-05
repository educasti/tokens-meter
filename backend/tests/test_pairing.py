"""Phase 2 pairing tests: real server on an ephemeral port, real SQLite.

Contract: design/backend-wifi/PHASE2-CONTRACT.md sections 2-5.
"""

import base64
import http.client
import io
import json
import os
import sqlite3
import sys
import threading
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import backend  # noqa: E402
import manage  # noqa: E402

USER_KEY = "u" * 40
CODE = "ABCD2345"
BOARD = "waveshare_amoled_216"
READING = {"s": 45.0, "sr": 120, "w": 28.0, "wr": 7200, "st": "allowed"}


@pytest.fixture
def db(tmp_path):
    return str(tmp_path / "data" / "backend.db")


@pytest.fixture
def srv(db):
    server = backend.make_server(db, "127.0.0.1", 0)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
    thread.start()
    yield server
    server.shutdown()
    server.server_close()
    thread.join(5)


@pytest.fixture
def tiny_srv(db):
    """A server whose pairing rate limit is small enough to trip in a test."""
    server = backend.make_server(db, "127.0.0.1", 0, rate_limit=2)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
    thread.start()
    yield server
    server.shutdown()
    server.server_close()
    thread.join(5)


def admin(db, *argv, stdin=None, monkeypatch=None):
    if stdin is not None:
        monkeypatch.setattr(sys, "stdin", io.StringIO(stdin))
    return manage.main(["--db", db, *argv])


@pytest.fixture
def keyed(db, srv, monkeypatch):
    assert admin(db, "set-user-key", stdin=USER_KEY + "\n", monkeypatch=monkeypatch) == 0
    return srv


def call(srv, method, path, token=None, body=None, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=5)
    h = dict(headers or {})
    if token is not None:
        h["Authorization"] = f"Bearer {token}"
    if body is not None and not isinstance(body, bytes):
        body = json.dumps(body).encode()
    conn.request(method, path, body=body, headers=h)
    resp = conn.getresponse()
    data = json.loads(resp.read())
    conn.close()
    return resp.status, data


def start(srv, code=CODE, board=BOARD):
    return call(srv, "POST", "/api/pair/start", body={"code": code, "board": board})


def status(srv, code=CODE):
    return call(srv, "GET", f"/api/pair/status?code={code}")


def approve(db, code=CODE, label="desk", capsys=None):
    rc = admin(db, "pair", "--code", code, "--label", label)
    out = capsys.readouterr().out if capsys is not None else ""
    return rc, out


def backdate(db, code, seconds):
    conn = sqlite3.connect(db)
    with conn:
        conn.execute("UPDATE pairing SET created_at=created_at-? WHERE code=?",
                     (seconds, code))
    conn.close()


# --------------------------------------------------------------- pair/start

def test_start_returns_pending_with_ttl(srv):
    code_status, data = start(srv)
    assert code_status == 200
    assert data == {"ok": True, "state": "pending", "expires_in": backend.PAIR_TTL}


def test_start_is_idempotent_and_refreshes_created_at(srv, db):
    assert start(srv)[0] == 200
    backdate(db, CODE, backend.PAIR_TTL - 5)
    assert start(srv)[0] == 200
    conn = sqlite3.connect(db)
    created, count = conn.execute(
        "SELECT created_at, COUNT(*) FROM pairing WHERE code=?", (CODE,)).fetchone()
    conn.close()
    assert count == 1
    assert time.time() - created < 5  # refreshed, not still the backdated value


@pytest.mark.parametrize("code", [
    "", "ABC", "ABCD23456", "ABCD2340", "ABCD234O", "abcd2345", "ABCD 345", None, 1234,
])
def test_start_rejects_bad_code(srv, code):
    body = {"code": code, "board": BOARD} if code is not None else {"board": BOARD}
    assert call(srv, "POST", "/api/pair/start", body=body) == \
        (400, {"ok": False, "err": "bad_code"})


@pytest.mark.parametrize("body", [
    b"", b"not json", b"[]", b'"x"', b'{"code":NaN}', b"{" + b" " * 5000 + b"}",
])
def test_start_bad_json_is_400(srv, body):
    assert call(srv, "POST", "/api/pair/start", body=body) == \
        (400, {"ok": False, "err": "bad_json"})


def test_start_board_is_optional(srv):
    assert call(srv, "POST", "/api/pair/start", body={"code": CODE})[0] == 200


def test_start_rejects_bad_board(srv):
    assert call(srv, "POST", "/api/pair/start",
                body={"code": CODE, "board": 5}) == (400, {"ok": False, "err": "bad_json"})
    assert call(srv, "POST", "/api/pair/start",
                body={"code": CODE, "board": "x" * 65}) == (400, {"ok": False, "err": "bad_json"})


# --------------------------------------------------------------- pair/status

def test_status_unknown_code(srv):
    assert status(srv, "ZZZZ2345") == (404, {"ok": False, "err": "unknown_code"})


@pytest.mark.parametrize("code", ["", "ABC", "ABCD2340", "ABCD234O"])
def test_status_bad_code_is_400(srv, code):
    assert call(srv, "GET", f"/api/pair/status?code={code}") == \
        (400, {"ok": False, "err": "bad_code"})


def test_status_without_code_is_400(srv):
    assert call(srv, "GET", "/api/pair/status") == (400, {"ok": False, "err": "bad_code"})


def test_status_pending(srv):
    start(srv)
    assert status(srv) == (200, {"ok": True, "state": "pending"})


def test_status_expired_before_approval(srv, db):
    start(srv)
    backdate(db, CODE, backend.PAIR_TTL + 1)
    assert status(srv) == (410, {"ok": False, "err": "expired"})


# --------------------------------------------------------------- full flow

def test_full_pairing_roundtrip(srv, db, monkeypatch, capsys):
    # 1. device opens a code
    assert start(srv)[0] == 200
    assert status(srv)[1]["state"] == "pending"

    # 2. owner approves it (token never printed)
    rc, out = approve(db, capsys=capsys)
    assert rc == 0 and "device id" in out and "desk" in out

    # 3. device fetches the token exactly once
    code_status, data = status(srv)
    assert code_status == 200 and data["state"] == "paired"
    token = data["token"]
    assert token and token not in out
    assert len(base64.urlsafe_b64decode(token + "=" * (-len(token) % 4))) == 32

    # 4. a second poll says claimed, with no token
    assert status(srv) == (200, {"ok": True, "state": "claimed"})

    # 5. the token works for GET /api/usage
    assert admin(db, "set-user-key", stdin=USER_KEY + "\n", monkeypatch=monkeypatch) == 0
    assert call(srv, "POST", "/api/usage", USER_KEY, READING)[0] == 200
    got_status, got = call(srv, "GET", "/api/usage", token)
    assert got_status == 200 and got["s"] == 45.0

    # 6. revocation -> 401
    assert admin(db, "revoke-device", "1") == 0
    assert call(srv, "GET", "/api/usage", token) == (401, {"ok": False, "err": "unauthorized"})


def test_token_is_wiped_and_hashed_after_claim(srv, db, capsys):
    start(srv)
    approve(db, capsys=capsys)
    token = status(srv)[1]["token"]
    conn = sqlite3.connect(db)
    pairing_token, claimed_at = conn.execute(
        "SELECT token, claimed_at FROM pairing WHERE code=?", (CODE,)).fetchone()
    device_hash = conn.execute("SELECT token_hash FROM device WHERE id=1").fetchone()[0]
    conn.close()
    assert pairing_token is None and claimed_at is not None
    assert device_hash == backend.hash_secret(token)


def test_restart_after_claim_does_not_reissue(srv, db, capsys):
    start(srv)
    approve(db, capsys=capsys)
    status(srv)  # claim
    start(srv)   # device re-opens the same code (idempotent refresh)
    assert status(srv) == (200, {"ok": True, "state": "claimed"})


def test_concurrent_status_yields_token_once(srv, db, capsys):
    start(srv)
    approve(db, capsys=capsys)
    results = []
    lock = threading.Lock()

    def poll():
        st, data = status(srv)
        with lock:
            results.append((st, data))

    threads = [threading.Thread(target=poll) for _ in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(5)
    tokens = [d["token"] for _, d in results if d.get("state") == "paired"]
    assert len(tokens) == 1
    assert all(st == 200 for st, _ in results)


# --------------------------------------------------------------- manage CLI

def test_pair_unknown_code_fails(db, capsys):
    assert approve(db, code="ZZZZ2345")[0] == 1
    assert "unknown" in capsys.readouterr().err.lower()


def test_pair_expired_code_fails(srv, db, capsys):
    start(srv)
    backdate(db, CODE, backend.PAIR_TTL + 1)
    assert approve(db)[0] == 1
    assert "expired" in capsys.readouterr().err.lower()
    # no device was created for a rejected code
    conn = sqlite3.connect(db)
    assert conn.execute("SELECT COUNT(*) FROM device").fetchone()[0] == 0
    conn.close()


def test_pair_twice_fails(srv, db, capsys):
    start(srv)
    assert approve(db, capsys=capsys)[0] == 0
    assert approve(db)[0] == 1
    assert "already" in capsys.readouterr().err.lower()


def test_pair_invalid_code_fails(db, capsys):
    assert approve(db, code="nope")[0] == 1


def test_list_pending_shows_age_and_no_secret(srv, db, capsys):
    start(srv)
    assert admin(db, "list-pending") == 0
    out = capsys.readouterr().out
    assert CODE in out and "pending" in out
    approve(db, capsys=capsys)
    assert admin(db, "list-pending") == 0
    assert CODE not in capsys.readouterr().out  # no longer pending


def test_list_pending_marks_expired(srv, db, capsys):
    start(srv)
    backdate(db, CODE, backend.PAIR_TTL + 5)
    assert admin(db, "list-pending") == 0
    assert "expired" in capsys.readouterr().out


def test_list_devices_does_not_leak_pairing_token(srv, db, capsys):
    start(srv)
    approve(db, capsys=capsys)
    token = status(srv)[1]["token"]
    assert admin(db, "list-devices") == 0
    assert token not in capsys.readouterr().out


# --------------------------------------------------------------- rate limit

def test_rate_limit_returns_429(tiny_srv):
    assert start(tiny_srv)[0] == 200
    assert start(tiny_srv)[0] == 200
    assert start(tiny_srv) == (429, {"ok": False, "err": "rate_limited"})


def test_rate_limit_is_per_forwarded_ip(tiny_srv):
    for _ in range(3):
        call(tiny_srv, "POST", "/api/pair/start", body={"code": CODE},
             headers={"X-Forwarded-For": "10.0.0.1"})
    assert call(tiny_srv, "POST", "/api/pair/start", body={"code": CODE},
                headers={"X-Forwarded-For": "10.0.0.2"})[0] == 200


def test_rate_limit_covers_status(tiny_srv):
    # the counter is shared across both pairing endpoints for one IP
    assert start(tiny_srv)[0] == 200
    assert status(tiny_srv)[0] == 200
    assert status(tiny_srv) == (429, {"ok": False, "err": "rate_limited"})


# --------------------------------------------------------------- methods

def test_wrong_methods(srv):
    assert call(srv, "GET", "/api/pair/start")[0] == 405
    assert call(srv, "POST", "/api/pair/status?code=" + CODE, body={})[0] == 405


def test_pairing_is_unauthenticated(srv):
    # no Authorization header anywhere in the flow
    assert start(srv)[0] == 200
    assert status(srv)[0] == 200


def test_pairing_secrets_not_logged(srv, db, capsys, caplog):
    import logging
    start(srv)
    approve(db, capsys=capsys)
    token = status(srv)[1]["token"]
    with caplog.at_level(logging.DEBUG):
        start(srv)
        status(srv)
    text = caplog.text
    assert "POST /api/pair/start 200" in text and "GET /api/pair/status 200" in text
    assert CODE not in text and token not in text and "code=" not in text
