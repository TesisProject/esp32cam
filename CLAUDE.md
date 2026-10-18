# CLAUDE.md

Contexto para Claude Code al trabajar en este repositorio.

## Qué es

Firmware para **ESP32-CAM AI-Thinker**, proyecto de tesis UPC ("ParkVision").
El dispositivo es **cliente**: cada ciclo captura una foto, la sube por POST a
una Raspberry Pi local ("fog node") que detecta plazas de parking y reenvía el
resultado a la nube, y entre captura y captura se apaga casi por completo
(deep sleep) para maximizar la autonomía con baterías Li-ion. No hay dashboard ni
servidor HTTP local: eso era la arquitectura anterior (ver "Migración" abajo).

Hay **3 unidades físicas** (`CAM-001`, `CAM-002`, `CAM-003`), mismo código
fuente y **un único `.bin`** sin secretos: la WiFi, la credencial
(`camera`: id, `apiKey`, `apiToken`) y el host del Fog se graban por USB en
la NVS de cada placa (ver "Aprovisionamiento"). `secrets.h` ya no existe
(desde `FW_VERSION` 2).

**Rumbo (decisión del 2026-10-12):** toda la configuración se define en la
nube, el Fog la descarga y cada ESP32 lee la suya del Fog; un único `.bin`
para las tres cámaras, sin secretos dentro. **Excepción (2026-10-17): la
WiFi, el host del Fog y la credencial solo se graban por USB**, nunca por
red (ver "OTA y configuración por USB"). La credencial pasó a USB el mismo
día a petición del usuario, en lugar del alta por MAC de E7. Plan y
contratos: `../delegacion-cambios-2026-10-11.md`, revisión 3 (E6 hecho; la
credencial por USB sustituye a E7 y el contrato C del plan no lo refleja
aún; E5 hecho, E8 pendiente).

El sketch está en la raíz del repo (`esp32cam.ino`). El sketch original
(fotos a microSD) se eliminó del repo en `d17d837`.

## Build

**Arduino IDE o `arduino-cli`**, no PlatformIO (se descartó deliberadamente:
su plataforma oficial va con el core Arduino 2.x y aquí se usa el 3.x).
`arduino-cli` está en el PATH y comparte los cores con el IDE
(`%LOCALAPPDATA%\Arduino15`, `esp32:esp32` 3.3.12), así que **sí se puede
compilar desde aquí**:

```
arduino-cli compile --fqbn esp32:esp32:esp32cam:PartitionScheme=min_spiffs --build-path build .
```

En VS Code, `.vscode/tasks.json` tiene Verificar / Subir / Monitor serie
(`Ctrl+Shift+B` = Verificar), con ese mismo FQBN en las tres tareas que lo
llevan. `build/` y `.vscode/` están en `.gitignore`.

Ajustes obligatorios en Herramientas (en el CLI van en el FQBN: la placa
`esp32cam` ya trae PSRAM, pero su partición por defecto es Huge APP, así que
el esquema hay que forzarlo):

| Opción | Valor |
|---|---|
| Placa | AI Thinker ESP32-CAM |
| Partition Scheme | **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)** — `PartitionScheme=min_spiffs` |
| PSRAM | **Enabled** |

**Por qué Minimal SPIFFS.** Huge APP tiene una sola ranura de app de 3 MB:
no hay dónde escribir una versión nueva por OTA. Minimal SPIFFS tiene dos
(`app0`/`app1`) de 1 966 080 B; el firmware ocupa ~1,24 MB (62 %). *Default*
(1,25 MB por ranura) dejaría ~86 %, demasiado justo. Cambiar de esquema
obliga a flashear cada placa por USB una vez (cambia la tabla de
particiones); después, el OTA no puede cambiarla. **No volver a Huge APP**
sin hablarlo: rompería el OTA y el rollback.

**OTA (E5).** `FW_VERSION` (entero, `config.h`) sale en el log de arranque
junto a la partición activa y viaja al Fog como `fw_version`. El cliente
está en `src/ota/` + `src/fogconfig/`; ver la sección "Cliente OTA (E5)". El rollback del bootloader está activo en el core
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`): `src/ota/ota_module.cpp`
sobrescribe `verifyRollbackLater()` (con `extern "C"`: el weak está en un `.c`
del core) para que el core no valide la imagen al arrancar, y el `.ino` llama
a `ota_confirm_if_pending()` solo tras una subida 2xx. Una imagen OTA que se
duerme sin eso vuelve sola a la anterior al despertar. Las imágenes
flasheadas por USB quedan en estado `UNDEFINED` y no se ven afectadas. **Subir
`FW_VERSION`** en cada binario que vaya a otra placa.

**Sin librerías externas.** Solo lo que trae el core ESP32: `HTTPClient`,
`Preferences` (NVS), `LittleFS` (fotos atrasadas) y cJSON
(`#include "cJSON.h"`, viene en los libs de ESP-IDF del core). No añadir dependencias del Gestor de Librerías sin
preguntar: fue un requisito explícito del usuario.

