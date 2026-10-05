#!/usr/bin/env python3
"""Tokens-meter backend (phase 1): latest Claude usage per owner, over HTTP.

Python 3 stdlib only. Binds a local port; Caddy terminates TLS in front of it.
Contract: design/backend-wifi/PHASE1-CONTRACT.md sections 3-5.

    POST /api/usage    Bearer <user api key>      -> store the latest reading
    GET  /api/usage    Bearer <device token>      -> return it
    GET  /api/health   (no auth)

Secrets are only ever stored as SHA-256 hashes and are never logged.
"""

import argparse
import hashlib
import hmac
import json
import logging
import math
import os
import sqlite3
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

API_VERSION = "1"
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CONFIG = {"bind": "127.0.0.1:8080", "db": "data/backend.db"}

MAX_BODY = 4096       # a usage reading is ~100 bytes
MAX_TOKEN_LEN = 512   # longest Authorization credential we bother hashing
REQUEST_TIMEOUT = 10  # seconds a client may stall on a socket read

log = logging.getLogger("backend")

SCHEMA = """
CREATE TABLE IF NOT EXISTS owner (
    id INTEGER PRIMARY KEY CHECK (id=1),
    user_key_hash TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS device (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    token_hash TEXT NOT NULL UNIQUE, label TEXT,
    created_at INTEGER NOT NULL, revoked INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS usage (
    owner_id INTEGER PRIMARY KEY REFERENCES owner(id),
    s REAL, sr INTEGER, w REAL, wr INTEGER, st TEXT,
    updated_at INTEGER NOT NULL);
"""


# ---------------------------------------------------------------- config / db

def hash_secret(secret):
    """SHA-256 hex of a secret. Both secrets are 256-bit random, so no salt."""
    return hashlib.sha256(secret.encode("utf-8")).hexdigest()


def load_config(path=None):
    """Return {"bind": "host:port", "db": <absolute path>}.

    A relative `db` resolves against the config file's directory (or this
    directory when there is no config file), not the cwd, so systemd and a
    shell see the same database.
    """
    cfg = dict(DEFAULT_CONFIG)
    base = HERE
    if path is None:
        candidate = os.path.join(HERE, "config.json")
        path = candidate if os.path.exists(candidate) else None
    if path is not None:
        with open(path, "r", encoding="utf-8") as f:
            loaded = json.load(f)
        if not isinstance(loaded, dict):
            raise ValueError("config must be a JSON object")
        cfg.update({k: loaded[k] for k in DEFAULT_CONFIG if k in loaded})
        base = os.path.dirname(os.path.abspath(path))
    if not os.path.isabs(cfg["db"]):
        cfg["db"] = os.path.join(base, cfg["db"])
    return cfg


def parse_bind(bind):
    host, sep, port = str(bind).rpartition(":")
    if not sep or not host or not port.isdigit():
        raise ValueError("bind must look like host:port")
    return host, int(port)


def connect(db_path):
    conn = sqlite3.connect(db_path, timeout=5)
    conn.row_factory = sqlite3.Row
    return conn


