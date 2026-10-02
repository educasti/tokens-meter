# OTA pull — operator runbook (self-signed cert, reserved IP, no domain)

Contract: [`../DESIGN.md`](../DESIGN.md) §4-§14 and decisions D1/D3; plan:
[`../IMPL.md`](../IMPL.md). This runbook is the concrete "stand up and operate"
companion to the design, for the case the design now assumes: the VM has a
**reserved public IP and no DNS name**, so Caddy serves an **explicit self-signed
ECDSA P-256 certificate** (no ACME) and the device **pins that certificate** with
`setCACert()` (DESIGN §8.1). Everything else (manifest, SHA-256, dual slot,
rollback, battery gate, BLE commands) is unchanged from the frozen contract.

Placeholders used throughout:

| Placeholder | Meaning |
|---|---|
| `<IP>` | the reserved public IP of the Oracle VM, e.g. `130.61.10.20` |
| `<user>` | the Linux login that owns `/srv/firmware` and whose `authorized_keys` accepts the deploy key |
| `<repo-path>` | absolute path of this checkout on the build machine, e.g. `/home/educasti/Projects/tokens-meter` |

Commands assume Ubuntu/Oracle Linux on Oracle Cloud. Run shell steps on the
build machine unless prefixed with `sudo`/an `ssh` hop.

---

## 1. Reserve a static public IP and open 443 in both firewall layers

Oracle ephemeral public IPs are released and re-assigned on stop/start, which
breaks the firmware pin and any DNS. Reserve first (DESIGN §4; backend ROADMAP
§10).

1. Reserve and assign a **reserved public IP** (Console: *Networking → IP
   management → Reserved public IPs → Reserve*; or CLI):

   ```bash
   oci network public-ip create \
     --compartment-id <compartment-ocid> \
     --lifetime RESERVED \
     --display-name clawdmeter-ota \
     --availability-domain <AD-1>
   ```

   Then assign it to the instance's primary VNIC (Console: instance → *Attached
   VNICs → primary VNIC → Assign public IP → Reserved*), and note the address as
   `<IP>`. Reboot the instance once and confirm `<IP>` is unchanged.

2. **Security List (VPC layer).** Console: *Networking → Virtual Cloud Networks
   → <VCN> → Security Lists → Default Security List → Add Ingress Rules*:

   - Stateless: **No**
   - Source Type: **CIDR**, Source CIDR: `0.0.0.0/0`
   - IP Protocol: **TCP**, Destination Port Range: `443` (and `80` only if you
     ever want to use it; pull OTA is 443-only).

   If the VNIC uses an NSG instead, add the rule to the NSG (safer CLI form that
   keeps existing rules):

   ```bash
   oci network nsg rules add --nsg-id <nsg-ocid> --security-rules '[{
     "direction":"INGRESS","protocol":"6","source":"0.0.0.0/0",
     "sourceType":"CIDR_BLOCK",
     "tcpOptions":{"destinationPortRange":{"min":443,"max":443}}}]'
   ```

3. **Image iptables (host layer).** Oracle images reject inbound traffic by
   default even after the Security List is opened. Insert the rule **before** the
   image's REJECT rule and persist it:

   ```bash
   sudo iptables -C INPUT -p tcp --dport 443 -m conntrack --ctstate NEW -j ACCEPT 2>/dev/null \
     || sudo iptables -I INPUT -p tcp --dport 443 -m conntrack --ctstate NEW -j ACCEPT
   sudo netfilter-persistent save          # Ubuntu / Debian image
   # Oracle Linux with firewalld instead:
   # sudo firewall-cmd --permanent --add-port=443/tcp && sudo firewall-cmd --reload
   ```

4. **Verify from a machine that is not the VM** (before Caddy is even up, use the
   port test; after §2, the curl):

   ```bash
   nc -vz <IP> 443
   curl -v https://<IP>/            # after §2: expect a TLS handshake, then 404
   ```

---

## 2. Generate the pinned self-signed certificate and configure Caddy (no ACME)