## Estructura

```
esp32cam/                       ← carpeta = nombre del .ino (lo exigen IDE y CLI)
├── esp32cam.ino                Orquestador: setup() = un ciclo completo -> deep sleep. No hay loop() real.
├── config.h                    TODAS las constantes y los valores por defecto
├── provision.example.json      Plantilla de la línea de aprovisionamiento (WiFi + credencial + Fog)
├── provision.local.json        La real, por placa/red — gitignored
├── scripts/provision.ps1       Envía la línea por serie (reinicia la placa por RTS)
├── tools/battery_monitor/      Sketch aparte de banco: AP propio + web con la batería (copia la recta)
├── tools/wifi_rssi/            Sketch aparte: RSSI de la red (serie + web + LED); lee la WiFi de la NVS
├── tools/ota_test/             Sketch aparte de banco: recibe un .bin por HTTP (sin auth) para probar OTA y rollback
└── src/
    ├── camera/camera_module.*  OV2640: init, captura, PWDN + hold para deep sleep
    ├── wifi/wifi_module.*      Estación: conexión con timeout, MAC, IP fija, apagado
    ├── upload/upload_module.*  POST multipart/form-data (foto + telemetría) a la Raspberry Pi
    ├── ota/ota_module.*        FW_VERSION, rollback y cliente OTA: firma, descarga, instalación
    ├── fogconfig/fogconfig_module.*  GET /camera/config del Fog (solo el bloque firmware; wifi se ignora)
    ├── battery/battery_module.*  Tensión del pack por GPIO13 (ADC2, antes del WiFi) y %
    ├── clock/clock_module.*    Hora real con la cabecera Date del Fog; deriva del RTC
    ├── backlog/backlog_module.*  Fotos no subidas en LittleFS (partición spiffs), cola y reenvío
    ├── settings/settings_module.*   ÚNICA puerta a la NVS (Preferences), esquema completo
    └── provision/provision_module.* Lee/valida la línea JSON por serie y la guarda vía settings
```

Arduino (IDE y CLI) compila la raíz del sketch y **solo** `src/` de forma
recursiva; cualquier otra subcarpeta se ignora. Módulos nuevos en
`src/<nombre>/<nombre>_module.*`. El sufijo `_module` evita nombres como
`wifi.h`, que en Windows (sin distinción de mayúsculas) se confunde con
`<WiFi.h>` del core. Los módulos incluyen `"../../config.h"`;
`config.h` se queda en la raíz porque es lo que se edita al compilar. El IDE 2.x no muestra `src/` como pestañas.

## Invariantes del diseño

Estas reglas se acordaron con el usuario. No romperlas sin avisar.

**1. `config.h` tiene todas las constantes y los valores por defecto; la
configuración real de cada placa vive en la NVS.** Ningún otro fichero
contiene pines, umbrales, intervalos, resoluciones ni límites: si hace falta
un valor nuevo, se define en `config.h`.

Lo que es propio de cada placa o despliegue **no se compila**: la WiFi, el
host/puerto del Fog y la credencial viven en la NVS y se graban solo por
USB; más adelante, los parámetros de captura (E8) también irán en la NVS y
llegarán desde la nube a través del Fog. Lo que `config.h` tenga para esos datos
(`FOG_SERVER_HOST_DEFAULT`, `FOG_SERVER_PORT_DEFAULT`, `CAM_FRAMESIZE_SNAPSHOT`…)
es solo el **defecto** cuando la NVS no tiene valor: cambiarlo no afecta a una
placa ya aprovisionada. Un defecto nunca puede ser un secreto (ni un SSID, ni
una contraseña, ni una IP de la red del proyecto).

La NVS solo se toca desde `src/settings/settings_module.*` (ver
"Aprovisionamiento").

No hay `secrets.h` ni ningún otro fichero de secretos que compilar: no
volver a introducirlo. `.gitignore` sigue ignorando `secrets.h` solo para
que una copia antigua con claves reales no acabe en un commit.