def init_db(db_path):
    """Create the database (owner-only perms) and schema if missing."""
    parent = os.path.dirname(db_path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    fresh = not os.path.exists(db_path)
    conn = connect(db_path)
    try:
        conn.execute("PRAGMA journal_mode=WAL")
        conn.executescript(SCHEMA)
        conn.commit()
    finally:
        conn.close()
    if fresh:
        os.chmod(db_path, 0o600)


# ------------------------------------------------------------------- handler

def _no_constants(name):
    raise ValueError("non-finite number")


def _is_number(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


def _is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def validate_reading(obj):
    """Return the stored row (s, sr, w, wr, st) or None if the body is bad."""
    if not isinstance(obj, dict):
        return None
    s, sr, w, wr, st = (obj.get(k) for k in ("s", "sr", "w", "wr", "st"))
    if not (_is_number(s) and _is_number(w)):
        return None
    if not (_is_int(sr) and _is_int(wr) and sr >= -1 and wr >= -1):
        return None
    if not (isinstance(st, str) and 0 < len(st) <= 32):
        return None
    if "t" in obj and not (_is_number(obj["t"]) and obj["t"] >= 0):
        return None
    return float(s), sr, float(w), wr, st


class Handler(BaseHTTPRequestHandler):
    server_version = "tokens-meter-backend"
    sys_version = ""
    timeout = REQUEST_TIMEOUT

    # --- logging: method, path (no query), status. Never headers or tokens.
    def log_request(self, code="-", size="-"):
        log.info("%s %s %s", self.command, urlsplit(self.path).path, code)

    def log_message(self, fmt, *args):
        log.info("%s", fmt % args)

    # --- helpers
    def _send(self, status, obj):
        body = json.dumps(obj, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _err(self, status, code):
        self._send(status, {"ok": False, "err": code})

    def _bearer(self):
        header = self.headers.get("Authorization", "")
        scheme, _, token = header.partition(" ")
        token = token.strip()
        if scheme.lower() != "bearer" or not token or len(token) > MAX_TOKEN_LEN:
            return None
        return token

    def _auth_user(self, conn):
        token = self._bearer()
        row = conn.execute("SELECT user_key_hash FROM owner WHERE id=1").fetchone()
        if token is None or row is None:
            return False
        return hmac.compare_digest(hash_secret(token), row["user_key_hash"])

    def _auth_device(self, conn):
        token = self._bearer()
        if token is None:
            return False
        row = conn.execute(
            "SELECT revoked FROM device WHERE token_hash=?", (hash_secret(token),)
        ).fetchone()
        return row is not None and not row["revoked"]

    def _read_body(self):
        """Body bytes, or None if absent/oversized/malformed length."""
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            return None
        if length < 0 or length > MAX_BODY:
            return None
        return self.rfile.read(length)

    # --- routes
    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/api/health":
            return self._send(200, {"ok": True, "version": API_VERSION})
        if path != "/api/usage":
            return self._err(404, "not_found")
        conn = connect(self.server.db_path)
        try:
            if not self._auth_device(conn):
                return self._err(401, "unauthorized")
            row = conn.execute(
                "SELECT s, sr, w, wr, st, updated_at FROM usage WHERE owner_id=1"
            ).fetchone()
        finally:
            conn.close()
        if row is None:
            return self._err(404, "no_data")
        self._send(200, {"ok": True, "s": row["s"], "sr": row["sr"], "w": row["w"],
                         "wr": row["wr"], "st": row["st"],
                         "updated_at": row["updated_at"]})

    def do_POST(self):
        path = urlsplit(self.path).path
        if path == "/api/health":
            return self._method_not_allowed()
        if path != "/api/usage":
            return self._err(404, "not_found")
        conn = connect(self.server.db_path)
        try:
            body = self._read_body()  # drained before replying so a 401 isn't an RST
            if not self._auth_user(conn):
                return self._err(401, "unauthorized")
            if body is None:
                return self._err(400, "bad_json")
            try:
                obj = json.loads(body.decode("utf-8"), parse_constant=_no_constants)
            except (ValueError, UnicodeDecodeError):
                return self._err(400, "bad_json")
            reading = validate_reading(obj)
            if reading is None:
                return self._err(400, "bad_json")
            now = int(time.time())
            conn.execute(
                "INSERT INTO usage (owner_id, s, sr, w, wr, st, updated_at) "
                "VALUES (1, ?, ?, ?, ?, ?, ?) "
                "ON CONFLICT(owner_id) DO UPDATE SET s=excluded.s, sr=excluded.sr, "
                "w=excluded.w, wr=excluded.wr, st=excluded.st, "
                "updated_at=excluded.updated_at",
                (*reading, now),
            )
            conn.commit()
        finally:
            conn.close()
        self._send(200, {"ok": True, "updated_at": now})

    def _method_not_allowed(self):
        self._err(405, "method_not_allowed")

    do_PUT = do_DELETE = do_PATCH = _method_not_allowed


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, addr, db_path):
        self.db_path = db_path
        super().__init__(addr, Handler)


def make_server(db_path, host="127.0.0.1", port=8080):
    init_db(db_path)
    return Server((host, port), db_path)


def main(argv=None):
    ap = argparse.ArgumentParser(description="tokens-meter usage backend")
    ap.add_argument("--config", help="path to config.json (default: backend/config.json if present)")
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, stream=sys.stderr,
                        format="%(asctime)s %(message)s")
    cfg = load_config(args.config)
    host, port = parse_bind(cfg["bind"])
    server = make_server(cfg["db"], host, port)
    log.info("listening on %s:%d (db %s)", host, port, cfg["db"])
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
