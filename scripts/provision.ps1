<#
.SYNOPSIS
  Graba la WiFi, la credencial de la camara, el host del Fog y (opcional) la
  calibracion de la bateria en la NVS de una ESP32-CAM por el puerto serie.

.DESCRIPTION
  Implementa el lado PC del contrato C (aprovisionamiento por USB):
    1. Valida el JSON localmente (mismas reglas que el firmware).
    2. Abre el puerto con DTR y RTS desactivados: en la base ESP32-CAM-MB RTS
       va a EN (reset) y DTR a GPIO0 (modo descarga).
    3. Reinicia la placa con un pulso de RTS (arranque en frio: asi se abre
       la ventana de aprovisionamiento aunque la placa ya este aprovisionada).
    4. Espera "PROV: READY", envia la linea y muestra la respuesta
       ("PROV: OK ..." o "PROV: ERROR ...") y el arranque que sigue.
  La contrasena, la apiKey y el apiToken nunca se imprimen.

  Ficheros sin tildes a proposito: Windows PowerShell 5.1 lee los .ps1 sin
  BOM como ANSI y estropearia los caracteres acentuados.

.PARAMETER Port
  Puerto serie de la placa, p. ej. COM5 (ver "arduino-cli board list").

.PARAMETER File
  JSON a enviar. Por defecto provision.local.json en la raiz del repo (git
  lo ignora). Plantilla: provision.example.json.

.PARAMETER Erase
  En vez de un JSON, envia {"cmd":"erase"}: borra la NVS del proyecto y la
  placa vuelve a pedir aprovisionamiento.

.EXAMPLE
  .\scripts\provision.ps1 -Port COM5
.EXAMPLE
  .\scripts\provision.ps1 -Port COM5 -File provision.local.json
.EXAMPLE
  .\scripts\provision.ps1 -Port COM5 -Erase
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$Port,
  [string]$File,
  [switch]$Erase,
  [int]$Baud = 115200,           # = SERIAL_BAUD de config.h
  [int]$ReadyTimeoutSec = 15,    # arranque + banner hasta "PROV: READY"
  [int]$ReplyTimeoutSec = 5,     # de enviar la linea a "PROV: OK/ERROR"
  [int]$WatchSec = 30            # cuanto se sigue mostrando el log tras el OK
)

$ErrorActionPreference = 'Stop'

function Fail([string]$msg, [int]$code = 1) {
  Write-Host "ERROR: $msg" -ForegroundColor Red
  exit $code
}

# --- 1. Linea a enviar ------------------------------------------------------
$repoRoot = Split-Path -Parent $PSScriptRoot