The device pins the exact server certificate, so the cert must be generated once,
kept stable, and placed where Caddy can read it. Generate an **ECDSA P-256**
certificate with `<IP>` in the SAN (DESIGN §8.1 pins this single certificate as
the only trust anchor; the rest of §8 — SNTP, verification, no `setInsecure()` —
still applies).

1. Create the directories and a private key:

   ```bash
   sudo mkdir -p /etc/caddy /var/log/caddy
   cd /etc/caddy
   sudo openssl ecparam -name prime256v1 -genkey -noout -out ota.key
   sudo chmod 600 ota.key
   ```

2. Self-sign a certificate valid for the IP literal. The `IP:<IP>` SAN is what
   the device matches; the `DNS:<IP>` alias is a belt-and-braces entry so mbedtls
   versions that only string-match CN/dNSName still accept the IP host:

   ```bash
   sudo openssl req -new -x509 -key ota.key -out ota.crt -days 3650 \
     -subj "/CN=<IP>" \
     -addext "subjectAltName=IP:<IP>,DNS:<IP>" \
     -addext "basicConstraints=critical,CA:TRUE" \
     -addext "keyUsage=critical,digitalSignature,keyCertSign" \
     -addext "extendedKeyUsage=serverAuth"
   ```

   `CA:TRUE` is what lets the firmware load this same file with
   `setCACert()` as its own trust anchor. Keep both files; you will copy `ota.crt`
   into the firmware pin (§6).

3. Make the files readable by the unprivileged Caddy service while still
   root-owned. Caddy needs the **key**, so grant group read:

   ```bash
   sudo chown root:caddy /etc/caddy/ota.crt /etc/caddy/ota.key
   sudo chmod 644 /etc/caddy/ota.crt
   sudo chmod 640 /etc/caddy/ota.key
   ```

4. Write `/etc/caddy/Caddyfile` adapted from
   [`Caddyfile.example`](Caddyfile.example). That file already uses a bare-IP
   site and an explicit self-signed `tls` pair; drop the ACME/hostname
   assumptions. The optional global `auto_https off` makes "no ACME" explicit:

   ```caddyfile
   {
       auto_https off
   }

   <IP> {
       tls /etc/caddy/ota.crt /etc/caddy/ota.key

       encode zstd gzip
       root * /srv

       header {
           # No HSTS: browsers ignore it for a bare-IP host and there is no domain.
           X-Content-Type-Options "nosniff"
           Referrer-Policy "no-referrer"
           -Server
       }

       @manifest path */manifest.json
       header @manifest Cache-Control "no-cache"

       @bin path *.bin
       header @bin Content-Type "application/octet-stream"
       header @bin Cache-Control "public, max-age=31536000, immutable"

       handle /firmware/manifest.json {
           rewrite * /firmware/waveshare_amoled_216/manifest.json
           file_server
       }

       handle {
           file_server
       }

       log {
           output file /var/log/caddy/ota-access.log
           format json
       }
   }
   ```

   The explicit `tls` directive plus `auto_https off` is the "no ACME" part;
   do not use a hostname here.

5. Validate, then start/reload Caddy:

   ```bash
   sudo caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile
   sudo systemctl enable --now caddy
   sudo systemctl reload caddy      # after later Caddyfile edits
   ```

6. Confirm the exact certificate the device will pin, and that it is being
   served. `openssl s_client` should show the `IP:<IP>` SAN and the P-256 curve:

   ```bash
   openssl x509 -in /etc/caddy/ota.crt -noout -subject -ext subjectAltName -text | grep -E 'Subject:|IP Address|Public Key Algorithm'
   echo | openssl s_client -connect <IP>:443 -servername <IP> 2>/dev/null \
     | openssl x509 -noout -fingerprint -sha256
   ```

---

## 3. Create the firmware directory layout and permissions

The publisher (`ota-publish.sh`, DESIGN §6.5) writes per-board directories under
`/srv/firmware`; Caddy serves `/srv` as the document root (DESIGN §6.4).