**2. Sin librerías externas.** Solo el core ESP32 (`HTTPClient`,
`Preferences`, `LittleFS`, cJSON).

**3. El endpoint de subida es fijo, no inventar campos.** El contrato (ver
`config.h`, sección 3, y "Contrato de la API" abajo) es un POST multipart a
`/api/v1/camera/frames` con la foto en `file` y, en el mismo multipart, estos
campos de telemetría acordados con el Fog (todos texto, todos opcionales para
él; nombres exactos de `fog-application/src/vision/camera_router.py`):

| Campo | Qué manda el firmware |
|---|---|
| `fw_version` | `FW_VERSION` de `config.h` (entero) |
| `rssi` | `WiFi.RSSI()`, dBm |
| `wifi_connect_ms` | `WiFi.begin` → `WL_CONNECTED`, ms (resolución 250 ms) |
| `reset_reason` | `esp_reset_reason()`: `8` = deep sleep, `9` = brownout |
| `awake_ms` | `millis()` justo antes del POST (sin ROM/bootloader ni el propio POST) |
| `free_heap` | `ESP.getFreeHeap()`: solo RAM interna |
| `wake_count` | despertares de deep sleep desde el último arranque en frío (`RTC_DATA_ATTR`; `0` tras cualquier reset que no sea deep sleep) |
| `wifi_fail_streak` | ciclos seguidos sin WiFi **antes** de este (`RTC_DATA_ATTR`; se pone a 0 al conectar, después de leerlo) |
| `framesize` | nombre del enum aplicado de verdad (`SVGA`, o `QVGA` si falta PSRAM) |
| `jpeg_quality` | calidad aplicada de verdad (10–63) |
| `battery_valid` | `true`/`false`: la lectura de GPIO13 cae dentro de 2,5–4,5 V |
| `battery_mv` | tensión del pack en reposo, mV (solo si `battery_valid` es `true`) |
| `captured_at` | **solo en fotos atrasadas** (ver "Fotos atrasadas"): ISO 8601 UTC, `2026-10-18T14:05:12Z`. La foto actual no lo lleva |

Una foto atrasada lleva solo los campos del momento de la captura
(`fw_version`, `reset_reason`, `framesize`, `jpeg_quality`, batería y
`captured_at`): `rssi`, `wifi_connect_ms`, `awake_ms`, `free_heap`,
`wake_count` y `wifi_fail_streak` describirían el ciclo de la subida, no el
de la foto.

**No** se envía el porcentaje: el Fog guarda medidas y deriva el % al leer
(su README dice que ignora `battery_percent`), así que una curva nueva no
invalida el histórico. El % solo sale en el log serie. Tampoco
`battery_load_mv`: GPIO13 es ADC2 y no se puede leer con el WiFi
transmitiendo. Ningún campo nuevo (timestamp, % de batería…) sin
confirmarlo antes con el backend. El `.ino`
recoge los valores y se los pasa a `upload_frame()` en un `UploadTelemetry`
(`upload_module.h`); el módulo de subida solo los serializa.

## Hardware: restricciones que NO son obvias

Esto ya costó una corrección. No repetir los errores.

**GPIO21 y GPIO22 NO están libres.** En la AI-Thinker `GPIO21 = Y5` (dato de
cámara) y `GPIO22 = PCLK`. El OV2640 ocupa:
`0, 5, 18, 19, 21, 22, 23, 25, 26, 27, 32, 34, 35, 36, 39`.

**Batería por GPIO13 (ADC2).** Alimentación: una celda 18650 (1S,
3,0–4,2 V) → TP4056 con protección → MT3608 a 5,0 V → pin `5V` → AMS1117.
Divisor 100k/100k desde VIN+ del MT3608 a GPIO13, 100 nF a GND (se quitó el
ADS1115 y las pilas AA). El ADC2 **sí** sirve si se lee antes de
`wifi_connect()`: lo que no funciona es leerlo con el WiFi encendido.
En GPIO13 solo está el divisor: `V13 = Vbat/2` (1,50–2,10 V, zona lineal del
ADC a 12 dB). Un handoff anterior suponía un pull-up de 47k en la placa que
llevaba a `0,242·Vbat + 1,70 V`; medido en la placa real el 2026-10-13, no
es así (con aquellos defectos una celda de ~3,9 V salía como 1,06 V). Se
calibra igual con dos puntos por el error de ganancia/offset del ADC de cada
chip; va en la NVS (`batteryCal`).