if ($Erase) {
  $line = '{"cmd":"erase"}'
  $summary = 'borrar la NVS del proyecto'
} else {
  if (-not $File) {
    $File = Join-Path $repoRoot 'provision.local.json'
  } elseif (-not [IO.Path]::IsPathRooted($File) -and -not (Test-Path -LiteralPath $File)) {
    # Relativo: primero al directorio actual y, si no existe, a la raiz del repo
    $candidate = Join-Path $repoRoot $File
    if (Test-Path -LiteralPath $candidate) { $File = $candidate }
  }
  if (-not (Test-Path -LiteralPath $File)) {
    Fail ("No existe '$File'. Copia provision.example.json a provision.local.json " +
          "(en la raiz del repo) y rellena la WiFi, la camara y el host del Fog.")
  }

  try {
    $cfg = Get-Content -LiteralPath $File -Raw -Encoding UTF8 | ConvertFrom-Json
  } catch {
    Fail "'$File' no es JSON valido: $($_.Exception.Message)"
  }

  # Mismas reglas que provision_module.cpp: fallar aqui es mas claro que leer
  # un "PROV: ERROR" por el serie.
  $known = @('wifi', 'camera', 'fogHost', 'fogPort', 'batteryCal')
  foreach ($p in $cfg.PSObject.Properties.Name) {
    if ($known -notcontains $p) { Fail "Campo desconocido en '$File': '$p' (validos: $($known -join ', '))" }
  }
  if (-not $cfg.wifi) { Fail "Falta 'wifi' en '$File'." }
  $ssid = [string]$cfg.wifi.ssid
  $pass = [string]$cfg.wifi.password
  $ssidBytes = [Text.Encoding]::UTF8.GetByteCount($ssid)
  if ($ssidBytes -lt 1 -or $ssidBytes -gt 32) { Fail "wifi.ssid debe tener entre 1 y 32 bytes (tiene $ssidBytes)." }
  if ($pass.Length -lt 8 -or $pass.Length -gt 63) { Fail "wifi.password debe tener entre 8 y 63 caracteres (WPA2; no se aceptan redes abiertas)." }
  if ($pass -cnotmatch '^[\x20-\x7E]+$') { Fail "wifi.password solo admite ASCII imprimible." }
  if ($pass -eq 'CAMBIAR-por-la-real') { Fail "La contrasena es la de la plantilla: edita '$File'." }

  # Credencial: los tres campos juntos (un solo blob en la NVS).
  if (-not $cfg.camera) { Fail "Falta 'camera' en '$File' ({""id"":""CAM-001"",""apiKey"":""..."",""apiToken"":""...""})." }
  $camKeys = @('id', 'apiKey', 'apiToken')
  foreach ($p in $cfg.camera.PSObject.Properties.Name) {
    if ($camKeys -notcontains $p) { Fail "camera: campo desconocido '$p' (validos: $($camKeys -join ', '))." }
  }
  $camId = [string]$cfg.camera.id
  if ($camId -cnotmatch '^[A-Za-z0-9_-]{1,15}$') { Fail "camera.id debe tener 1-15 caracteres: letras, digitos, '-' y '_' (p. ej. CAM-001)." }
  foreach ($k in @('apiKey', 'apiToken')) {
    $v = [string]$cfg.camera.$k
    if ($v -cnotmatch '^[\x21-\x7E]{1,63}$') { Fail "camera.$k debe tener entre 1 y 63 caracteres ASCII imprimibles, sin espacios." }
    if ($v -like 'CAMBIAR*') { Fail "camera.$k es el de la plantilla: edita '$File'." }
  }
  if ($null -ne $cfg.fogHost) {
    if ([string]$cfg.fogHost -notmatch '^[A-Za-z0-9.-]{1,63}$') {
      Fail "fogHost debe ser un nombre o una IP (letras, digitos, '.', '-'; sin http:// ni :puerto)."
    }
  }
  if ($null -ne $cfg.fogPort) {
    $portNum = 0
    if (-not [int]::TryParse([string]$cfg.fogPort, [ref]$portNum) -or $portNum -lt 1 -or $portNum -gt 65535) {
      Fail "fogPort debe ser un entero entre 1 y 65535."
    }
  }

  $calText = 'defecto'
  if ($null -ne $cfg.batteryCal) {
    # Dos puntos: mV en GPIO13 (linea "BAT:" del log) y mV reales del pack.
    $calKeys = @('adcA', 'batA', 'adcB', 'batB')
    foreach ($p in $cfg.batteryCal.PSObject.Properties.Name) {
      if ($calKeys -notcontains $p) { Fail "batteryCal: campo desconocido '$p' (validos: $($calKeys -join ', '))." }
    }
    $cal = @{}
    foreach ($k in $calKeys) {
      $n = 0
      if (-not [int]::TryParse([string]$cfg.batteryCal.$k, [ref]$n)) { Fail "batteryCal.$k debe ser un entero en mV." }
      $cal[$k] = $n
    }
    if ($cal.adcA -lt 0 -or $cal.adcA -gt 3300 -or $cal.adcB -lt 0 -or $cal.adcB -gt 3300) {
      Fail "batteryCal: adcA/adcB deben estar entre 0 y 3300 mV."
    }
    if ($cal.batA -lt 2500 -or $cal.batA -gt 4500 -or $cal.batB -lt 2500 -or $cal.batB -gt 4500) {
      Fail "batteryCal: batA/batB deben estar entre 2500 y 4500 mV (Li-ion 1S)."
    }
    if ($cal.batB - $cal.batA -lt 300) { Fail "batteryCal: batB debe superar a batA en 300 mV o mas." }
    if ($cal.adcB - $cal.adcA -lt 30) { Fail "batteryCal: adcB debe superar a adcA en 30 mV o mas." }
    $calText = "$($cal.adcA)->$($cal.batA),$($cal.adcB)->$($cal.batB)"
  }

  # Una sola linea: el firmware lee hasta '\n'.
  $line = $cfg | ConvertTo-Json -Compress -Depth 5
  $summary = "camera.id=$camId wifi.ssid=""$ssid"" fogHost=$($cfg.fogHost) fogPort=$($cfg.fogPort) batteryCal=$calText"
}

