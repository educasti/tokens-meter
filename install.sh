#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_NAME="claude-usage-daemon"
SERVICE_FILE="$SCRIPT_DIR/daemon/$SERVICE_NAME.service"
USER_SERVICE_DIR="$HOME/.config/systemd/user"
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

# Detecta los directorios de configuración ~/.claude* que contienen credenciales
# y, si encuentra más de uno, deja que el usuario elija qué planes mostrar. El
# daemon consulta todos los directorios elegidos y muestra el que esté activo.
# Escribe la clave `config_dirs` conservando las demás claves que ya estén en el
# archivo de configuración. No hace nada (se queda con ~/.claude) si solo existe
# un directorio o el usuario no quiere añadir ninguno.
configure_config_dirs() {
    local -a candidates=()
    local d
    for d in "$HOME"/.claude*; do
        [ -d "$d" ] && [ -f "$d/.credentials.json" ] && candidates+=("$d")
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
# predeterminado, para que quien pulse Enter en todo el instalador no llene la
# configuración con valores por omisión.
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

echo "=== Claude Usage Tracker - Instalación ==="
echo ""

# Comprobar las dependencias
echo "[1/4] Comprobando las dependencias..."
for cmd in curl awk bluetoothctl busctl dbus-monitor python3 setsid stdbuf systemctl; do
    command -v "$cmd" >/dev/null || { echo "Error: $cmd es necesario, pero no está instalado"; exit 1; }
done
echo "  Todas las dependencias encontradas"
echo ""

# Instalar el servicio de usuario de systemd con la ruta ya resuelta
echo "[2/4] Instalando el servicio de usuario de systemd..."
mkdir -p "$USER_SERVICE_DIR"
DAEMON_BIN="$SCRIPT_DIR/daemon/$SERVICE_NAME.sh"
sed "s|DAEMON_PATH|${DAEMON_BIN}|g" "$SERVICE_FILE" > "$USER_SERVICE_DIR/$SERVICE_NAME.service"
systemctl --user daemon-reload

# Configuración interactiva del daemon: qué planes consultar, más el reloj
# opcional y el aviso sonoro al renovarse la sesión. El daemon lo relee todo en
# cada consulta.
echo "[3/4] Configurando el daemon..."
configure_config_dirs
configure_clock
configure_chime
echo ""

# Habilitar el servicio
echo "[4/4] Habilitando el servicio..."
systemctl --user enable "$SERVICE_NAME"

echo ""
echo "=== ¡Listo! ==="
echo ""
echo "El daemon se iniciará automáticamente cuando inicies sesión"
echo "y se conectará al dispositivo por Bluetooth Low Energy."
echo ""
echo "Emparejamiento Bluetooth (la primera vez):"
echo "  1. Enciende el dispositivo"
echo "  2. Ejecuta: bluetoothctl scan le"
echo "  3. Busca 'Clawdmeter' y anota la dirección MAC"
echo "  4. Ejecuta: bluetoothctl pair <MAC>"
echo "  5. Ejecuta: bluetoothctl trust <MAC>"
echo "  6. Inicia el daemon: systemctl --user start $SERVICE_NAME"
echo ""
echo "Comandos útiles:"
echo "  systemctl --user status $SERVICE_NAME    # ver el estado"
echo "  journalctl --user -u $SERVICE_NAME -f    # ver los registros"
echo "  systemctl --user restart $SERVICE_NAME   # reiniciar"
echo "  systemctl --user stop $SERVICE_NAME      # detener"
echo ""
