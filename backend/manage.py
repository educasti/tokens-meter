#!/usr/bin/env python3
"""Admin CLI for the tokens-meter backend (local only).

    manage.py set-user-key              # key read from stdin / hidden prompt
    manage.py mint-device --label "desk"
    manage.py pair --code ABCD2345 --label "desk"   # approve a device's code
    manage.py revoke-device <id>
    manage.py list-devices
    manage.py list-pending

Secrets are hashed (SHA-256) before they touch the database. The user key is
never accepted as an argv value (it would show in `ps`); a device token is
printed once by mint-device and cannot be recovered afterwards. `pair` mints a
token too, but never prints it: the device fetches it once from
`GET /api/pair/status`, which is why the `pairing` row briefly holds it.
"""

import argparse
import base64
import getpass
import secrets
import sys
import time

import backend

MIN_USER_KEY_LEN = 32  # plain SHA-256 is only sound for high-entropy secrets


def _open(args):
    cfg = backend.load_config(args.config)
    db = args.db or cfg["db"]
    backend.init_db(db)
    return backend.connect(db)


def cmd_set_user_key(args):
    if sys.stdin.isatty():
        key = getpass.getpass("New user API key: ")
    else:
        key = sys.stdin.readline().rstrip("\r\n")
    if len(key) < MIN_USER_KEY_LEN:
        print(f"error: user key must be at least {MIN_USER_KEY_LEN} characters "
              "(try: python3 -c 'import secrets; print(secrets.token_urlsafe(32))')",
              file=sys.stderr)
        return 1
    conn = _open(args)
    with conn:
        conn.execute(
            "INSERT INTO owner (id, user_key_hash) VALUES (1, ?) "
            "ON CONFLICT(id) DO UPDATE SET user_key_hash=excluded.user_key_hash",
            (backend.hash_secret(key),))
    conn.close()
    print("user key set")
    return 0


def cmd_mint_device(args):
    token = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode("ascii")
    conn = _open(args)
    with conn:
        cur = conn.execute(
            "INSERT INTO device (token_hash, label, created_at) VALUES (?, ?, ?)",
            (backend.hash_secret(token), args.label, int(time.time())))
        dev_id = cur.lastrowid
    conn.close()
    print(f"device id {dev_id}", file=sys.stderr)
    print(token)  # the only time it exists in the clear
    return 0


def cmd_pair(args):
    """Approve a pending pairing code and mint its device token.

    The token is bound to the code so the device can fetch it exactly once via
    `GET /api/pair/status`; it is never printed here.
    """
    code = args.code
    if not backend.valid_pair_code(code):
        print(f"error: {code!r} is not an {backend.PAIR_CODE_LEN}-char pairing code",
              file=sys.stderr)
        return 1
    conn = _open(args)
    try:
        row = conn.execute(
            "SELECT created_at, paired_at FROM pairing WHERE code=?", (code,)
        ).fetchone()
        if row is None:
            print(f"error: unknown pairing code {code}", file=sys.stderr)
            return 1
        if row["paired_at"] is not None:
            print(f"error: pairing code {code} is already approved", file=sys.stderr)
            return 1
        if backend.pair_expired(row["created_at"]):
            print(f"error: pairing code {code} expired", file=sys.stderr)
            return 1
        token = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode("ascii")
        now = int(time.time())
        with conn:  # one transaction: never leave an orphan device row
            cur = conn.execute(
                "INSERT INTO device (token_hash, label, created_at) VALUES (?, ?, ?)",
                (backend.hash_secret(token), args.label, now))
            dev_id = cur.lastrowid
            # The guarded UPDATE wins a concurrent double-approval exactly once.
            upd = conn.execute(
                "UPDATE pairing SET paired_at=?, device_id=?, token=? "
                "WHERE code=? AND paired_at IS NULL",
                (now, dev_id, token, code))
            if upd.rowcount != 1:
                conn.rollback()  # drop the orphan device row we just inserted
                print(f"error: pairing code {code} is already approved",
                      file=sys.stderr)
                return 1
    finally:
        conn.close()
    print(f"paired {code} -> device id {dev_id} label {args.label!r}")
    return 0


def cmd_revoke_device(args):
    conn = _open(args)
    with conn:
        cur = conn.execute("UPDATE device SET revoked=1 WHERE id=?", (args.id,))
    conn.close()
    if cur.rowcount == 0:
        print(f"error: no device with id {args.id}", file=sys.stderr)
        return 1
    print(f"device {args.id} revoked")
    return 0


def cmd_list_devices(args):
    conn = _open(args)
    rows = conn.execute(
        "SELECT id, label, created_at, revoked FROM device ORDER BY id").fetchall()
    conn.close()
    print(f"{'ID':>4}  {'LABEL':<24}  {'CREATED (UTC)':<20}  STATUS")
    for r in rows:
        created = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(r["created_at"]))
        print(f"{r['id']:>4}  {(r['label'] or ''):<24}  {created:<20}  "
              f"{'revoked' if r['revoked'] else 'active'}")
    return 0


def cmd_list_pending(args):
    conn = _open(args)
    now = int(time.time())
    rows = conn.execute(
        "SELECT code, board, created_at FROM pairing WHERE paired_at IS NULL "
        "ORDER BY created_at").fetchall()
    conn.close()
    print(f"{'CODE':<8}  {'BOARD':<24}  {'AGE':>7}  STATUS")
    for r in rows:
        age = max(0, now - r["created_at"])
        status = "expired" if backend.pair_expired(r["created_at"], now) else "pending"
        print(f"{r['code']:<8}  {(r['board'] or ''):<24}  {age:>6}s  {status}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="tokens-meter backend admin")
    ap.add_argument("--config", help="path to config.json")
    ap.add_argument("--db", help="database path (overrides the config)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("set-user-key", help="set/replace the user API key").set_defaults(fn=cmd_set_user_key)
    p = sub.add_parser("mint-device", help="create a device token (printed once)")
    p.add_argument("--label", default="", help="free-text name, e.g. 'desk'")
    p.set_defaults(fn=cmd_mint_device)
    p = sub.add_parser("pair", help="approve a pending pairing code (mints a device token)")
    p.add_argument("--code", required=True, help="the 8-char code shown on the device")
    p.add_argument("--label", default="", help="free-text name, e.g. 'desk'")
    p.set_defaults(fn=cmd_pair)
    p = sub.add_parser("revoke-device", help="revoke a device token")
    p.add_argument("id", type=int)
    p.set_defaults(fn=cmd_revoke_device)
    sub.add_parser("list-devices", help="list device tokens (no secrets)").set_defaults(fn=cmd_list_devices)
    sub.add_parser("list-pending", help="list pending pairing codes with age").set_defaults(fn=cmd_list_pending)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
