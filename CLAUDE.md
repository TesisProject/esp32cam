# CLAUDE.md

Contexto para Claude Code al trabajar en este repositorio.

## Qué es

Firmware para **ESP32-CAM AI-Thinker**, proyecto de tesis UPC ("ParkVision").
El dispositivo es **cliente**: cada ciclo captura una foto, la sube por POST a
una Raspberry Pi local ("fog node") que detecta plazas de parking y reenvía el
resultado a la nube, y entre captura y captura se apaga casi por completo
(deep sleep) para maximizar la autonomía con pilas AA. No hay dashboard ni
servidor HTTP local: eso era la arquitectura anterior (ver "Migración" abajo).

Hay **3 unidades físicas** (`CAM-001`, `CAM-002`, `CAM-003`), mismo código
fuente, cada una con su propio `secrets.h` (`DEVICE_ID`/`API_KEY`/`API_TOKEN`)
antes de flashearla.

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
(`app0`/`app1`) de 1 966 080 B; el firmware ocupa ~1,13 MB (57 %). *Default*
(1,25 MB por ranura) dejaría ~86 %, demasiado justo. Cambiar de esquema
obliga a flashear cada placa por USB una vez (cambia la tabla de
particiones); después, el OTA no puede cambiarla. **No volver a Huge APP**
sin hablarlo: rompería el OTA y el rollback.

**OTA preparado, no implementado.** `FW_VERSION` (entero, `config.h`) sale en
el log de arranque junto a la partición activa y viaja al Fog como
`fw_version`. El rollback del bootloader está activo en el core
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`): `src/ota/ota_module.cpp`
sobrescribe `verifyRollbackLater()` (con `extern "C"`: el weak está en un `.c`
del core) para que el core no valide la imagen al arrancar, y el `.ino` llama
a `ota_confirm_if_pending()` solo tras una subida 2xx. Una imagen OTA que se
duerme sin eso vuelve sola a la anterior al despertar. Las imágenes
flasheadas por USB quedan en estado `UNDEFINED` y no se ven afectadas. **Subir
`FW_VERSION`** en cada binario que vaya a otra placa.

**Sin librerías externas** salvo `HTTPClient` (viene con el core ESP32, no hay
que instalarla). No añadir dependencias del Gestor de Librerías sin
preguntar: fue un requisito explícito del usuario.

## Estructura

```
esp32cam/                       ← carpeta = nombre del .ino (lo exigen IDE y CLI)
├── esp32cam.ino                Orquestador: setup() = un ciclo completo -> deep sleep. No hay loop() real.
├── config.h                    TODA la configuración (menos credenciales)
├── secrets.h                   WiFi + DEVICE_ID/API_KEY/API_TOKEN — gitignored
├── secrets.h.example           Plantilla de secrets.h
└── src/
    ├── camera/camera_module.*  OV2640: init, captura, PWDN + hold para deep sleep
    ├── wifi/wifi_module.*      Estación: conexión con timeout, MAC, IP fija, apagado
    ├── upload/upload_module.*  POST multipart/form-data (foto + telemetría) a la Raspberry Pi
    └── ota/ota_module.*        FW_VERSION/partición en el log, rollback (sin cliente OTA aún)
```

Arduino (IDE y CLI) compila la raíz del sketch y **solo** `src/` de forma
recursiva; cualquier otra subcarpeta se ignora. Módulos nuevos en
`src/<nombre>/<nombre>_module.*`. El sufijo `_module` evita nombres como
`wifi.h`, que en Windows (sin distinción de mayúsculas) se confunde con
`<WiFi.h>` del core. Los módulos incluyen `"../../config.h"`;
`config.h`/`secrets.h` se quedan en la raíz porque es lo que se edita al
flashear. El IDE 2.x no muestra `src/` como pestañas.

## Invariantes del diseño

Estas reglas se acordaron con el usuario. No romperlas sin avisar.

**1. Toda la configuración vive en `config.h`.** Ningún otro fichero contiene
pines, umbrales, intervalos ni resoluciones. Si hace falta un valor nuevo, se
define ahí.

*Única excepción, deliberada*: `WIFI_SSID`/`WIFI_PASSWORD` y las credenciales
por cámara (`DEVICE_ID`, `API_KEY`, `API_TOKEN`) están en `secrets.h`, que git
ignora. `config.h` lo incluye con `__has_include` y lanza un `#error` claro si
falta. Al añadir una credencial nueva, va en `secrets.h` **y** en
`secrets.h.example`.

**2. Sin librerías externas** salvo `HTTPClient` (core ESP32).

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

**No** se envían `battery_mv`, `battery_load_mv` ni `battery_valid`: el Fog
los sigue declarando, pero el firmware ya no mide la batería. Ningún campo
nuevo (timestamp, batería…) sin confirmarlo antes con el backend. El `.ino`
recoge los valores y se los pasa a `upload_frame()` en un `UploadTelemetry`
(`upload_module.h`); el módulo de subida solo los serializa.