1. Create the tree and give it to the publish user:

   ```bash
   sudo mkdir -p /srv/firmware/waveshare_amoled_216
   sudo chown -R <user>:<user> /srv/firmware
   sudo chmod 755 /srv /srv/firmware /srv/firmware/waveshare_amoled_216
   ```

2. Ensure Caddy (a different, unprivileged user) can traverse and read every
   file the publisher writes. New files come out `0644`; enforce it after a
   manual copy if you ever create files by hand:

   ```bash
   sudo find /srv/firmware -type d -exec chmod 755 {} \;
   sudo find /srv/firmware -type f -exec chmod 644 {} \;
   ```

3. Optional hardening of the publish login. If you create a dedicated key for
   CI, restrict it with `restrict` and no port forwarding. `rsync` over `ssh`
   needs a shell, so do **not** use a forced `command=`:

   ```
   # /home/<user>/.ssh/authorized_keys
   restrict,no-agent-forwarding,no-port-forwarding,no-pty ssh-ed25519 AAAA... github-actions-ota-publish
   ```

   `ota-publish.sh` will `mkdir -p` `<dest>/<board>/.tmp`, `rsync` into it, then
   `mv` in place, so `<user>` must own the whole `/srv/firmware` subtree.

---

## 4. Configure the GitHub repo secrets for the publish workflow

`.github/workflows/ota-publish.yml` stages an SSH key, a pinned `known_hosts`
file, and the rsync target, then runs `ota-publish.sh` (IMPL §5.2). Never put
secrets in the repo or the command line.

1. Generate a dedicated deploy key **on the VM as `<user>`** and authorize it:

   ```bash
   ssh-keygen -t ed25519 -f ~/.ssh/ota_deploy -N "" -C "github-actions ota-publish"
   cat ~/.ssh/ota_deploy.pub >> ~/.ssh/authorized_keys
   chmod 600 ~/.ssh/authorized_keys
   ```

2. Test the key can reach the firmware directory:

   ```bash
   ssh -i ~/.ssh/ota_deploy <user>@<IP> 'ls -ld /srv/firmware'
   ```

3. Create the three repository secrets. Copy the **private** key text to
   `OTA_SSH_KEY`; pin the host key with `ssh-keyscan -H <IP>`; set the target
   exactly as `<user>@<IP>:/srv/firmware`:

   ```bash
   # from a machine with the GitHub CLI authenticated for this repo
   gh secret set OTA_SSH_KEY       < ~/.ssh/ota_deploy
   gh secret set OTA_KNOWN_HOSTS   < <(ssh-keyscan -H <IP> 2>/dev/null)
   gh secret set OTA_DEST          --body '<user>@<IP>:/srv/firmware'
   ```

   Or add them in *Settings → Secrets and variables → Actions → New repository
   secret* with the same names and values. `OTA_DEST` is parsed by
   `ota-publish.sh` as `user@host:/path`; the workflow also strips the user to
   print the manifest URL.

4. Sanity-check the workflow's assumptions: `ssh-keyscan -H <IP>` must not have
   been pre-seeded with an old host key, and the deploy key must not be
   passphrase-protected (CI cannot type a passphrase).

---

## 5. Build and publish a release, then verify over the public IP

Version comes from a `vX.Y.Z` tag stamped at build time (DESIGN §5.2); the
manifest version must equal the image's `FW_VERSION` or the workflow aborts
(IMPL §5.2 step 3).

Both paths must build with the reserved IP **and** the pinned cert baked in
(DESIGN §14.2). The publisher only ships the `.bin` it is given, so make sure the
binary it receives came from a build with
`-DOTA_PULL_MANIFEST_URL="https://<IP>/firmware"`; otherwise the image falls back
to `https://ota.invalid/firmware` and every device check fails. Before tagging,
confirm the CI build step injects the host (e.g. a repository variable/secret fed
into `PLATFORMIO_BUILD_FLAGS`) and that `firmware/certs/pinned_server.pem` is
present in the build workspace (CI has no untracked PEM unless you inject it).