**No hay microSD, ni siquiera en modo 1 bit.** En modo 1 bit la SD solo usa
GPIO14/15/2, pero la pista D3 del slot sigue unida a GPIO13, y toda
microSD tiene en DAT3 un pull-up interno (10–90 kΩ a 3,3 V) activo desde
que recibe alimentación. La tarjeta está siempre alimentada (rail de 3,3 V,
también en deep sleep), así que leer la batería antes de montar la SD no lo
evita: con 50 kΩ, `V13 = (Vbat + 6,6)/4` (2,40–2,70 V), la mitad de rango y
fuera de la zona lineal del ADC. Para tener las dos cosas habría que cortar
la pista DAT3 o mover el divisor a GPIO33 (ADC1, quitando el LED rojo). Las
fotos atrasadas van a la flash interna (ver "Fotos atrasadas").
GPIO14/15 están libres.
**GPIO12 nunca** para un bus con pull-up: strapping MTDI, impide arrancar.

**GPIO32 (PWDN de la cámara) es RTC-capable.** El deep sleep del ESP32 apaga
la CPU pero **no** corta la alimentación de la cámara (comparte el rail de
3,3 V) ni conserva el nivel de los pines por defecto. Por eso
`camera_power_down()` pone PWDN en alto y llama a `rtc_gpio_hold_en()`; el
siguiente `camera_init()` debe soltar el hold con `rtc_gpio_hold_dis()` antes
de reconfigurar el pin, o `pinMode`/`digitalWrite` no tienen efecto.

**Monitor serie con `arduino-cli`: `dtr=off,rts=off`.** Por defecto los deja
activos y en la ESP32-CAM-MB RTS va a EN (placa en reset permanente) y DTR a
GPIO0 (arranque en modo descarga).

## Aprovisionamiento por USB y NVS

Contrato C del plan: una línea JSON por el puerto serie, una vez por placa.

```
{"wifi":{"ssid":"…","password":"…"},"camera":{"id":"CAM-001","apiKey":"…","apiToken":"…"},"fogHost":"192.168.1.20","fogPort":8000}
{"cmd":"erase"}        ← borra el namespace del proyecto y vuelve a pedir aprovisionar
```

Se puede enviar con el script o pegarla en el monitor serie. Aprovisionada =
`wifi` **y** `cred` en la NVS (`settings_is_provisioned()`); `wifi` y
`camera` solo son obligatorios en la línea si la NVS no los tiene ya.
Opcional: `"batteryCal":{"adcA":…,"batA":…,"adcB":…,"batB":…}` (mV en GPIO13
y mV reales del pack, dos puntos). Al reaprovisionar puede ir sola.

Respuestas, siempre con `Serial` directo (no `LOG()`: son protocolo y deben
salir aunque `DEBUG_ENABLED` sea 0): `PROV: READY …` (escuchando),
`PROV: OK …` (guardado, reinicia) y `PROV: ERROR <motivo>` (sigue escuchando).

**Cuándo escucha** (en `setup()`, antes de arrancar la cámara):

| Situación | Qué hace | Coste |
|---|---|---|
| NVS sin WiFi o sin credencial | espera la línea `PROV_WAIT_MS` (60 s); si no llega, deep sleep `PROV_RETRY_SLEEP_MS` (1 h) | ~0,7 mAh/h en campo sin USB: estado de error, no puede esperar indefinidamente |
| Aprovisionada y `ESP_RST_POWERON` (encendido, botón RST, pulso RTS) | ventana de `PROV_WINDOW_MS` (3 s) para reescribir o borrar | solo en arranques en frío |
| Despertar de deep sleep, `ESP_RST_SW`, brownout, cuelgue | nada | los ciclos normales no pagan nada |

**Validación** (en el firmware y también en el script): SSID de 1–32 bytes;
contraseña WPA2 de 8–63 caracteres ASCII imprimibles (se rechazan las redes
abiertas); `fogHost` solo letras, dígitos, `.` y `-` (así se atrapa el
típico `http://…` o `…:8000`), hasta 63; `fogPort` entero 1–65535;
`camera` con los tres campos: `id` 1–15 de letras, dígitos, `-` y `_`;
`apiKey`/`apiToken` 1–63 ASCII imprimible sin espacios (van en cabeceras
HTTP). Un campo desconocido es error (un `fog_host` mal escrito no se ignora
en silencio). Se valida todo antes de escribir nada y se escribe la WiFi
**la última** (Fog → calibración → credencial → WiFi): en una placa nueva la
WiFi es la que completa el aprovisionamiento, así que un corte entre
escrituras deja la placa pidiendo aprovisionar de nuevo, no a medias.
`fogHost`/`fogPort` son opcionales: sin ellos se usan los defectos de
`config.h`. `batteryCal`: los cuatro enteros, `adc*` ≤ 3300, `bat*` en
2500–4500, B por encima de A en ≥ 300 mV de batería y ≥ 30 mV de ADC. La
contraseña, la `apiKey` y el `apiToken` nunca salen por el serie (el `id` sí).

