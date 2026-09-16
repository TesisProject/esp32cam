# CLAUDE.md

Contexto para Claude Code al trabajar en este repositorio.

## Qué es

Firmware para **ESP32-CAM AI-Thinker** que sirve un dashboard web con la cámara
en vivo y el nivel de batería. El dispositivo es **autosuficiente**: no hay
Raspberry Pi, ni servidor Python, ni servicios externos. Proyecto de tesis.

El proyecto activo es `esp32cam_live_monitor/`. En `legacy/` está el sketch
anterior (fotos a microSD), archivado y sin desarrollo.

## Build

**Arduino IDE**, no PlatformIO (se convirtió deliberadamente: el usuario no
tiene PlatformIO instalado y el IDE ya lo tenía). No hay toolchain ESP32 en esta
máquina, así que **no se puede compilar desde aquí** — la verificación la hace
el usuario pulsando *Verificar* en el IDE.

Ajustes obligatorios en Herramientas:

| Opción | Valor |
|---|---|
| Placa | AI Thinker ESP32-CAM |
| Partition Scheme | **Huge APP (3MB No OTA/1MB SPIFFS)** |
| PSRAM | **Enabled** |

Con el esquema de particiones por defecto el binario no cabe (`Sketch too big`).

**Sin librerías externas.** Solo el core ESP32 de Espressif. El driver del
ADS1115 está escrito a mano en `battery_module.cpp` y el dashboard va empotrado
en `dashboard_html.h`. No añadir dependencias del Gestor de Librerías sin
preguntar: fue un requisito explícito del usuario.

## Estructura

```
esp32cam_live_monitor/          ← carpeta = nombre del .ino (lo exige el IDE)
├── esp32cam_live_monitor.ino   setup()/loop(), WiFi, energía, LED de estado
├── config.h                    TODA la configuración (menos credenciales)
├── secrets.h                   Credenciales WiFi — gitignored, no versionar
├── secrets.h.example           Plantilla de secrets.h
├── camera_module.h / .cpp      OV2640
├── battery_module.h / .cpp     ADS1115 + curvas de descarga
├── web_server_module.h / .cpp  esp_http_server
└── dashboard_html.h            HTML+CSS+JS del dashboard, en PROGMEM
```

El Arduino IDE compila todos los `.cpp`/`.h` de la carpeta del sketch.

## Invariantes del diseño

Estas reglas se acordaron con el usuario. No romperlas sin avisar.

**1. Toda la configuración vive en `config.h`.** Ningún otro fichero contiene
pines, umbrales, intervalos ni resoluciones. Si hace falta un valor nuevo, se
define ahí.

*Única excepción, deliberada*: las cuatro credenciales WiFi (`WIFI_SSID`,
`WIFI_PASSWORD`, `WIFI_AP_SSID`, `WIFI_AP_PASSWORD`) están en `secrets.h`, que
git ignora. `config.h` lo incluye con `__has_include` y lanza un `#error` claro
si falta. Al añadir una credencial nueva, va en `secrets.h` **y** en
`secrets.h.example`.

**2. El dashboard no duplica configuración.** `dashboard_html.h` pide `/config`
al arrancar y de ahí saca intervalos, rutas, modo de vídeo, puerto del stream y
umbrales de batería. Nunca meter constantes en el JS: añadirlas al JSON de
`config_handler()`.

**3. El stream MJPEG va en su propio puerto (81).** Un handler MJPEG no termina
mientras el cliente mire, y `esp_http_server` atiende cada puerto en un solo
hilo. Compartir puerto dejaría `/battery` colgado.

**4. Sin librerías externas.** Ver arriba.

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

**Buses I2C separados.** La cámara usa SCCB en el puerto I2C 1 (pines 26/27);
`Wire` (puerto 0) queda libre para el ADS1115. No hay conflicto.

**Límite del ADS1115**: entrada analógica ≤ VDD + 0,3 V = **3,6 V**. Con 4 pilas
en serie (6,4 V) el divisor tiene que ser 220k/100k, no 100k/100k.

