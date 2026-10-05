#!/usr/bin/env python3
"""Tokens-meter backend (phases 1-2): latest Claude usage per owner, over HTTP.

Python 3 stdlib only. Binds a local port; Caddy terminates TLS in front of it.
Contracts: design/backend-wifi/PHASE1-CONTRACT.md and PHASE2-CONTRACT.md §3-5.

    POST /api/usage        Bearer <user api key>   -> store the latest reading
    GET  /api/usage        Bearer <device token>   -> return it
    GET  /api/health       (no auth)
    POST /api/pair/start   (no auth, rate-limited) -> open/refresh a pairing code
    GET  /api/pair/status  (no auth, rate-limited) -> pending|paired(token once)|claimed

Secrets are only ever stored as SHA-256 hashes and are never logged. The one
exception is the device token minted by `manage.py pair`: it must travel to the
device through the status poll, so the `pairing` row holds it in the clear only
between approval and the first fetch, and it is wiped the moment it is claimed.
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
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

API_VERSION = "1"
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CONFIG = {"bind": "127.0.0.1:8080", "db": "data/backend.db"}

MAX_BODY = 4096       # a usage reading is ~100 bytes
MAX_TOKEN_LEN = 512   # longest Authorization credential we bother hashing
REQUEST_TIMEOUT = 10  # seconds a client may stall on a socket read

PAIR_TTL = 900        # a pairing code expires 15 min after pair/start
PAIR_RATE_LIMIT = 30  # pairing requests allowed per IP per minute
MAX_BOARD_LEN = 64    # board name carried by pair/start

# Unambiguous pairing alphabet: no 0/O/1/I/L (PHASE2-CONTRACT §2).
PAIR_ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ"
PAIR_CODE_LEN = 8

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
CREATE TABLE IF NOT EXISTS pairing (
    code       TEXT PRIMARY KEY,
    board      TEXT,
    created_at INTEGER NOT NULL,
    paired_at  INTEGER,
    claimed_at INTEGER,
    device_id  INTEGER REFERENCES device(id),
    token      TEXT);
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


def valid_pair_code(code):
    """True for exactly 8 chars from the unambiguous pairing alphabet."""
    return (isinstance(code, str) and len(code) == PAIR_CODE_LEN
            and all(c in PAIR_ALPHABET for c in code))


def pair_expired(created_at, now=None):
    """A pending code is dead once PAIR_TTL seconds have elapsed."""
    now = int(time.time()) if now is None else now
    return now - created_at > PAIR_TTL


class RateLimiter:
    """Simple fixed-window, per-key in-memory counter (PHASE2-CONTRACT §3)."""

    def __init__(self, limit, window=60):
        self.limit = limit
        self.window = window
        self._lock = threading.Lock()
        self._buckets = {}  # key -> (window_start, count)

    def allow(self, key):
        now = int(time.time())
        start = now - (now % self.window)
        with self._lock:
            bucket_start, count = self._buckets.get(key, (start, 0))
            if bucket_start != start:
                count = 0
            count += 1
            self._buckets[key] = (start, count)
            return count <= self.limit


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

    def _json_body(self):
        """Parsed JSON object, or None if absent/malformed/not an object."""
        body = self._read_body()
        if body is None:
            return None
        try:
            obj = json.loads(body.decode("utf-8"), parse_constant=_no_constants)
        except (ValueError, UnicodeDecodeError):
            return None
        return obj if isinstance(obj, dict) else None

    # --- pairing (unauthenticated, rate-limited)
    def _client_ip(self):
        """Real client IP: Caddy sets X-Forwarded-For; else the socket peer."""
        forwarded = self.headers.get("X-Forwarded-For", "")
        if forwarded:
            return forwarded.split(",")[0].strip()
        return self.client_address[0]

    def _pair_rate_ok(self):
        """Reply 429 and return False when this IP is over the limit."""
        if not self.server.rate_limiter.allow(self._client_ip()):
            self._err(429, "rate_limited")
            return False
        return True

    def _pair_start(self):
        if not self._pair_rate_ok():
            return
        obj = self._json_body()
        if obj is None:
            return self._err(400, "bad_json")
        code = obj.get("code")
        if not valid_pair_code(code):
            return self._err(400, "bad_code")
        board = obj.get("board")
        if board is not None and not (isinstance(board, str)
                                      and len(board) <= MAX_BOARD_LEN):
            return self._err(400, "bad_json")
        now = int(time.time())
        conn = connect(self.server.db_path)
        try:
            with conn:  # re-start refreshes created_at (idempotent)
                conn.execute(
                    "INSERT INTO pairing (code, board, created_at) VALUES (?, ?, ?) "
                    "ON CONFLICT(code) DO UPDATE SET board=excluded.board, "
                    "created_at=excluded.created_at",
                    (code, board, now))
        finally:
            conn.close()
        self._send(200, {"ok": True, "state": "pending", "expires_in": PAIR_TTL})

    def _pair_status(self):
        if not self._pair_rate_ok():
            return
        code = parse_qs(urlsplit(self.path).query).get("code", [""])[0]
        if not valid_pair_code(code):
            return self._err(400, "bad_code")
        conn = connect(self.server.db_path)
        try:
            row = conn.execute(
                "SELECT created_at, paired_at, token FROM pairing WHERE code=?",
                (code,)).fetchone()
            if row is None:
                return self._err(404, "unknown_code")
            if row["paired_at"] is None:
                if pair_expired(row["created_at"]):
                    return self._err(410, "expired")
                return self._send(200, {"ok": True, "state": "pending"})
            # Approved: the token is handed out exactly once. The guarded UPDATE
            # makes concurrent polls race-safe (only the winner sees rowcount 1).
            with conn:
                cur = conn.execute(
                    "UPDATE pairing SET claimed_at=?, token=NULL "
                    "WHERE code=? AND claimed_at IS NULL", (int(time.time()), code))
            if cur.rowcount == 1 and row["token"] is not None:
                return self._send(200, {"ok": True, "state": "paired",
                                        "token": row["token"]})
            return self._send(200, {"ok": True, "state": "claimed"})
        finally:
            conn.close()

    # --- routes
    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/api/health":
            return self._send(200, {"ok": True, "version": API_VERSION})
        if path == "/api/pair/status":
            return self._pair_status()
        if path == "/api/pair/start":
            return self._method_not_allowed()
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
        if path == "/api/pair/start":
            return self._pair_start()
        if path == "/api/pair/status":
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

    def __init__(self, addr, db_path, rate_limit=PAIR_RATE_LIMIT):
        self.db_path = db_path
        self.rate_limiter = RateLimiter(rate_limit)
        super().__init__(addr, Handler)


def make_server(db_path, host="127.0.0.1", port=8080, rate_limit=PAIR_RATE_LIMIT):
    init_db(db_path)
    return Server((host, port), db_path, rate_limit)


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