**NVS** (`settings_module`, namespace `SETTINGS_NVS_NAMESPACE`): esquema
completo desde E6 (WiFi, Fog, calibración, credencial, captura, `cfg_rev`,
versión OTA rechazada); la tabla de claves está en `settings_module.h`. Cada
grupo que se escribe junto es un blob (escritura atómica). Un cambio de
`SETTINGS_SCHEMA_VERSION` borra el namespace **salvo** WiFi, Fog,
calibración y credencial: ver "OTA y configuración por USB" antes de tocarlo. En un
ciclo normal solo se lee: no hay escrituras en flash por ciclo. `{"cmd":"erase"}` borra solo el namespace del proyecto, no
la NVS del driver WiFi ni la calibración de radio (`WiFi.persistent(false)`:
el driver no guarda copia del SSID/contraseña).

**Script:** `scripts/provision.ps1 -Port COM5 [-File provision.local.json] [-Erase]`
(tareas de VS Code "Arduino: Aprovisionar" y "Arduino: Borrar NVS"). Valida
el JSON en local, abre el puerto con DTR/RTS a `false`, reinicia con un
pulso de RTS de 150 ms (eso da `ESP_RST_POWERON`, que abre la ventana aunque
la placa ya esté aprovisionada), espera `PROV: READY`, envía la línea y
muestra el log hasta el `SLEEP:`. Escrito sin tildes: Windows PowerShell 5.1
lee los `.ps1` sin BOM como ANSI. **No ejecutarlo desde una sesión de agente
contra la placa** sin que el usuario lo pida: reinicia la placa y escribe su
NVS.

## OTA y configuración por USB — reglas para no dejar cámaras incomunicadas

**Leer antes de tocar `settings_module`, `provision_module`, `config.h`
(secciones 7 y 8) o cualquier cosa del OTA.** Decisión del usuario del
2026-10-17.

**El principio:** lo que solo se graba por USB no se puede recuperar a
distancia. Si un firmware lo borra o lo deja ilegible, la cámara no
encuentra la red o el Fog, no puede recibir otro OTA que lo arregle, y hay
que ir a cada placa con un cable. Y por OTA el error llega a **las tres
cámaras a la vez**.

**Qué se graba solo por USB** (aprovisionamiento, contrato C):

| Clave NVS | Struct | Tamaño congelado |
|---|---|---|
| `wifi` | `SettingsWifi` | 97 B |
| `fog` | `SettingsFog` | 66 B |
| `bat_cal` | `SettingsBatteryCal` | 8 B |
| `cred` | `SettingsCredential` | 144 B |

Lo demás (captura E8, `cfg_rev`, `ota_rej`) se puede volver a pedir al Fog:
perderlo cuesta un ciclo, no un viaje.

**Reglas:**

1. **La WiFi, el host del Fog y la credencial nunca llegan por red.** Ni la nube ni el Fog
   los envían, y el firmware no acepta cambiarlos por HTTP. Si alguien pide
   añadir `wifi` a `GET /camera/config` o "redes por nodo", recordar esta
   decisión antes de implementarlo (revisión 3 del plan).
2. **No cambiar la forma de esos cuatro structs ni renombrar sus claves** (ni
   `schema`). Los `static_assert` de `settings_module.h` lo impiden al
   compilar; no quitarlos para que compile.
3. **Dato nuevo = clave nueva**, con su defecto cuando no existe (como se
   hizo con `bat_cal`). Así `SETTINGS_SCHEMA_VERSION` se queda en 1.
4. **Si de verdad hay que subir `SETTINGS_SCHEMA_VERSION`:**
   `reset_keeping_usb_keys()` borra todo menos `wifi`, `fog`, `bat_cal` y `cred`,
   clave a clave y con el esquema escrito al final (un corte a mitad se
   repite en el siguiente arranque). Nunca `clear()` del namespace en un
   cambio de esquema. Una clave nueva grabada por USB se añade a
   `kUsbKeys` en el `.cpp`, con su `static_assert`.
