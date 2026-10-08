#!/bin/bash
# Instalador de macOS para el daemon de Clawdmeter (Python + bleak + launchd).
# Sigue el mismo flujo que install.sh, pero usa LaunchAgents en lugar de
# unidades de usuario de systemd.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_LABEL="com.user.claude-usage-daemon"
PLIST_SRC="$SCRIPT_DIR/daemon/$SERVICE_LABEL.plist"
PLIST_DST="$HOME/Library/LaunchAgents/$SERVICE_LABEL.plist"
VENV_DIR="$SCRIPT_DIR/daemon/.venv"
DAEMON_PY="$SCRIPT_DIR/daemon/claude_usage_daemon.py"
LOG_DIR="$HOME/Library/Logs"
LOG_OUT="$LOG_DIR/claude-usage-daemon.out.log"
LOG_ERR="$LOG_DIR/claude-usage-daemon.err.log"
CONFIG_FILE="$HOME/.config/claude-usage-monitor/config"

# Vuelve a convertir una ruta absoluta bajo $HOME en forma ~ para que las
# entradas del archivo de configuración queden limpias.
_tilde() { case "$1" in "$HOME"/*) echo "~${1#"$HOME"}";; *) echo "$1";; esac; }

# Muestra el valor actual de una clave de configuración (recortado), o vacío si
# no está definida.
current_config_value() {
    [ -f "$CONFIG_FILE" ] || return 0
    grep -E "^[[:space:]]*$1[[:space:]]*=" "$CONFIG_FILE" | tail -1 \
        | tr -d '\r' \
        | sed -E "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//; s/[[:space:]]*(#.*)?$//"
}

# Inserta o reemplaza `key = value`, conservando todas las demás claves del
# archivo.
upsert_config_key() {
    local key="$1" value="$2"
    mkdir -p "$(dirname "$CONFIG_FILE")"
    touch "$CONFIG_FILE"
    grep -vE "^[[:space:]]*$key[[:space:]]*=" "$CONFIG_FILE" > "$CONFIG_FILE.tmp" 2>/dev/null || true
    mv "$CONFIG_FILE.tmp" "$CONFIG_FILE"
    echo "$key = $value" >> "$CONFIG_FILE"
}

# Detecta los directorios de configuración ~/.claude* y, si encuentra más de uno,
# deja que el usuario elija qué planes mostrar. El daemon consulta todos los
# directorios elegidos y muestra el que esté activo. Nota de macOS: el ~/.claude
# predeterminado guarda su token en el Keychain (a menudo sin archivo
# .credentials.json), así que siempre cuenta como candidato; los directorios
# adicionales se reconocen por su archivo de credenciales — igual que hace
# read_token_for en el daemon.
configure_config_dirs() {
    local -a candidates=()
    local d
    for d in "$HOME"/.claude*; do
        [ -d "$d" ] || continue
        if [ -f "$d/.credentials.json" ] || [ "$d" = "$HOME/.claude" ]; then
            candidates+=("$d")
        fi
    done

    if [ ${#candidates[@]} -le 1 ]; then
        echo "  Se encontró un directorio de configuración de Claude — se usará el predeterminado (~/.claude)."
        return 0
    fi

    echo "  Se encontraron varios directorios de configuración de Claude. El daemon puede"
    echo "  consultar varios planes y mostrar el que estés usando en cada momento."
    if [ ! -t 0 ]; then
        local list=""
        for d in "${candidates[@]}"; do list="${list:+$list, }$(_tilde "$d")"; done
        echo "  Shell no interactiva — se omite. Para activarlo, añade a $CONFIG_FILE:"
        echo "    config_dirs = $list"
        return 0
    fi

    local -a selected=()
    local ans
    for d in "${candidates[@]}"; do
        if [ "$d" = "$HOME/.claude" ]; then
            read -r -p "  ¿Consultar $(_tilde "$d")? [Y/n] " ans || ans=""
            if [[ ! "$ans" =~ ^[Nn]$ ]]; then selected+=("$d"); fi
        else
            read -r -p "  ¿Consultar también $(_tilde "$d")? [y/N] " ans || ans=""
            if [[ "$ans" =~ ^[Yy]$ ]]; then selected+=("$d"); fi
        fi
    done

    if [ ${#selected[@]} -eq 0 ]; then
        echo "  No se seleccionó nada — se mantiene el predeterminado (~/.claude)."
        return 0
    fi
    if [ ${#selected[@]} -eq 1 ] && [ "${selected[0]}" = "$HOME/.claude" ]; then
        echo "  Solo el predeterminado (~/.claude) — no hace falta cambiar la configuración."
        return 0
    fi

    local joined="" sd
    for sd in "${selected[@]}"; do joined="${joined:+$joined, }$(_tilde "$sd")"; done

    upsert_config_key config_dirs "$joined"
    echo "  Escrito: config_dirs = $joined"
    echo "  -> $CONFIG_FILE"
}

# Ofrece la visualización opcional del reloj (se muestra en lugar del título
# "Consumo"). Solo escribe la clave cuando cambia realmente el valor actual o el
# predeterminado.
configure_clock() {
    [ -t 0 ] || return 0
    local ans cur
    cur=$(current_config_value clock)
    read -r -p "  ¿Mostrar un reloj en lugar del título \"Consumo\"? [off/auto/12/24] (por omisión: off) " ans || ans=""
    ans=$(echo "$ans" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')
    [ -z "$ans" ] && ans="off"
    case "$ans" in
        off|auto|12|24) ;;
        *) echo "  Opción no reconocida '$ans' — el reloj queda sin cambios."; return 0 ;;
    esac
    if [ "$ans" = "off" ] && { [ -z "$cur" ] || [ "$cur" = "off" ]; }; then
        echo "  Reloj desactivado (predeterminado)."
        return 0
    fi
    upsert_config_key clock "$ans"
    echo "  Ajustado: clock = $ans"
}

# Ofrece el aviso sonoro opcional al renovarse la sesión (suena por el altavoz de
# la placa).
configure_chime() {
    [ -t 0 ] || return 0
    local ans cur
    cur=$(current_config_value chime)
    read -r -p "  ¿Avisar por el altavoz cuando se renueve tu límite de sesión de 5 h? [y/N] " ans || ans=""
    if [[ "$ans" =~ ^[Yy]$ ]]; then
        upsert_config_key chime on
        echo "  Ajustado: chime = on"
    elif [ "$cur" = "on" ]; then
        upsert_config_key chime off
        echo "  Ajustado: chime = off"
    else
        echo "  Aviso sonoro desactivado (predeterminado)."
    fi
}

echo "=== Instalación de Clawdmeter en macOS ==="
echo ""

echo "[1/6] Comprobando los requisitos..."
command -v curl >/dev/null || { echo "Error: curl es necesario"; exit 1; }

# El daemon usa sintaxis de Python 3.10+ (PEP 604, `X | None`). macOS trae un
# python3 del sistema antiguo (3.9), así que se prefiere un intérprete más
# nuevo — el de Homebrew si está presente — y en su defecto cualquiera del PATH
# que sea >= 3.10.
py_ge_310() { "$1" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' >/dev/null 2>&1; }
PYTHON3=""
for cand in \
    "$(command -v python3.13)" "$(command -v python3.12)" \
    "$(command -v python3.11)" "$(command -v python3.10)" \
    /opt/homebrew/bin/python3 /usr/local/bin/python3 \
    "$(command -v python3)"; do
    [ -n "$cand" ] && [ -x "$cand" ] || continue
    if py_ge_310 "$cand"; then PYTHON3="$cand"; break; fi
done
if [ -z "$PYTHON3" ]; then
    echo "Error: se necesita Python >= 3.10. Instálalo con: brew install python"
    exit 1
fi
echo "  Usando $($PYTHON3 --version) en $PYTHON3"
# blueutil deja que el daemon se recupere solo de un enlace BLE caducado
# (CoreBluetooth Code=15 "failed to encrypt") tras volver a flashear el firmware,
# sin que tengas que hacerlo a mano con "Forget This Device". Best-effort: se
# instala con Homebrew si está presente y, si no, solo se avisa — el daemon
# degrada con elegancia (deja en los registros una pista para arreglarlo a mano).
if ! command -v blueutil >/dev/null 2>&1; then
    if command -v brew >/dev/null 2>&1; then
        echo "  Instalando blueutil (para la recuperación automática del enlace BLE)..."
        brew install blueutil >/dev/null 2>&1 || echo "  Advertencia: 'brew install blueutil' falló; la recuperación automática queda desactivada."
    else
        echo "  Nota: no se encontró blueutil y tampoco está Homebrew. Instala blueutil"
        echo "        ('brew install blueutil') para activar la recuperación automática de"
        echo "        enlaces BLE caducados; si no, tendrás que olvidar el dispositivo a mano."
    fi
fi
if ! security find-generic-password -s "Claude Code-credentials" -a "$USER" -w >/dev/null 2>&1; then
    echo "Advertencia: no se encontró el token OAuth de Claude Code en el Keychain (servicio 'Claude Code-credentials')."
    echo "  Inicia sesión primero con Claude Code y vuelve a ejecutar este instalador."
    echo "  Se continúa de todas formas — el daemon lo reintentará en cada consulta."
fi
echo "  OK"
echo ""

echo "[2/6] Creando el entorno virtual de Python en daemon/.venv ..."
# Recrea el venv si falta o se construyó con un intérprete anterior a 3.10 (por
# ejemplo, una ejecución previa que eligió el python3 del sistema).
if [ -d "$VENV_DIR" ] && ! py_ge_310 "$VENV_DIR/bin/python"; then
    echo "  El venv existente es demasiado antiguo; se recrea con $PYTHON3"
    rm -rf "$VENV_DIR"
fi
if [ ! -d "$VENV_DIR" ]; then
    "$PYTHON3" -m venv "$VENV_DIR"
fi
"$VENV_DIR/bin/pip" install --quiet --upgrade pip
"$VENV_DIR/bin/pip" install --quiet "bleak>=0.22" "httpx>=0.27"
PYTHON_BIN="$VENV_DIR/bin/python"
echo "  OK ($PYTHON_BIN)"
echo ""

echo "[3/6] Generando el plist de launchd..."
mkdir -p "$HOME/Library/LaunchAgents" "$LOG_DIR"
sed \
    -e "s|__PYTHON_BIN__|${PYTHON_BIN}|g" \
    -e "s|__DAEMON_PATH__|${DAEMON_PY}|g" \
    -e "s|__REPO_DIR__|${SCRIPT_DIR}|g" \
    -e "s|__LOG_OUT__|${LOG_OUT}|g" \
    -e "s|__LOG_ERR__|${LOG_ERR}|g" \
    -e "s|__HOME__|${HOME}|g" \
    "$PLIST_SRC" > "$PLIST_DST"
echo "  Instalado: $PLIST_DST"
echo ""

# Configuración interactiva del daemon: qué planes consultar, más el reloj
# opcional y el aviso sonoro al renovarse la sesión. El daemon lo relee todo en
# cada consulta.
echo "[4/6] Configurando el daemon..."
configure_config_dirs
configure_clock
configure_chime
echo ""

echo "[5/6] Comprobando el permiso de Bluetooth..."
echo "  En la primera ejecución el daemon disparará un aviso de permiso de Bluetooth."
echo "  macOS solo pide permiso a los procesos en primer plano — por eso lo"
echo "  ejecutamos una vez de forma interactiva. Pulsa Ctrl+C cuando veas 'Scanning...'"
echo "  y concede el permiso. Después vuelve a ejecutar este instalador"
echo "  (o simplemente continúa) para activar el arranque automático con launchd."
echo ""
read -r -p "¿Ejecutar ahora un escaneo para preparar el permiso? [Y/n] " ans
if [[ ! "$ans" =~ ^[Nn]$ ]]; then
    "$PYTHON_BIN" "$DAEMON_PY" || true
fi
echo ""

# blueutil necesita su PROPIO permiso de Bluetooth (identidad distinta de la del
# daemon en Python) para recuperarse solo de un enlace caducado. Se BLOQUEA en
# lugar de dar error cuando no está autorizado, así que lo preparamos ahora con
# una espera limitada: devuelve el control al instante si ya está autorizado, o
# dispara el aviso único de permiso de Bluetooth (la concesión se mantiene
# aunque se agote la espera antes de que hagas clic).
if command -v blueutil >/dev/null 2>&1; then
    echo "  Preparando el permiso de Bluetooth de blueutil (concédelo si te lo pide)..."
    blueutil --paired >/dev/null 2>&1 &
    bu_pid=$!
    ( sleep 20; kill "$bu_pid" 2>/dev/null ) >/dev/null 2>&1 &
    bu_killer=$!
    if wait "$bu_pid" 2>/dev/null; then
        echo "  blueutil autorizado — recuperación automática de enlaces caducados activada."
    else
        echo "  blueutil todavía no pudo acceder a Bluetooth. Si la recuperación"
        echo "  automática falla más adelante, concédele el permiso en Ajustes del"
        echo "  Sistema > Privacidad y seguridad > Bluetooth, y vuelve a ejecutar: blueutil --paired"
    fi
    kill "$bu_killer" 2>/dev/null || true
fi
echo ""

echo "[6/6] Cargando el servicio de launchd..."
launchctl unload "$PLIST_DST" 2>/dev/null || true
launchctl load -w "$PLIST_DST"
echo "  Cargado."
echo ""

echo "=== Listo ==="
echo ""
echo "Emparejamiento Bluetooth (la primera vez, tras flashear el firmware):"
echo "  1. Enciende el dispositivo."
echo "  2. Abre Ajustes del Sistema → Bluetooth."
echo "  3. Haz clic en 'Conectar' junto a 'Clawdmeter'."
echo "  4. El daemon lo detectará en unos ~30 s y empezará a consultarlo."
echo ""
echo "Comandos útiles:"
echo "  launchctl list | grep claude-usage     # comprobar que está en ejecución"
echo "  tail -F $LOG_OUT                       # registros en vivo"
echo "  launchctl unload $PLIST_DST            # detener"
echo "  launchctl load -w $PLIST_DST           # iniciar"