1. Tag and push to trigger the publish (P2 path):

   ```bash
   cd <repo-path>
   git checkout main && git pull
   git tag -a v0.2.0 -m "Release 0.2.0"
   git push origin v0.2.0
   ```

2. Or build and publish manually (P0 path), which is the way to inject the pin
   and the IP for sure:

   ```bash
   cd <repo-path>
   PLATFORMIO_BUILD_FLAGS='-DOTA_PULL_MANIFEST_URL="https://<IP>/firmware"' \
     pio run -d firmware -e waveshare_amoled_216
   OTA_DEST='<user>@<IP>:/srv/firmware' OTA_KEEP=5 \
   design/ota-pull/deploy/ota-publish.sh \
     --bin firmware/.pio/build/waveshare_amoled_216/firmware.bin \
     --version 0.2.0 \
     --board waveshare_amoled_216 \
     --min-from 0.1.0 \
     --notes "First pull-OTA release"
   ```

   The script refuses a dirty git tree unless you pass `--allow-dirty`, writes
   the binary then the manifest atomically, symlinks the flat alias, and keeps
   the last `--keep` versions (DESIGN §6.5).

3. Verify the manifest is live and correct over the reserved IP. From the VM,
   trust the cert directly; from anywhere else use `--cacert` (preferred) or
   `-k` for a quick look:

   ```bash
   # on the VM
   curl -fsS https://<IP>/firmware/waveshare_amoled_216/manifest.json \
     --cacert /etc/caddy/ota.crt | python3 -m json.tool

   # on any other machine (the CA is not in the system store)
   curl -fsS --cacert ./ota.crt https://<IP>/firmware/waveshare_amoled_216/manifest.json \
     | python3 -m json.tool
   ```

4. Download the binary and prove the manifest's `sha256` and `size` match
   (DESIGN §6.2; this is exactly what the device checks):

   ```bash
   VER="$(curl -fsS --cacert ./ota.crt https://<IP>/firmware/waveshare_amoled_216/manifest.json | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')"
   curl -fsS --cacert ./ota.crt -o /tmp/clawdmeter-$VER.bin \
     https://<IP>/firmware/waveshare_amoled_216/clawdmeter-$VER.bin
   sha256sum /tmp/clawdmeter-$VER.bin
   stat -c '%s' /tmp/clawdmeter-$VER.bin
   ```

5. Check the header contract the device relies on (DESIGN §6.3): manifest is
   `Cache-Control: no-cache` with an `ETag`; binary is `immutable`. Also confirm
   the flat alias resolves:

   ```bash
   curl -fsSI --cacert ./ota.crt https://<IP>/firmware/waveshare_amoled_216/manifest.json
   curl -fsSI --cacert ./ota.crt https://<IP>/firmware/waveshare_amoled_216/clawdmeter-0.2.0.bin
   curl -fsS  --cacert ./ota.crt https://<IP>/firmware/manifest.json | python3 -m json.tool
   ```

   A `304 Not Modified` on a repeated request with `If-None-Match: "<etag>"` is
   expected and is how the device short-circuits a check.

---

## 6. Flash the device with the pin and manifest URL, then provision WiFi over BLE

The device only trusts the cert it was built with, and it only knows the VM
address from the build macro (DESIGN §8.1, §14.2). Both are injected before the
image is built.