## Hardware: restricciones que NO son obvias

Esto ya costó una corrección. No repetir los errores.

**GPIO21 y GPIO22 NO están libres.** En la AI-Thinker `GPIO21 = Y5` (dato de
cámara) y `GPIO22 = PCLK`. El OV2640 ocupa:
`0, 5, 18, 19, 21, 22, 23, 25, 26, 27, 32, 34, 35, 36, 39`.

**Sin medida de batería.** Se quitó el ADS1115 (y su driver) a petición del
usuario. Si vuelve: no hay ADC nativo usable (la cámara ocupa ADC1 salvo
GPIO33, que lleva el LED rojo en paralelo; ADC2 no funciona con WiFi), así que
haría falta un ADC externo. GPIO14/15 están libres (no hay microSD).
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

## Energía — deep sleep real

A diferencia de la arquitectura anterior (servidor siempre disponible, deep
sleep inviable), ahora **sí** se usa deep sleep entre capturas: no hay nada
que servir entre ciclos.

- Cada ciclo: cámara → WiFi STA → capturar → POST → apagar
  cámara (PWDN + hold) → WiFi off → `esp_deep_sleep_start()`.
- `WIFI_CONNECT_TIMEOUT_MS` acota el intento de conexión: si la Pi o el router
  no responden, el ciclo se aborta y se duerme igual (no reintentar
  indefinidamente, gasta batería con pilas).
- Solo modo estación: un AP propio obligaría a emitir balizas sin parar y la
  radio no podría dormir nunca.
- `CAPTURE_INTERVAL_MS` en `config.h` (5 min en campo; bajarlo a 30 s solo
  para pruebas, y devolverlo antes de dejar la cámara desatendida).

## Convenciones

- **Comentarios en español sin tildes** en el código (`.ino`, `.h`, `.cpp`):
  evita problemas de codificación en el monitor serie y en el IDE.
- Markdown (README, este fichero): español **con** tildes.
- Los comentarios explican **por qué**, no qué. Mucho de este código tiene
  decisiones contraintuitivas por las restricciones del hardware.
- Logs con el macro `LOG()` de `config.h`, prefijados por subsistema:
  `CAM:`, `WIFI:`, `UPLOAD:`, `OTA:`, `POWER:`, `SLEEP:`.

## Contrato de la API (Raspberry Pi / fog node)

```
POST http://<FOG_SERVER_HOST>:<FOG_SERVER_PORT>/api/v1/camera/frames   → 202 Accepted
Headers:  X-API-Key, X-API-Token
Body:     multipart/form-data, campo "file" = JPEG + campos de telemetría (texto)
```

Fuente de verdad: `fog-application/src/vision/camera_router.py` (el
proveedor), no esta documentación.

- `camera` en **singular** y **sin id**: el Fog resuelve qué cámara es a
  partir de `X-API-Key`/`X-API-Token`. `/api/v1/cameras/<id>/frames` existe en
  el Fog, pero solo como `GET` de administración (un `POST` ahí da `405`).
- `DEVICE_ID` no viaja en la petición: solo sale en el log de arranque para
  saber qué unidad se ha flasheado.
- Responde **`202 Accepted`** (la detección se procesa después de responder);
  el firmware trata cualquier 2xx como éxito.
- Es HTTP plano (red local). Si el ESP32 no resuelve `.local` en estación,
  cambiar `FOG_SERVER_HOST` por la IP de la Pi directamente — es un cambio de
  config, no de código.

## Migración (contexto histórico)

Este firmware era antes un **servidor** con dashboard en vivo (streaming
MJPEG, foto cada 5 s) y se migró a cliente REST con deep sleep para tesis.
Se eliminaron `dashboard_html.h` y `web_server_module.*`, y con ellos el modo
AP, los endpoints HTTP del propio ESP32 y el modo de vídeo. El detalle
completo de la decisión (arquitectura antes/después, credenciales, checklist)
vivió en un documento de migración temporal en el escritorio del usuario, ya
aplicado.

## Seguridad

Las credenciales están fuera del control de versiones (`secrets.h`, gitignored).
**Nunca escribirlas en `config.h`, en el README ni en un mensaje de commit.**

La subida a la Pi es HTTP plano sin TLS: aceptable en la red local del
proyecto (`ParkVisionFog1`), no exponerla a Internet.

## Cómo trabaja el usuario

Habla español; responder en español. Es una tesis de la UPC, así que el rigor
técnico importa más que la rapidez: prefiere entender los trade-offs (batería,
pines, precisión de medida) a que se le den cosas hechas sin explicación.

Corregir de frente cuando una premisa suya sea incorrecta — ya pasó con
GPIO21/22 y con «4 pilas son más que suficiente». Agradece los números
concretos.
