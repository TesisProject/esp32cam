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

El proyecto activo es `esp32cam_live_monitor/`. En `legacy/` está el sketch
original (fotos a microSD), archivado y sin desarrollo.

## Build

**Arduino IDE**, no PlatformIO (se convirtió deliberadamente: el usuario no
tiene PlatformIO instalado y el IDE ya lo tenía). No hay toolchain ESP32 en esta
máquina, así que **no se puede compilar desde aquí** — la verificación la hace
el usuario pulsando *Verificar* en el IDE.

Ajustes obligatorios en Herramientas:

| Opción | Valor |
|---|---|
| Placa | AI Thinker ESP32-CAM |
| Partition Scheme | probar primero **por defecto**; si da `Sketch too big`, pasar a Huge APP |
| PSRAM | **Enabled** |

**Sin librerías externas** salvo `HTTPClient` (viene con el core ESP32, no hay
que instalarla). El driver del ADS1115 está escrito a mano en
`battery_module.cpp`. No añadir dependencias del Gestor de Librerías sin
preguntar: fue un requisito explícito del usuario.

## Estructura

```
esp32cam_live_monitor/          ← carpeta = nombre del .ino (lo exige el IDE)
├── esp32cam_live_monitor.ino   setup() = un ciclo completo -> deep sleep. No hay loop() real.
├── config.h                    TODA la configuración (menos credenciales)
├── secrets.h                   WiFi + DEVICE_ID/API_KEY/API_TOKEN — gitignored
├── secrets.h.example           Plantilla de secrets.h
├── camera_module.h / .cpp      OV2640: init, captura, PWDN + hold para deep sleep
├── battery_module.h / .cpp     ADS1115 + curvas de descarga (solo log por serie)
└── upload_module.h / .cpp      POST multipart/form-data a la Raspberry Pi
```

El Arduino IDE compila todos los `.cpp`/`.h` de la carpeta del sketch.

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
`config.h`, sección 3, y el documento de migración) solo acepta el campo
`file` en un POST multipart. No añadir batería, timestamp, etc. sin confirmar
antes con el backend.

## Hardware: restricciones que NO son obvias

Esto ya costó una corrección. No repetir los errores.

**GPIO21 y GPIO22 NO están libres.** En la AI-Thinker `GPIO21 = Y5` (dato de
cámara) y `GPIO22 = PCLK`. El OV2640 ocupa:
`0, 5, 18, 19, 21, 22, 23, 25, 26, 27, 32, 34, 35, 36, 39`.

**No hay ADC nativo usable.** La cámara se queda con todos los canales de ADC1
salvo GPIO33 (que lleva el LED rojo integrado en paralelo y falsea la lectura).
ADC2 no funciona con el WiFi encendido — limitación del silicio, no un bug.
De ahí el ADS1115 externo por I2C.

**I2C en GPIO14 (SDA) / GPIO15 (SCL).** Libres porque no se usa microSD.
GPIO15 es strapping MTDO: en alto = arranque normal, el pull-up del bus es
seguro. **GPIO12 nunca**: strapping MTDI, un pull-up ahí impide arrancar.

**GPIO32 (PWDN de la cámara) es RTC-capable.** El deep sleep del ESP32 apaga
la CPU pero **no** corta la alimentación de la cámara (comparte el rail de
3,3 V) ni conserva el nivel de los pines por defecto. Por eso
`camera_power_down()` pone PWDN en alto y llama a `rtc_gpio_hold_en()`; el
siguiente `camera_init()` debe soltar el hold con `rtc_gpio_hold_dis()` antes
de reconfigurar el pin, o `pinMode`/`digitalWrite` no tienen efecto.

**Límite del ADS1115**: entrada analógica ≤ VDD + 0,3 V = **3,6 V**. Con 4 pilas
en serie (6,4 V) el divisor tiene que ser 220k/100k, no 100k/100k.

## Energía — deep sleep real

A diferencia de la arquitectura anterior (servidor siempre disponible, deep
sleep inviable), ahora **sí** se usa deep sleep entre capturas: no hay nada
que servir entre ciclos.

- Cada ciclo: batería (log) → cámara → WiFi STA → capturar → POST → apagar
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
  `CAM:`, `BAT:`, `WIFI:`, `UPLOAD:`, `POWER:`, `SLEEP:`.

## Contrato de la API (Raspberry Pi / fog node)

```
POST http://<FOG_SERVER_HOST>:<FOG_SERVER_PORT>/api/v1/cameras/<DEVICE_ID>/frames
Headers:  X-API-Key, X-API-Token
Body:     multipart/form-data, campo "file" = JPEG
```

`DEVICE_ID` va en la URL, no en el body. Es HTTP plano (red local). Si el
ESP32 no resuelve `.local` en estación, cambiar `FOG_SERVER_HOST` por la IP
de la Pi directamente — es un cambio de config, no de código.

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