1. Drop the served certificate into the untracked pin path consumed by
   `firmware/scripts/gen_pinned_cert.py`:

   ```bash
   mkdir -p <repo-path>/firmware/certs
   cp /etc/caddy/ota.crt <repo-path>/firmware/certs/pinned_server.pem
   git -C <repo-path> status --short   # must NOT list pinned_server.pem / _pem.h
   ```

   `firmware/certs/pinned_server.pem` and the generated
   `firmware/src/certs/pinned_server_pem.h` are already in `.gitignore` (confirm
   with `git check-ignore firmware/certs/pinned_server.pem`).

   `gen_pinned_cert.py` renders that PEM into
   `firmware/src/certs/pinned_server_pem.h` as `PINNED_SERVER_PEM`, which
   `ota_pull.cpp` passes to `WiFiClientSecure::setCACert()`. It is wired as a
   PlatformIO pre-build extra script
   (`extra_scripts = pre:scripts/version.py, pre:scripts/gen_pinned_cert.py`),
   so the normal build regenerates the header; it also runs standalone:

   ```bash
   python3 <repo-path>/firmware/scripts/gen_pinned_cert.py
   ```

   If the PEM is missing the script emits a **fail-closed** placeholder (the
   build succeeds, but every handshake is rejected until the real cert is
   installed), so a missing pin never silently disables verification.

2. Build with the reserved IP in the compile-time base URL, at the `v0.2.0` tag
   so `FW_VERSION` matches the published manifest:

   ```bash
   cd <repo-path>
   git checkout v0.2.0 2>/dev/null || true
   PLATFORMIO_BUILD_FLAGS='-DOTA_PULL_MANIFEST_URL="https://<IP>/firmware"' \
     pio run -d firmware -e waveshare_amoled_216
   ```

   `OTA_PULL_MANIFEST_URL` is the base only; the device appends
   `/<board>/manifest.json` (DESIGN §6.4). Confirm the pin and host actually
   landed in the image (the literal IP should appear; the baked cert adds the
   `BEGIN CERTIFICATE` text):

   ```bash
   strings firmware/.pio/build/waveshare_amoled_216/firmware.bin | grep -c 'https://<IP>/firmware'
   ```

3. Flash over USB:

   ```bash
   pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/ttyACM0
   ```

4. Provision WiFi over the existing BLE path (DESIGN §2 rule 3; NVS namespace
   `otah`, §14.1). The device must be bonded and reachable, and only one host may
   hold the single BLE link. The simplest path reuses the working helper — it
   provisions credentials and (re)flashes the same image via the hybrid
   `espota` path in one go:

   ```bash
   python daemon/ota_flash.py \
     --firmware firmware/.pio/build/waveshare_amoled_216/firmware.bin \
     --ssid "<SSID>" --pass "<WiFi password>"
   ```

   To **provision only** without the hybrid upload, send the frozen `wifi`
   command straight to CTRL (the same frame the helper sends):

   ```python
   # provision_wifi.py — run with the daemon stopped, device bonded
   import asyncio, json
   from bleak import BleakClient
   TX   = "4c41555a-4465-7669-6365-000000000003"   # notify
   CTRL = "4c41555a-4465-7669-6365-000000000005"   # write (encrypted)
   async def main(addr, ssid, pw):
       async with BleakClient(addr) as c:
           await c.start_notify(TX, lambda _, d: print(d.decode()))
           await c.write_gatt_char(CTRL,
               json.dumps({"cmd": "wifi", "ssid": ssid, "pass": pw}).encode())
           await asyncio.sleep(2)
   asyncio.run(main("<device-BLE-address>", "<SSID>", "<WiFi password>"))
   ```

   Expected TX reply: `{"ok":true,"cmd":"wifi"}`; a failure is
   `{"ok":false,"err":"..."}`. The daemon config keys
   `ota_ssid`/`ota_wifi_password` are an alternative source the helper reads.

5. Confirm identity and force the first check without waiting for the 24 h tick
   (DESIGN §9.1). The reply must show the pinned version and the new fields:

   ```json
   {"cmd":"info"}
   -> {"ok":true,"board":"waveshare_amoled_216","fw":"0.2.0","sha":"abcdef0",
       "build":"2026-10-02T12:00:00Z","id":"AA:BB:CC:DD:EE:FF"}
   {"cmd":"update","mode":"check"}
   -> {"ok":true,"cmd":"update","state":"checking"}
      then {"state":"up_to_date","version":"0.2.0"} or {"state":"available",...}
   ```

   Auto-check runs 60 s after boot (after `HEALTHY_MS`) and every 24 h while
   credentials are stored (DESIGN §10.1).

