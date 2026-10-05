#!/usr/bin/env python3
"""Phase 2 scripted round-trip (acceptance §9.1).

Starts the real backend on an ephemeral port, drives the real `manage.py` CLI
as a subprocess, and walks the whole pairing lifecycle over HTTP:

    pair/start -> manage.py pair -> status returns the token once -> claimed
    -> GET /api/usage works -> revoke-device -> 401

Run from anywhere:

    python3 backend/tests/roundtrip_phase2.py

Exits non-zero on the first failed assertion. Prints no secret except the token
length (the token itself is never echoed).
"""

import http.client
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BACKEND_DIR = os.path.dirname(HERE)
PY = sys.executable
USER_KEY = "u" * 40
CODE = "ABCD2345"
BOARD = "waveshare_amoled_216"
READING = {"s": 45.0, "sr": 120, "w": 28.0, "wr": 7200, "st": "allowed"}


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def run_cli(db, *argv, stdin=None):
    return subprocess.run(
        [PY, os.path.join(BACKEND_DIR, "manage.py"), "--db", db, *argv],
        input=stdin, text=True, capture_output=True)


def call(port, method, path, token=None, body=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    headers = {}
    if token is not None:
        headers["Authorization"] = f"Bearer {token}"
    payload = None
    if body is not None:
        payload = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    conn.request(method, path, body=payload, headers=headers)
    resp = conn.getresponse()
    data = json.loads(resp.read())
    conn.close()
    return resp.status, data


def main():
    tmp = tempfile.mkdtemp(prefix="tokens-meter-pair-")
    db = os.path.join(tmp, "backend.db")
    port = free_port()
    cfg = os.path.join(tmp, "config.json")
    with open(cfg, "w", encoding="utf-8") as f:
        json.dump({"bind": f"127.0.0.1:{port}", "db": db}, f)

    assert run_cli(db, "set-user-key", stdin=USER_KEY + "\n").returncode == 0

    server = subprocess.Popen(
        [PY, os.path.join(BACKEND_DIR, "backend.py"), "--config", cfg],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                if call(port, "GET", "/api/health")[0] == 200:
                    break
            except OSError:
                time.sleep(0.1)
        else:
            raise SystemExit("FAIL: server never became healthy")

        st, d = call(port, "POST", "/api/pair/start",
                     body={"code": CODE, "board": BOARD})
        assert st == 200 and d["state"] == "pending", (st, d)
        print(f"1. POST /api/pair/start        -> {st} {d}")

        p = run_cli(db, "pair", "--code", CODE, "--label", "roundtrip")
        assert p.returncode == 0, p.stderr
        print(f"2. manage.py pair              -> {p.stdout.strip()}")

        st, d = call(port, "GET", f"/api/pair/status?code={CODE}")
        assert st == 200 and d["state"] == "paired" and d.get("token"), (st, d)
        token = d["token"]
        print(f"3. GET /api/pair/status        -> {st} state=paired "
              f"token=<{len(token)} chars, shown once>")

        st, d = call(port, "GET", f"/api/pair/status?code={CODE}")
        assert st == 200 and d["state"] == "claimed" and "token" not in d, (st, d)
        print(f"4. GET /api/pair/status again  -> {st} {d}")

        st, _ = call(port, "POST", "/api/usage", token=USER_KEY, body=READING)
        assert st == 200, st
        st, d = call(port, "GET", "/api/usage", token=token)
        assert st == 200 and d["s"] == 45.0, (st, d)
        print(f"5. GET /api/usage (device tok) -> {st} s={d['s']} st={d['st']}")

        assert run_cli(db, "revoke-device", "1").returncode == 0
        st, d = call(port, "GET", "/api/usage", token=token)
        assert st == 401 and d["err"] == "unauthorized", (st, d)
        print(f"6. revoke-device               -> GET /api/usage {st} {d}")

        print("PASS: phase 2 pairing round-trip complete")
    finally:
        server.terminate()
        try:
            server.wait(5)
        except subprocess.TimeoutExpired:
            server.kill()


if __name__ == "__main__":
    main()