# --- 2. Puerto ----------------------------------------------------------------
$ports = [System.IO.Ports.SerialPort]::GetPortNames()
if ($ports -notcontains $Port) {
  $list = if ($ports) { $ports -join ', ' } else { 'ninguno' }
  Fail "El puerto '$Port' no existe. Puertos disponibles: $list. (arduino-cli board list)" 2
}

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$sp.Handshake = [System.IO.Ports.Handshake]::None
$sp.DtrEnable = $false     # DTR -> GPIO0: activo = arranque en modo descarga
$sp.RtsEnable = $false     # RTS -> EN: activo = placa en reset
$sp.Encoding  = [Text.Encoding]::UTF8   # por defecto es ASCII: romperia un SSID con tildes
$sp.ReadTimeout = 100

try {
  $sp.Open()
} catch {
  Fail ("No se pudo abrir $Port ($($_.Exception.Message)). " +
        "Cierra el Monitor serie del IDE/VS Code u otro programa que lo use.") 2
}

# Lee lo que haya llegado y devuelve las lineas completas; el resto queda en
# $script:pending para la siguiente llamada.
$script:pending = ''
function Read-Lines {
  $chunk = $sp.ReadExisting()
  if (-not $chunk) { return @() }
  $script:pending += $chunk
  $parts = $script:pending -split "`n"
  $script:pending = $parts[-1]
  if ($parts.Count -le 1) { return @() }
  return $parts[0..($parts.Count - 2)] | ForEach-Object { $_.TrimEnd("`r") }
}

# Espera una linea que contenga alguno de los patrones; muestra todo el log.
function Wait-For([string[]]$patterns, [int]$timeoutSec, [scriptblock]$onTick) {
  $deadline = (Get-Date).AddSeconds($timeoutSec)
  while ((Get-Date) -lt $deadline) {
    foreach ($l in (Read-Lines)) {
      Write-Host "  | $l"
      foreach ($p in $patterns) { if ($l.Contains($p)) { return $l } }
    }
    if ($onTick) { & $onTick }
    Start-Sleep -Milliseconds 50
  }
  return $null
}

try {
  # --- 3. Reinicio por RTS ----------------------------------------------------
  Write-Host "Reiniciando la placa en $Port (pulso RTS)..."
  $sp.RtsEnable = $true
  Start-Sleep -Milliseconds 150
  $sp.RtsEnable = $false
  $sp.DiscardInBuffer()

  $script:hinted = $false
  $script:start = Get-Date
  $ready = Wait-For @('PROV: READY') $ReadyTimeoutSec {
    if (-not $script:hinted -and ((Get-Date) - $script:start).TotalSeconds -gt 4) {
      Write-Host "  (sin respuesta todavia: si tu adaptador no tiene RTS, pulsa RST en la placa ahora)" -ForegroundColor Yellow
      $script:hinted = $true
    }
  }
  if (-not $ready) {
    Fail ("La placa no dijo 'PROV: READY' en $ReadyTimeoutSec s. Comprueba el puerto, " +
          "que el firmware tenga aprovisionamiento (FW_VERSION 2 o mas, con 'camera') y que GPIO0 no este a GND.") 3
  }

  # --- 4. Envio y respuesta -----------------------------------------------------
  Write-Host "Enviando: $summary" -ForegroundColor Cyan
  $sp.Write($line + "`n")

  $reply = Wait-For @('PROV: OK', 'PROV: ERROR') $ReplyTimeoutSec $null
  if (-not $reply) { Fail "Sin respuesta 'PROV: OK/ERROR' en $ReplyTimeoutSec s." 4 }
  if ($reply.Contains('PROV: ERROR')) { Fail "La placa rechazo la configuracion: $reply" 1 }

  Write-Host "OK: la placa guardo la configuracion y reinicia." -ForegroundColor Green
  if ($WatchSec -gt 0) {
    Write-Host "Log del arranque siguiente (max. $WatchSec s, hasta el deep sleep):"
    [void](Wait-For @('SLEEP: durmiendo') $WatchSec $null)
  }
  exit 0
} finally {
  if ($sp.IsOpen) { $sp.Close() }
}
