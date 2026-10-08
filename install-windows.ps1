# install-windows.ps1 - Instalación lista para usar de Clawdmeter en Windows (D-09)
#
# Crea un entorno virtual de Python, instala las dependencias de
# daemon\requirements-windows.txt, registra la aplicación de bandeja para que se
# inicie al iniciar sesión (HKCU\...\Run, sin necesidad de administrador) y la
# arranca de inmediato.
#
# Uso:
#   powershell -ExecutionPolicy Bypass -File install-windows.ps1
#
# O bien, si ya has definido una política de ejecución permisiva:
#   .\install-windows.ps1
#
# Para desactivar más adelante el arranque automático: haz clic derecho en el
# icono de la bandeja -> desmarca "Start at login"
# O elimínalo a mano: reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v Clawdmeter /f
#
# Seguridad: este script no descarga nada de internet. Instala únicamente los
# paquetes listados en daemon\requirements-windows.txt del repositorio.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Log {
    param([string]$Msg)
    $ts = Get-Date -Format "HH:mm:ss"
    Write-Host "[$ts] $Msg"
}

$RepoRoot = $PSScriptRoot
if (-not $RepoRoot) {
    $RepoRoot = (Get-Location).Path
}

Log "=== Instalación de Clawdmeter en Windows ==="
Log "Raíz del repositorio: $RepoRoot"

# ------------------------------------------------------------------
# Protección: no instalar desde una ruta de WSL (APP-02 / SC#4 / SC#5)
# ------------------------------------------------------------------
# Si $RepoRoot está en el recurso compartido de WSL (\\wsl$\... o
# \\wsl.localhost\...), tanto el venv como la entrada de arranque automático en
# HKCU\Run apuntarían a una ruta que desaparece al apagar WSL -- justo la
# dependencia de WSL que este proyecto existe para eliminar. Copia primero el
# repositorio a una ruta nativa de Windows.
if ($RepoRoot -match '\\\\wsl(\$|\.localhost)\\') {
    throw @"
Se cancela la instalación desde una ruta de WSL:
  $RepoRoot

El daemon de Clawdmeter debe ser independiente de WSL. Instalar desde el recurso
compartido de WSL haría que el entorno virtual y la entrada de arranque
automático apuntaran a una ruta inaccesible una vez que WSL se apague.

Solución: copia este repositorio a una ubicación nativa de Windows y ejecuta ahí
el instalador, por ejemplo:

  Copy-Item -Recurse '$RepoRoot' "$env:USERPROFILE\Clawdmeter"
  cd "$env:USERPROFILE\Clawdmeter"
  powershell -ExecutionPolicy Bypass -File install-windows.ps1
"@
}

# ------------------------------------------------------------------
# Paso 1: Crear el entorno virtual
# ------------------------------------------------------------------
$VenvDir = Join-Path $RepoRoot ".venv"
if (Test-Path $VenvDir) {
    Log "El entorno virtual ya existe en .venv - se omite su creación"
} else {
    Log "Creando el entorno virtual en .venv ..."
    & python -m venv $VenvDir
    if ($LASTEXITCODE -ne 0) { throw "No se pudo crear el entorno virtual (código de salida $LASTEXITCODE)" }
    Log "Entorno virtual creado"
}

# ------------------------------------------------------------------
# Paso 2: Instalar las dependencias
# ------------------------------------------------------------------
$PythonExe  = Join-Path $VenvDir "Scripts\python.exe"
$PythonwExe = Join-Path $VenvDir "Scripts\pythonw.exe"
$RequirementsFile = Join-Path $RepoRoot "daemon\requirements-windows.txt"

Log "Instalando las dependencias de daemon\requirements-windows.txt ..."
& $PythonExe -m pip install --quiet -r $RequirementsFile
if ($LASTEXITCODE -ne 0) { throw "La instalación con pip falló (código de salida $LASTEXITCODE)" }
Log "Dependencias instaladas"

# ------------------------------------------------------------------
# Paso 3: Registrar el arranque automático (HKCU\Run, por usuario, sin admin)
# ------------------------------------------------------------------
# Calcula todas las rutas en el momento de la instalación - nunca escribas una
# ruta absoluta fija que se rompa al mover el repositorio (lección de CLAUDE.md
# "repoint ExecStart", antipatrón de RESEARCH).
$TrayScript = Join-Path $RepoRoot "daemon\tray_windows.py"

Log "Registrando el arranque automático (HKCU\Software\Microsoft\Windows\CurrentVersion\Run) ..."
# Invoca el ayudante de arranque automático con el python del venv recién creado
# para que sys.executable apunte al pythonw.exe del venv (la ruta que se escribirá
# en el registro).
& $PythonExe -c @"
import sys, os
sys.path.insert(0, r'$RepoRoot')
import daemon.autostart_windows as a
a.enable(tray_script=r'$TrayScript')
"@
if ($LASTEXITCODE -ne 0) { throw "El registro del arranque automático falló (código de salida $LASTEXITCODE)" }
Log "Arranque automático registrado - Clawdmeter se iniciará solo en el próximo inicio de sesión"

# ------------------------------------------------------------------
# Paso 4: Arrancar la aplicación de bandeja (sin consola - pythonw.exe BASE)
# ------------------------------------------------------------------
# Usa el pythonw.exe del intérprete BASE, NO el de Scripts\pythonw.exe del venv.
# El pythonw del venv es un redirigente que vuelve a lanzar la compilación CON
# consola de python.exe como hijo (un error del lanzador de venv de CPython), lo
# que abre una ventana de consola negra.
# tray_windows.py añade él solo los site-packages del venv a sys.path, así que las
# dependencias del venv siguen resolviéndose. (Ver autostart_windows._command -
# mismo motivo.)
$BasePrefix  = & $PythonExe -c "import sys; print(sys.base_exec_prefix)"
$BasePythonw = Join-Path $BasePrefix "pythonw.exe"

Log "Iniciando la aplicación de bandeja ..."
$StartArgs = @{
    FilePath         = $BasePythonw
    ArgumentList     = "`"$TrayScript`""
    WorkingDirectory = $RepoRoot
}
Start-Process @StartArgs
Log "Aplicación de bandeja iniciada - busca el icono de Clawdmeter en el área de notificación"
Log "=== Instalación completada ==="