5. **Primer OTA:** cada placa tiene que llevar, flasheado por USB, un
   firmware con `reset_keeping_usb_keys()` **antes** de recibir su primer
   OTA. El de E6 (`6066aa6`) hace `clear()`: si un OTA fallido hiciera
   rollback a ese firmware tras un cambio de esquema, borraría la WiFi.
6. **El cliente OTA (E5) no toca la NVS** salvo `ota_rej`. La NVS es otra
   partición: escribir `app0`/`app1` no la afecta, y el rollback tampoco.
7. **No cambiar el esquema de particiones** (Minimal SPIFFS): el OTA no
   puede cambiarlo, y moverlo puede dejar la NVS ilegible.
8. **Aprovisionamiento:** `{"cmd":"erase"}` sí borra la WiFi, a propósito,
   porque solo llega por USB con alguien delante. Nada que llegue por red
   puede provocar un `settings_erase_all()`.
9. **Un `FW_VERSION` nuevo por cada binario** que vaya a otra placa, y un
   firmware OTA solo confirma la imagen (`ota_confirm_if_pending()`) tras un
   2xx del Fog: si no conecta, el rollback lo devuelve al anterior.

**Prueba antes de publicar un OTA que toque `settings_module`:** en una
placa aprovisionada, flashear por USB el firmware nuevo y comprobar en el
log que sigue conectando sin reaprovisionar (`NVS: esquema …` solo si se
subió el esquema, y después `WIFI: conectado`).

## Energía — deep sleep real

A diferencia de la arquitectura anterior (servidor siempre disponible, deep
sleep inviable), ahora **sí** se usa deep sleep entre capturas: no hay nada
que servir entre ciclos.

- Cada ciclo: NVS (WiFi, Fog, calibración) → batería → cámara → WiFi STA →
  capturar → POST → (si 2xx: hora del Fog y hasta 3 fotos atrasadas; si no:
  guardar la foto) → apagar cámara (PWDN + hold) → WiFi off →
  `esp_deep_sleep_start()`.
- Corte por batería baja: por debajo de `BATTERY_CUTOFF_MV` (3,3 V) se duerme
  sin encender cámara ni WiFi, y no se vuelve a transmitir hasta superar
  `BATTERY_RESUME_MV` (3,4 V; histéresis en `RTC_DATA_ATTR`). Con una lectura
  no válida (placa sin divisor, USB sin celda) **no** se corta.
- `camera_power_down()` apaga el sensor también si no se llegó a iniciar
  (placa sin aprovisionar o init fallido): si no, dormiría con la cámara
  alimentada y PWDN flotando.
- `WIFI_CONNECT_TIMEOUT_MS` acota el intento de conexión: si la Pi o el router
  no responden, el ciclo se aborta y se duerme igual (no reintentar
  indefinidamente, gasta batería con pilas).
- Solo modo estación: un AP propio obligaría a emitir balizas sin parar y la
  radio no podría dormir nunca.
- `CAPTURE_INTERVAL_MS` en `config.h` (5 min en campo; bajarlo a 30 s solo
  para pruebas, y devolverlo antes de dejar la cámara desatendida).

## Cliente OTA (E5)

Contrato del Fog (proveedor, fuente de verdad):
`fog-application/src/firmware/router.py`, `service.py` (`firmware_block`) y
`signing.py`; firma de la nube: `FirmwareManifest.canonicalString()`.

- **Cuándo:** tras una subida 2xx, cada `OTA_CHECK_EVERY_CYCLES` (12 = 1 h)
  ciclos, y en el primer ciclo bueno tras un arranque en frío, se pide
  `GET /api/v1/camera/config`. Solo se lee su bloque `firmware`
  (`{version, size, sha256, signature, url}` o `null`). **`wifi` se ignora
  siempre** (regla 1 de abajo), aunque el Fog lo mande.
- **Qué se instala:** solo si `version > FW_VERSION` y `version != ota_rej`.
- **Orden de las comprobaciones** (de barata a cara): versión → `ota_rej` →
  rollback ya ocurrido con esa versión → batería (≥ `OTA_MIN_BATTERY_MV`,
  salvo lectura no válida en el banco) → **firma** → descarga. Una firma
  mala no cuesta ninguna descarga.
