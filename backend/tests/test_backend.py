"""Backend tests: real server on an ephemeral port, real SQLite in tmp_path."""

import http.client
import io
import json
import logging
import os
import sqlite3
import sys
import threading

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import backend  # noqa: E402
import manage  # noqa: E402

USER_KEY = "u" * 40
READING = {"s": 45.0, "sr": 120, "w": 28.0, "wr": 7200, "st": "allowed", "t": 1791000000}


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


def admin(db, *argv, stdin=None, monkeypatch=None):
    if stdin is not None:
        monkeypatch.setattr(sys, "stdin", io.StringIO(stdin))
    return manage.main(["--db", db, *argv])


@pytest.fixture
def keyed(db, srv, monkeypatch):
    assert admin(db, "set-user-key", stdin=USER_KEY + "\n", monkeypatch=monkeypatch) == 0
    return srv


def mint(db, capsys, label="desk"):
    assert admin(db, "mint-device", "--label", label) == 0
    return capsys.readouterr().out.strip()


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


def test_health_needs_no_auth(srv):
    assert call(srv, "GET", "/api/health") == (200, {"ok": True, "version": "1"})


def test_roundtrip_user_post_then_device_get(keyed, db, capsys):
    token = mint(db, capsys)
    status, data = call(keyed, "POST", "/api/usage", USER_KEY, READING)
    assert status == 200 and data["ok"] and isinstance(data["updated_at"], int)
    status, got = call(keyed, "GET", "/api/usage", token)
    assert status == 200
    assert {k: got[k] for k in ("s", "sr", "w", "wr", "st")} == \
        {k: READING[k] for k in ("s", "sr", "w", "wr", "st")}
    assert got["ok"] is True and got["updated_at"] == data["updated_at"]


def test_post_replaces_latest(keyed, db, capsys):
    token = mint(db, capsys)
    call(keyed, "POST", "/api/usage", USER_KEY, READING)
    call(keyed, "POST", "/api/usage", USER_KEY, {**READING, "s": 90.5, "st": "limited"})
    _, got = call(keyed, "GET", "/api/usage", token)
    assert got["s"] == 90.5 and got["st"] == "limited"


def test_t_is_optional(keyed, db, capsys):
    body = {k: v for k, v in READING.items() if k != "t"}
    assert call(keyed, "POST", "/api/usage", USER_KEY, body)[0] == 200


def test_get_before_any_post_is_no_data(keyed, db, capsys):
    token = mint(db, capsys)
    assert call(keyed, "GET", "/api/usage", token) == (404, {"ok": False, "err": "no_data"})


@pytest.mark.parametrize("token", [None, "wrong-key-" + "x" * 30])
def test_post_rejects_missing_or_wrong_user_key(keyed, token):
    assert call(keyed, "POST", "/api/usage", token, READING) == \
        (401, {"ok": False, "err": "unauthorized"})


def test_post_without_any_owner_is_401(srv):
    assert call(srv, "POST", "/api/usage", USER_KEY, READING)[0] == 401


@pytest.mark.parametrize("token", [None, "not-a-device-token"])
def test_get_rejects_missing_or_unknown_device_token(keyed, token):
    assert call(keyed, "GET", "/api/usage", token) == \
        (401, {"ok": False, "err": "unauthorized"})


def test_credentials_are_not_interchangeable(keyed, db, capsys):
    token = mint(db, capsys)
    call(keyed, "POST", "/api/usage", USER_KEY, READING)
    assert call(keyed, "GET", "/api/usage", USER_KEY)[0] == 401
    assert call(keyed, "POST", "/api/usage", token, READING)[0] == 401


def test_non_bearer_scheme_rejected(keyed, db, capsys):
    token = mint(db, capsys)
    assert call(keyed, "GET", "/api/usage",
                headers={"Authorization": f"Basic {token}"})[0] == 401


def test_revoked_device_gets_401(keyed, db, capsys):
    token = mint(db, capsys)
    call(keyed, "POST", "/api/usage", USER_KEY, READING)
    assert call(keyed, "GET", "/api/usage", token)[0] == 200
    assert admin(db, "revoke-device", "1") == 0
    assert call(keyed, "GET", "/api/usage", token)[0] == 401


def test_revoke_unknown_device_fails(db, capsys):
    assert admin(db, "revoke-device", "99") == 1


@pytest.mark.parametrize("body", [
    b"", b"not json", b"[]", b'"x"', b"{}",
    b'{"s":NaN,"sr":1,"w":1,"wr":1,"st":"ok"}',
    json.dumps({**READING, "s": "45"}).encode(),
    json.dumps({**READING, "s": True}).encode(),
    json.dumps({**READING, "sr": 1.5}).encode(),
    json.dumps({**READING, "wr": -2}).encode(),
    json.dumps({**READING, "st": ""}).encode(),
    json.dumps({**READING, "st": 5}).encode(),
    json.dumps({**READING, "t": "now"}).encode(),
    b"\xff\xfe",
    b"{" + b" " * 5000 + b"}",  # over the body limit
])
def test_bad_body_is_400(keyed, body):
    assert call(keyed, "POST", "/api/usage", USER_KEY, body) == \
        (400, {"ok": False, "err": "bad_json"})