---

## 7. Hardware smoke-test matrix

Run these on the bench with USB serial attached (115200 baud) so you can read
the `OTA: pull ...` log lines; the screen indicator is the transient status line
from DESIGN §12. Each row is self-contained; re-flash or republish between rows.

| Case | Setup | Expect on screen | Expect on serial / BLE |
|---|---|---|---|
| **happy path** | Publish a valid manifest newer than the running `fw`; keep the device charged. | `Update 0.2.0` → `Updating… <pct>%` → `Verifying…` → `Restarting…`; after reboot the version/UI is the new one. | `available` → progress → `rebooting`; download hash matches; `boot_tries` resets after 60 s healthy. |
| **no update** | Manifest `version` == running version. | No indicator, or a brief `Checking…` only. | `no_update` / `up_to_date`; radio goes off; no flash, `otadata` unchanged. |
| **bad hash** | Flip a byte in the served `.bin` but leave the manifest `sha256` (and `size`) untouched. | `Update failed` for ~3 s. | `hash_mismatch`; `esp_ota_abort()`; **no reboot**; old slot still boots. |
| **bad board** | Change manifest `board` to e.g. `other_board`. | `Update failed` (no download). | `board_mismatch`; reject before `DOWNLOAD` (DESIGN §11). |
| **VM down** | `sudo systemctl stop caddy`, or temporarily block 443 from the device's network. | `Checking…` then `Update failed` (transient). | Retry/backoff per DESIGN §7.3 (`2/4/8 s` for the manifest, up to 5 check attempts); radio off between tries; no flash. |
| **bad image** | Truncate the `.bin` to a size that is not a valid app image, then recompute `sha256` and `size` so the hash passes. | `Verifying…` → `Update failed`. | `bad_image` from `esp_ota_end()`; never activates; old slot still boots. |
| **crash-loop rollback** | Publish a valid image whose hash matches but that panics shortly after boot (test build). | New version flashes and boots, then the device returns to the old version after the retries. | `boot_tries` reaches 3 → `rollback_and_restart()`; `fw` is the last-known-good; serial shows the rollback (DESIGN §7.2, §11). |
| **battery gate** | Discharge below 20 % and leave USB unplugged. | No update indicator; nothing flashes. | Auto check skipped/deferred (`battery_low` if reported); BLE `{"cmd":"update","mode":"check"}` may still report `available`. Plug in / charge ≥ 50 % (or `mandatory`/owner `force`) to pass the apply gate (DESIGN §10.2). |
| **hybrid busy** | Start hybrid `{"cmd":"ota","mode":"on"}` and immediately trigger a pull (or the reverse). | No visual contention; whichever loses shows busy in the log. | Second caller gets `{"ok":false,"err":"busy"}`; only one WiFi owner ever holds the radio (DESIGN §7.4). |
| **cert failure** | Serve a **different** self-signed cert while the firmware still pins the original (point Caddy's `tls` at another cert/key). | `Update failed`. | `tls_fail`; handshake aborts before any HTTP GET; no download; verify with `openssl s_client` that the served cert is the pinned one. |
| **SNTP failure** | Block outbound UDP/123 on the device's network (AP/router), or otherwise make NTP unreachable. | `Checking…` then `Update failed`. | `timeout` / `SNTP_FAIL` **before** TLS (DESIGN §8.2); no download. Restore NTP and recheck. |

Record the pass/fail and the logged `err` for each row; the acceptance criteria
are the P1 hardware items in IMPL §4.3 with the matrix in IMPL §4.4.

---

## 8. Rollback, certificate rotation, and lost/revoked devices

### 8.1 Rolling back a bad release

1. **Automatic rollback** already covers a crash-looping image: after 3
   unconfirmed boots the existing `boot_tries` path returns `otadata` to the
   last-known-good slot and reboots (DESIGN §7.2, §11). No operator action is
   needed; confirm the old `fw` in `{"cmd":"info"}`.

2. **Owner-forced downgrade** for a bad-but-booting release. The manifest for
   the target version must still be served (the publisher keeps the last
   `--keep` versions, default 5), and the command is only accepted on a
   bonded+encrypted owner link (DESIGN §5.4, §9.1):

   ```json
   {"cmd":"update","force":true,"to":"0.1.0"}
   ```

   `to` must equal the manifest's `version` or the reply is
   `{"err":"target_mismatch"}`. The SHA-256 check is still enforced.

3. **USB reflash is always the final fallback** and works with no network
   (DESIGN §11): rebuild at the good tag with the pin + `OTA_PULL_MANIFEST_URL`
   and run `-t upload` as in §6.

4. To stop automatic updates entirely for one device, clear its WiFi credentials
   with `{"cmd":"wifi_clear"}` (it will then answer `no_wifi` and never check),
   or publish nothing new for that board.

### 8.2 Rotating the pinned self-signed certificate

Because the firmware pins the certificate itself, a new cert cannot be trusted
until the firmware that carries the new pin is installed — and that firmware
must first be fetched under the **old** pin. Plan the rotation as a two-phase
rollout (the same chicken-and-egg logic as DESIGN §8.4 for the pinned leaf):

1. **Generate the new cert/key early** (`ota-v2.crt` / `ota-v2.key` per §2),
   but **keep serving the old cert** so existing devices can still update.
   Keep the old cert valid and undeleted for the whole transition; do not let it
   expire.

2. **Ship a firmware release that pins both certs.** Concatenate old + new into
   the pin file and build/publish a new version:

   ```bash
   cat /etc/caddy/ota.crt /etc/caddy/ota-v2.crt \
     > <repo-path>/firmware/certs/pinned_server.pem
   # build with OTA_PULL_MANIFEST_URL as in §6, tag, publish (§5)
   ```

   This release reaches devices over the **old** cert. `gen_pinned_cert.py`
   embeds `pinned_server.pem` verbatim, so a concatenated two-cert PEM is a valid
   trust bundle and the device accepts either server certificate. Wait until every
   device you can reach has confirmed the new version (`{"cmd":"info"}`), tracked
   operationally.

3. **Switch the server to the new cert** only after the fleet has the dual-pin
   build. Edit `/etc/caddy/Caddyfile` to point `tls` at `ota-v2.crt` /
   `ota-v2.key`, then reload:

   ```bash
   sudo caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile
   sudo systemctl reload caddy
   echo | openssl s_client -connect <IP>:443 -servername <IP> 2>/dev/null \
     | openssl x509 -noout -fingerprint -sha256
   ```

   Devices on the dual-pin build keep working; devices that never updated (e.g.
   offline) will now fail TLS and must be USB-reflashed.

4. A later release may drop the old cert from the pin. Until then, leave it in
   the bundle. Devices that never received a certificate-carrying update are the
   residual risk and must be handled physically.

   *Alternative to avoid this churn:* pin a long-lived self-signed **CA**
   (`CA:TRUE`) and serve a short-lived leaf signed by it, so leaf rotation never
   requires a firmware change. If you adopt that, the "pin" is the CA, not the
   served leaf, and step 2/3 become a normal leaf reissue.

5. **Rotate the publish deploy key** at the same time if compromise is
   suspected: generate a new keypair on the VM, replace the `authorized_keys`
   entry, update the `OTA_SSH_KEY` secret, and remove the old key. Rotating
   `OTA_KNOWN_HOSTS` is only needed if the VM host key changes.

### 8.3 Lost or compromised device

Pull OTA is intentionally unauthenticated per device (DESIGN §1.2, §13.2): there
is no token to revoke and the VM keeps no per-device state for OTA. Therefore:

1. **You cannot revoke a single device at the VM.** The server will keep serving
   firmware to anyone who can reach it.
2. **Cut the device off at the network.** Clear/rotate the WiFi credentials
   (`{"cmd":"wifi_clear"}` then reprovision) or change the WiFi password so the
   lost unit can no longer reach the VM. After a cert rotation (§8.2) an offline
   device also loses TLS.
3. **Stop publishing sensitive firmware** if the next release must not reach the
   lost unit; publish only to a board directory it cannot guess, or take Caddy
   down. Remember the firmware is public-by-obscurity (DESIGN §1.2).
4. **Assume flash is readable.** No Secure Boot or flash encryption in this
   phase (DESIGN §13.2); a physical attacker can read the image and its baked
   pin. Treat a lost device as disclosed. Signed images / Secure Boot are the
   P4 future path (DESIGN §13.3).

---

## 9. Troubleshooting

Check Caddy's log (`/var/log/caddy/ota-access.log`) and the device's USB serial
log first; the `err` token below is the frozen value from DESIGN §9.2.

| Symptom / `err` | Likely cause | Fix |
|---|---|---|
| **401 / 403** from the server | Caddy cannot read the file (wrong owner/perms, non-traversable directory), or a reverse proxy / WAF in front is imposing auth. | Re-apply §3 perms (`find … -chmod 755/644`, `<user>` owns the tree); confirm no auth directive sits in front of `file_server`; check the access log for the exact path. |
| **404** `http_404` | Manifest/binary not published, wrong board directory, or Caddy `root` not `/srv` so the URL→disk mapping is wrong. | Verify `ls -l /srv/firmware/waveshare_amoled_216/manifest.json`; confirm `root * /srv`; confirm the published board id matches `board_caps().id`; test `curl` as in §5. |
| **TLS / handshake** `tls_fail` | Device clock not synced; served cert is not the pinned one; SAN missing `<IP>`; pin file stale after a cert change; cert expired. | Fix SNTP first (next row); compare fingerprints per §2.6/§8.2; regenerate the cert with `IP:<IP>` and reflash the pin; verify `PLATFORMIO_BUILD_FLAGS` injected the new `OTA_PULL_MANIFEST_URL`. |
| **SNTP** `timeout` before any HTTP | Outbound NTP (UDP/123) blocked; DNS to `pool.ntp.org`/`time.cloudflare.com` fails; no internet from the device's WiFi. | Allow UDP/123 outbound; test with a phone hotspot; the device aborts before TLS by design (DESIGN §8.2), so nothing is downloaded until the clock is sane. |
| **battery_low** | Below the 20 % check floor, or below 50 % and not charging for apply. | Charge / plug in USB (treated as charging); or publish `--mandatory`, or use the owner `force` command (DESIGN §5.4, §10.2). Mandatory bypasses only the gate, never the hash/board/version checks. |
| **busy** | Hybrid OTA and pull both want the single WiFi owner (or two pulls). | Wait for the in-flight operation; the loser is rejected, not queued (DESIGN §7.4). Retry the BLE command. |
| **timeout** during join / fetch / download | WiFi join > 20 s or bad signal; manifest GET > 15 s; download stalled > 15 s idle or > 300 s total; server unreachable. | Check RSSI/AP, the Security List + iptables in BOTH layers (§1), Caddy is running, and the VM is not resource-starved; re-run `{"cmd":"update","mode":"check"}`. Backoff is automatic (DESIGN §7.3). |
| Wrong device receives an update | Board directory collision, or a manifest copied into the wrong path. | Never flatten across boards; keep the per-board canonical path and let Caddy's alias rewrite handle `/firmware/manifest.json` (DESIGN §6.4). The device refuses `board_mismatch` regardless. |
| Update flashes but reverts | New image fails to confirm within 60 s. | Expected crash-loop protection: fix the image, republish a higher version; the old slot is preserved (DESIGN §7.2). |

### Quick health snapshot

```bash
# VM side
systemctl is-active caddy && curl -fsSI --cacert /etc/caddy/ota.crt https://<IP>/firmware/waveshare_amoled_216/manifest.json
sudo iptables -S INPUT | grep 443
# Device side (serial): board, fw, sha, build, then the last OTA err
# via BLE: {"cmd":"info"} and {"cmd":"update","mode":"check"}
```