- **Firma:** ECDSA P-256 + SHA-256, DER en Base64, sobre
  `parkvision-fw/1|esp32cam-aithinker|<version>|<size>|<sha256 hex minúsculas>`,
  verificada con mbedTLS (`mbedtls_pk_verify`) y la clave pública compilada
  (`OTA_SIGNING_PUBLIC_KEY_PEM`, `config.h` §10). **Vacía = OTA
  desactivado.** Se firma la cadena y no el binario: un binario antiguo y
  legítimo no se puede presentar como una versión mayor.
- **Descarga:** `GET <url>` con `X-API-Key`/`X-API-Token`, streaming a la
  ranura libre (`OTA_WITH_SEQUENTIAL_WRITES`); exige `Content-Length == size`
  y que el SHA-256 de lo recibido sea el firmado (la cabecera `x-SHA256` no
  se usa: en la LAN se puede falsificar). Después `esp_ota_end` →
  `esp_ota_set_boot_partition` → `esp_restart()`.
- **Confirmación:** la imagen nueva arranca en `PENDING_VERIFY` y solo se
  confirma tras una subida 2xx (`ota_confirm_if_pending()`). Si no, el
  bootloader vuelve a la anterior en el siguiente arranque.
- **`ota_rej`:** la imagen anterior, al ver la ranura descartada
  (`esp_ota_get_last_invalid_partition`), calcula el SHA-256 de sus primeros
  `size` bytes; si es el del `.bin` ofrecido, apunta esa versión en
  `ota_rej` y no la vuelve a descargar (sin esto: descarga y rollback cada
  hora). Es la única escritura del cliente en la NVS (regla 6).
- **Autotest:** en cada `ESP_RST_POWERON` se verifica el vector de la nube
  (`cloud-service/src/test/resources/firmware/signature-test-vector.json`):
  v4 válida y la misma firma como v5 rechazada. Log:
  `OTA: autotest de firma OK`.
- **Memoria RTC entre builds:** cada compilación coloca las `RTC_DATA_ATTR`
  en otras direcciones, y tras un OTA o un rollback el arranque llega como
  despertar de deep sleep. `s_rtc_owner` guarda 4 bytes del SHA-256 del ELF;
  si no coincide, la memoria RTC se trata como tras un arranque en frío
  (`rtc_reason`). Al Fog viaja siempre el `reset_reason` real.
- **Banco:** `tools/ota_test` empuja un `.bin` a mano (sin firma) para
  probar el rollback sin el Fog.

## Fotos atrasadas (buffer en la flash interna)

Si la foto del ciclo no se puede subir (sin WiFi, error de red o 5xx), se
guarda en la partición `spiffs` de Minimal SPIFFS (`0x3D0000`, 128 KB) con
LittleFS, y se sube en el siguiente ciclo con conexión. Contrato con el Fog
y la nube, y sus cambios: `docs/handoff-buffer-fotos-offline.md`.

- **Capacidad: 1–3 fotos, 5–15 min de corte.** LittleFS asigna bloques de
  4 KB: una foto de 30 KB ocupa 8, una de 40 KB 10 y una de 70 KB 18, y
  quedan ~27 libres. Si se llena, se descarta la más antigua.
- **Orden de subida:** primero la foto actual (sin `captured_at`); si da 2xx,
  hasta `BACKLOG_MAX_SEND_PER_CYCLE` (3) atrasadas, de la más antigua a la
  más nueva. 2xx o 4xx → se borra (un 4xx no va a cambiar); error de red o
  5xx → se conserva y se para.
- **Hora:** no hay RTC con pila. `clock_module` fija el reloj con la
  cabecera `Date` de cada 2xx del Fog; sigue contando en deep sleep con el
  RC de 150 kHz (deriva ≤ ~5 %). En la siguiente sincronización mide la
  deriva real y corrige (renombrando) las fotos fechadas entre medias. Sin
  sincronización desde el último arranque en frío **no se guarda nada**:
  una foto sin hora no se puede fechar.
- **Formato:** `/q1/<epoch de 10 cifras>.bin` = cabecera de 32 B
  (`BacklogHeader`) + JPEG. La hora va en el nombre para corregirla con un
  rename. Se escribe en `/q1/tmp` y se renombra al terminar; un formato
  nuevo = directorio nuevo (`/q2`), nunca cambiar `BacklogHeader`.
- **Coste:** un contador en `RTC_DATA_ATTR` evita montar LittleFS en los
  ciclos sin pendientes. El primer montaje formatea la partición (~1,5 s,
  una vez). Antes de escribir se apagan cámara y WiFi (pico de corriente).
- **Desgaste:** solo se escribe cuando falla una subida; en el peor caso
  (corte permanente), ~3,8 años hasta los 100 000 ciclos por sector.