def test_missing_content_length_is_400(keyed):
    conn = http.client.HTTPConnection("127.0.0.1", keyed.server_address[1], timeout=5)
    conn.putrequest("POST", "/api/usage")
    conn.putheader("Authorization", f"Bearer {USER_KEY}")
    conn.endheaders()
    resp = conn.getresponse()
    assert resp.status == 400
    conn.close()


def test_bad_body_does_not_clobber_stored_reading(keyed, db, capsys):
    token = mint(db, capsys)
    call(keyed, "POST", "/api/usage", USER_KEY, READING)
    call(keyed, "POST", "/api/usage", USER_KEY, {"s": 1})
    assert call(keyed, "GET", "/api/usage", token)[1]["s"] == 45.0


def test_unknown_route_and_method(keyed):
    assert call(keyed, "GET", "/api/nope")[0] == 404
    assert call(keyed, "GET", "/")[0] == 404
    assert call(keyed, "POST", "/api/health", body={})[0] == 405
    assert call(keyed, "DELETE", "/api/usage")[0] == 405


def test_query_string_ignored(keyed):
    assert call(keyed, "GET", "/api/health?x=1")[0] == 200


def test_secrets_hashed_at_rest(keyed, db, capsys):
    token = mint(db, capsys)
    call(keyed, "POST", "/api/usage", USER_KEY, READING)
    conn = sqlite3.connect(db)
    owner = conn.execute("SELECT user_key_hash FROM owner").fetchone()[0]
    dev = conn.execute("SELECT token_hash FROM device").fetchone()[0]
    conn.close()
    assert owner == backend.hash_secret(USER_KEY) and len(owner) == 64
    assert dev == backend.hash_secret(token) and len(dev) == 64
    raw = open(db, "rb").read() + open(db + "-wal", "rb").read() \
        if os.path.exists(db + "-wal") else open(db, "rb").read()
    assert USER_KEY.encode() not in raw and token.encode() not in raw


def test_db_file_is_owner_only(srv, db):
    assert os.stat(db).st_mode & 0o077 == 0


def test_no_credential_in_logs(keyed, db, capsys, caplog):
    token = mint(db, capsys)
    with caplog.at_level(logging.DEBUG):
        call(keyed, "POST", "/api/usage", USER_KEY, READING)
        call(keyed, "GET", "/api/usage", token)
        call(keyed, "GET", "/api/usage", "bad-secret-value-123")
        call(keyed, "GET", "/api/usage?token=" + token)
    text = caplog.text
    assert "POST /api/usage 200" in text and "GET /api/usage 401" in text
    for secret in (USER_KEY, token, "bad-secret-value-123", "Authorization", "Bearer"):
        assert secret not in text


def test_minted_tokens_are_unique_and_32_bytes(db, capsys):
    a, b = mint(db, capsys, "a"), mint(db, capsys, "b")
    assert a != b
    import base64
    assert len(base64.urlsafe_b64decode(a + "=" * (-len(a) % 4))) == 32


def test_list_devices_shows_no_tokens(db, capsys):
    token = mint(db, capsys, "kitchen")
    admin(db, "revoke-device", "1")
    assert admin(db, "list-devices") == 0
    out = capsys.readouterr().out
    assert "kitchen" in out and "revoked" in out and token not in out


def test_short_user_key_refused(db, monkeypatch, capsys):
    assert admin(db, "set-user-key", stdin="short\n", monkeypatch=monkeypatch) == 1


def test_set_user_key_replaces_old_key(keyed, db, monkeypatch):
    new_key = "n" * 40
    assert admin(db, "set-user-key", stdin=new_key + "\n", monkeypatch=monkeypatch) == 0
    assert call(keyed, "POST", "/api/usage", USER_KEY, READING)[0] == 401
    assert call(keyed, "POST", "/api/usage", new_key, READING)[0] == 200


def test_load_config_resolves_db_relative_to_config(tmp_path):
    cfg = tmp_path / "c.json"
    cfg.write_text(json.dumps({"bind": "127.0.0.1:9999", "db": "d/x.db"}))
    loaded = backend.load_config(str(cfg))
    assert loaded == {"bind": "127.0.0.1:9999", "db": str(tmp_path / "d" / "x.db")}
    assert backend.parse_bind(loaded["bind"]) == ("127.0.0.1", 9999)
    with pytest.raises(ValueError):
        backend.parse_bind("nonsense")