**`httpd_err_code_t` no tiene 503.** Solo 400, 401, 403, 404, 405, 408, 411,
414, 431, 500, 501 y 505. Un 503 se compone con `httpd_resp_set_status()`.

## Energía

El **deep sleep no es viable** con un servidor que debe responder: al despertar
se pierde la asociación WiFi y hay 2-4 s sin IP. Es incompatible por diseño.

Lo que sí se aplica: modem sleep WiFi (solo en modo estación), CPU dinámica
160/80 MHz, modo híbrido snapshot+stream bajo demanda, límite de fps.

`POWER_ACTIVE_HOLD_MS` mantiene la CPU rápida tras cualquier petición HTTP. Sin
eso, las fotos sueltas se codificaban a 80 MHz (el doble de lento) — fue un bug
real, no tocarlo a la ligera.

En **modo AP el modem sleep no existe**: un punto de acceso emite balizas sin
parar. El firmware lo detecta y lo desactiva solo.

## Convenciones

- **Comentarios en español sin tildes** en el código (`.ino`, `.h`, `.cpp`):
  evita problemas de codificación en el monitor serie y en el IDE.
- Markdown (README, este fichero): español **con** tildes.
- Los comentarios explican **por qué**, no qué. Mucho de este código tiene
  decisiones contraintuitivas por las restricciones del hardware.
- Logs con el macro `LOG()` de `config.h`, prefijados por subsistema:
  `CAM:`, `BAT:`, `WIFI:`, `WEB:`, `POWER:`, `FS:`.

## Endpoints

| Ruta | Puerto | |
|---|---|---|
| `/` | 80 | Dashboard |
| `/config` | 80 | JSON de configuración para el dashboard |
| `/battery` | 80 | Tensión, porcentaje, estado |
| `/capture` | 80 | Una foto JPEG |
| `/status` | 80 | Diagnóstico: uptime, heap, RSSI, modo de red, `cameraReady` |
| `/test` | 80 | **Diagnóstico sin JavaScript**: un `<img>` pelado |
| `/stream` | 81 | MJPEG multipart |

`/test` existe para separar «la cámara no va» de «el JS no va». Es la primera
herramienta cuando el usuario reporta que no ve imagen.

## Estado actual

**Sin resolver: el dashboard no muestra imagen.** El HTML carga, así que la red
y el servidor funcionan. Falta que el usuario reporte qué devuelve `/test` y la
línea `CAM:` del monitor serie. Sospechas: PSRAM sin habilitar, cable plano, o
alimentación insuficiente (el sensor pide ~300 mA al inicializar).

**Nunca se ha compilado ni ejecutado desde aquí.** El único feedback del
compilador fue un error de `HTTPD_503_SERVICE_UNAVAILABLE`, ya corregido. Todo
lo posterior (modo AP, `/test`, LED de estado, curvas de batería) está sin
verificar.

**Hardware de batería sin montar.** El usuario tiene 4 pilas AA. Falta confirmar
si son alcalinas o NiMH (`BATTERY_CHEMISTRY` en `config.h`, ahora en alcalina).
El ADS1115 y el divisor aún no están cableados.

**Pendiente para uso desatendido**: reintento si falla `camera_init()`,
watchdog, y reinicio preventivo programado. Están identificados y sin hacer.

## Seguridad

Las credenciales están fuera del control de versiones (`secrets.h`, gitignored).
**Nunca escribirlas en `config.h`, en el README ni en un mensaje de commit.** El
repositorio se reinició tras extraerlas, así que el historial está limpio: no
volver a ensuciarlo.

El servidor no tiene autenticación ni HTTPS. Vale para una red local o para el
AP propio; no exponerlo a Internet.

## Cómo trabaja el usuario

Habla español; responder en español. Es una tesis de la UPC, así que el rigor
técnico importa más que la rapidez: prefiere entender los trade-offs (batería,
pines, precisión de medida) a que se le den cosas hechas sin explicación.

Corregir de frente cuando una premisa suya sea incorrecta — ya pasó con
GPIO21/22 y con «4 pilas son más que suficiente». Agradece los números
concretos.