- La partición es distinta de la NVS y de `app0`/`app1`: ni el OTA ni el
  rollback la tocan. `{"cmd":"erase"}` también borra las fotos pendientes.
- **Dependencia de la nube:** sin que la nube compare `occurredAt` (HU-C1
  del handoff), una foto atrasada sobrescribe el estado actual de la plaza.
  No poner este firmware en campo antes de que eso esté desplegado.

## Convenciones

- **Comentarios en español sin tildes** en el código (`.ino`, `.h`, `.cpp`):
  evita problemas de codificación en el monitor serie y en el IDE.
- Markdown (README, este fichero): español **con** tildes.
- Los comentarios explican **por qué**, no qué. Mucho de este código tiene
  decisiones contraintuitivas por las restricciones del hardware.
- Logs con el macro `LOG()` de `config.h`, prefijados por subsistema:
  `CAM:`, `WIFI:`, `UPLOAD:`, `OTA:`, `NVS:`, `PROV:`, `POWER:`, `BAT:`,
  `CLOCK:`, `BACKLOG:`, `SLEEP:`.
  Las respuestas del protocolo de aprovisionamiento (`PROV: READY/OK/ERROR`)
  no usan `LOG()` sino `Serial` directo.

## Contrato de la API (Raspberry Pi / fog node)

```
POST http://<fogHost>:<fogPort>/api/v1/camera/frames   → 202 Accepted     (host/puerto: NVS o defecto)
Headers:  X-API-Key, X-API-Token
Body:     multipart/form-data, campo "file" = JPEG + campos de telemetría (texto)
```

Fuente de verdad: `fog-application/src/vision/camera_router.py` (el
proveedor), no esta documentación.

- `camera` en **singular** y **sin id**: el Fog resuelve qué cámara es a
  partir de `X-API-Key`/`X-API-Token`. `/api/v1/cameras/<id>/frames` existe en
  el Fog, pero solo como `GET` de administración (un `POST` ahí da `405`).
- El `camera.id` aprovisionado (`CAM-001`…) no viaja en la petición: solo
  sale en la línea `NVS:` del arranque para saber qué unidad es.
- Responde **`202 Accepted`** (la detección se procesa después de responder);
  el firmware trata cualquier 2xx como éxito.
- Es HTTP plano (red local). Si el ESP32 no resuelve `.local` en estación,
  aprovisionar la placa con la IP de la Pi en `fogHost` — es un cambio de la
  NVS, no de código ni de `config.h`. El host **no** se actualiza desde la
  nube: si llegara uno equivocado, la cámara ya no podría recibir la
  corrección.

## Migración (contexto histórico)

Este firmware era antes un **servidor** con dashboard en vivo (streaming
MJPEG, foto cada 5 s) y se migró a cliente REST con deep sleep para tesis.
Se eliminaron `dashboard_html.h` y `web_server_module.*`, y con ellos el modo
AP, los endpoints HTTP del propio ESP32 y el modo de vídeo. El detalle
completo de la decisión (arquitectura antes/después, credenciales, checklist)
vivió en un documento de migración temporal en el escritorio del usuario, ya
aplicado.

## Seguridad

Las credenciales están fuera del control de versiones: solo en
`provision.local.json` (WiFi, credencial de la API y host del Fog),
gitignored. Ni la WiFi ni la credencial están en el binario: se comprueba
buscando el SSID y la `apiKey` en `build/esp32cam.ino.bin`. **Nunca
escribirlas en `config.h`, en el README ni en un mensaje de commit**, y no
leer ni copiar `provision.local.json` (ni un `secrets.h` antiguo que quede
en disco) desde una sesión de agente.

La NVS de la placa guarda la contraseña WiFi y el token de la API en claro (sin cifrado de NVS ni
de flash): quien tenga la placa y un USB puede leerla. Es aceptable para el
proyecto, pero conviene saberlo.

La subida a la Pi es HTTP plano sin TLS: aceptable en la red local del
proyecto (`ParkVisionFog1`), no exponerla a Internet.

## Cómo trabaja el usuario

Habla español; responder en español. Es una tesis de la UPC, así que el rigor
técnico importa más que la rapidez: prefiere entender los trade-offs (batería,
pines, precisión de medida) a que se le den cosas hechas sin explicación.

Corregir de frente cuando una premisa suya sea incorrecta — ya pasó con
GPIO21/22 y con «4 pilas son más que suficiente». Agradece los números
concretos.
