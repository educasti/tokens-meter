#!/usr/bin/env bash
#
# Install / update the tokens-meter backend as a *system* systemd service.
#
#   sudo backend/deploy/install.sh
#
# Idempotent: safe to re-run. It copies backend.service to
# /etc/systemd/system/backend.service, reloads the manager, enables the unit to
# start on boot and starts it now, then prints its status. If the unit content
# changed while the service was already running, the service is restarted so the
# new definition takes effect.
#
# Must be run as root (the system manager owns /etc/systemd/system and the unit
# runs as the unprivileged `educasti` user).
set -euo pipefail

SERVICE="backend"
UNIT_NAME="${SERVICE}.service"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
UNIT_SRC="${SCRIPT_DIR}/${UNIT_NAME}"
UNIT_DST="/etc/systemd/system/${UNIT_NAME}"
DATA_DIR="${BACKEND_DIR}/data"
SERVICE_USER="educasti"
DEFAULT_BIND="127.0.0.1:8080"

info() { printf '\033[1m==>\033[0m %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# --------------------------------------------------------------------- checks
[[ "${EUID}" -eq 0 ]] || die "must run as root: sudo $0"
[[ -f "${UNIT_SRC}" ]] || die "unit file not found: ${UNIT_SRC}"
command -v systemctl >/dev/null 2>&1 || die "systemctl not found (is this a systemd host?)"

# ------------------------------------------------ data dir + stray processes
# The unit's ReadWritePaths= carves out the data dir; make sure it exists and is
# owned by the service user so the database can be created there.
install -d -m 0750 -o "${SERVICE_USER}" -g "${SERVICE_USER}" "${DATA_DIR}"

# Stop any manually-started backend.py (e.g. an old `setsid nohup` process) that
# would keep the port busy and make the unit fail to bind. Processes already
# managed by the unit live in its cgroup and are left untouched.
stray_pids=()
while read -r pid; do
    [[ -n "${pid}" ]] || continue
    cgroup="$(cat "/proc/${pid}/cgroup" 2>/dev/null || true)"
    [[ "${cgroup}" == *"/${UNIT_NAME}"* ]] && continue
    info "stopping manually-started backend process (PID ${pid})"
    stray_pids+=("${pid}")
    kill "${pid}" 2>/dev/null || true
done < <(pgrep -f 'python3 .*backend\.py' 2>/dev/null || true)

# Wait for those PIDs to actually exit so the listening socket is released.
for pid in ${stray_pids[@]+"${stray_pids[@]}"}; do
    for _ in $(seq 1 50); do
        kill -0 "${pid}" 2>/dev/null || break
        sleep 0.1
    done
done

# ------------------------------------------------------------ install + start
changed=0
if [[ ! -f "${UNIT_DST}" ]] || ! cmp -s "${UNIT_SRC}" "${UNIT_DST}"; then
    changed=1
fi
was_active=0
systemctl is-active --quiet "${SERVICE}" && was_active=1 || true

install -m 0644 "${UNIT_SRC}" "${UNIT_DST}"
info "installed ${UNIT_DST}"

systemctl daemon-reload

rc=0
if ! systemctl enable --now "${SERVICE}"; then
    rc=1
    info "warning: 'systemctl enable --now ${SERVICE}' failed; see status below"
fi

# `enable --now` does not restart an already-active unit, so apply an updated
# unit definition explicitly.
if [[ "${rc}" -eq 0 && "${changed}" -eq 1 && "${was_active}" -eq 1 ]]; then
    info "unit changed; restarting ${SERVICE} to apply it"
    systemctl restart "${SERVICE}" || rc=1
fi

# --------------------------------------------------------------- report status
echo
systemctl --no-pager --full status "${SERVICE}" || true
echo

# Best-effort health probe against the configured bind address.
bind="${DEFAULT_BIND}"
if [[ -f "${BACKEND_DIR}/config.json" ]] && command -v python3 >/dev/null 2>&1; then
    bind="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("bind") or sys.argv[2])' \
        "${BACKEND_DIR}/config.json" "${DEFAULT_BIND}" 2>/dev/null || echo "${DEFAULT_BIND}")"
fi
health_url="http://${bind}/api/health"
if command -v curl >/dev/null 2>&1; then
    if curl -fsS --max-time 5 "${health_url}" >/dev/null 2>&1; then
        info "health OK: ${health_url}"
    else
        info "health not reachable yet: ${health_url}"
    fi
fi

if [[ "${rc}" -eq 0 ]]; then
    info "${SERVICE} is enabled and running (starts on boot)"
else
    info "${SERVICE} did not start cleanly; check: journalctl -u ${SERVICE} -e"
fi
exit "${rc}"
