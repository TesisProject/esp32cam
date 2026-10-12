# ESP32-CAM Live Monitor — cliente REST con deep sleep

Firmware para **ESP32-CAM AI-Thinker**, proyecto de tesis UPC ("ParkVision").
Cada ciclo el módulo captura una foto, la sube por `POST` a una Raspberry Pi
local ("fog node") que detecta plazas de parking y reenvía el resultado como
JSON a la nube, y entre captura y captura se apaga casi por completo (deep
sleep) para maximizar la autonomía con pilas AA.

**Sin dashboard. Sin servidor HTTP local. Sin librerías externas** salvo
`HTTPClient` (viene con el core ESP32). Un sketch, un flasheo por unidad.

```
ESP32-CAM  ─ HTTP POST (JPEG) ─→  Raspberry Pi (fog node)  ─→  nube
   │
   ├─ 5V ←── pack de 4 pilas AA
   └─ SCCB ──→ OV2640
```

Hay **3 unidades físicas** (`CAM-001`, `CAM-002`, `CAM-003`), mismo código
fuente, cada una con su propio `secrets.h` antes de flashearla.

---

## 1. Configuración elegida

| Parámetro | Valor |
|---|---|
| Pack de batería | **4 × AA alcalinas en serie**, sin medida de tensión |
| microSD | **No se usa** |
| Red | **Solo estación** — necesita el router para llegar a la Pi; el modo AP impide dormir |
| Intervalo de captura | **5 minutos** (`CAPTURE_INTERVAL_MS`), deep sleep entre medio |
| Subida | `POST` multipart a `http://<FOG_SERVER_HOST>:<FOG_SERVER_PORT>/api/v1/camera/frames` (sin id: el Fog identifica la cámara por `X-API-Key`/`X-API-Token`); responde `202 Accepted` |

Todo esto vive en `config.h`, salvo las credenciales (`secrets.h`, ver §2).

---

## 2. Estructura del sketch

```
esp32cam/
├── esp32cam.ino                Orquesta el ciclo: setup() = un ciclo completo -> deep sleep.
├── config.h                    TODA la configuración (pines, umbrales, endpoint…)
├── secrets.h                   WiFi + DEVICE_ID/API_KEY/API_TOKEN — NO se versiona
├── secrets.h.example           Plantilla de secrets.h
├── src/
│   ├── camera/camera_module.*  OV2640: init, captura, PWDN + hold para deep sleep
│   ├── wifi/wifi_module.*      Conexión en modo estación (timeout, MAC, IP fija)
│   ├── upload/upload_module.*  POST multipart/form-data (foto + telemetría) a la Raspberry Pi
│   └── ota/ota_module.*        FW_VERSION y partición en el log; rollback de imágenes OTA
└── README.md
```

Arduino compila los ficheros de la raíz del sketch y, **recursivamente, todo
lo que haya dentro de `src/`**. Cualquier otra subcarpeta se ignora, así que
un módulo nuevo va en `src/<nombre>/`. Desde el `.ino` se incluyen con la
ruta (`#include "src/wifi/wifi_module.h"`) y los módulos llegan a `config.h`
con `#include "../../config.h"`.

`config.h` y `secrets.h` se quedan en la raíz a propósito: son lo único que
se toca para configurar o flashear cada unidad.

> El Arduino IDE 2.x solo muestra como pestañas los ficheros de la raíz: los
> de `src/` compilan igual, pero se editan desde VS Code u otro editor.

> **La carpeta debe llamarse igual que el `.ino`.** Si la renombras, renombra
> también el sketch, o el IDE no lo abrirá.

### Un ciclo completo

No hay `loop()` real: cada despertar del deep sleep ejecuta `setup()` de
principio a fin y termina en `esp_deep_sleep_start()`, que no retorna.

1. Inicializar la cámara.
2. Conectar WiFi en modo estación, con timeout (`WIFI_CONNECT_TIMEOUT_MS`).
3. Capturar 1 frame JPEG y subirlo por `POST` a la Pi, con la telemetría
   del ciclo en el mismo multipart (ver abajo). Si la respuesta es 2xx y la
   imagen en ejecución viene de un OTA sin confirmar, confirmarla.
4. Apagar la cámara (PWDN + `rtc_gpio_hold_en`), apagar WiFi, dormir
   `CAPTURE_INTERVAL_MS` y repetir.

Si falla la cámara o el WiFi, el ciclo se aborta y el dispositivo duerme
igual — no se queda colgado reintentando, que gasta batería con pilas.

### Telemetría que acompaña a cada foto

Va como partes de texto del mismo `POST` (no gasta radio extra). Los nombres
son los que acepta el Fog (`fog-application/src/vision/camera_router.py`),
que descarta sin fallar cualquier valor que no sepa leer.

| Campo | Contenido |
|---|---|
| `fw_version` | `FW_VERSION` de `config.h` |
| `rssi` | señal WiFi, dBm |
| `wifi_connect_ms` | tiempo hasta asociarse al router, ms |
| `reset_reason` | `esp_reset_reason()`: `8` = deep sleep, `9` = brownout |
| `awake_ms` | ms despierto hasta el `POST` (sin contar ROM/bootloader) |
| `free_heap` | RAM interna libre, bytes |
| `wake_count` | despertares desde el último arranque en frío |
| `wifi_fail_streak` | ciclos seguidos sin WiFi antes de este |
| `framesize` / `jpeg_quality` | resolución y calidad aplicadas de verdad (`QVGA`/15 si falta PSRAM) |

No se envían los campos de batería que el Fog todavía declara
(`battery_mv`, `battery_load_mv`, `battery_valid`): ya no hay medida de
batería.

---

## 3. Pines ocupados

**GPIO21 y GPIO22 NO están libres en la AI-Thinker.** En esta placa `GPIO21 = Y5`
(bit de datos de la cámara) y `GPIO22 = PCLK`. Están cableados al OV2640.

El OV2640 ocupa: `0, 5, 18, 19, 21, 22, 23, 25, 26, 27, 32, 34, 35, 36, 39`.
GPIO33 lleva el LED rojo integrado (LED de estado, `STATUS_LED_PIN`).

> **Si en el futuro añades algo por GPIO**: GPIO12 es el strapping MTDI y un
> pull-up ahí impide arrancar el chip. GPIO14/15 están libres mientras no se
> use la microSD.

---

## 4. Cableado

### 4.1 Alimentación

La ESP32-CAM tiene un LDO **AMS1117** de 5 V → 3,3 V. Con 4 pilas AA en serie
(hasta 6,4 V con pilas nuevas) entrando al pin `5V`, el LDO tira a calor todo
lo que sobra por encima de 3,3 V. Pon un **condensador de 470–1000 µF** lo más
cerca posible del pin de alimentación: los picos de TX del WiFi son de
300–500 mA durante microsegundos y son la causa nº 1 de reinicios por
brownout con pilas.

### 4.2 GPIO32 (PWDN) durante el deep sleep

El deep sleep del ESP32 apaga la CPU pero **no** corta la alimentación de la
cámara: comparte el mismo rail de 3,3 V. Antes de dormir, el firmware pone
PWDN (GPIO32) en alto para apagar el sensor y usa `rtc_gpio_hold_en()` para
que ese nivel se mantenga durante todo el deep sleep (GPIO32 es RTC-capable).
Al despertar, `camera_init()` suelta el hold con `rtc_gpio_hold_dis()` antes
de reconfigurar el pin.

---

## 5. Autonomía con deep sleep

A diferencia de un dispositivo que sirve un dashboard permanentemente (donde
el deep sleep es inviable porque se pierde la asociación WiFi), aquí **no hay
nada que servir entre capturas**: el dispositivo despierta, sube una foto y
vuelve a dormir.

| Estado | Corriente aproximada | Duración por ciclo |
|---|---|---|
| Deep sleep | microamperios | ~5 min (`CAPTURE_INTERVAL_MS`) |
| Despierto: cámara + WiFi + POST | ~150–200 mA | 2–5 s típico |

Con un ciclo de ~3 s despierto cada 5 minutos, el consumo medio está dominado
casi por completo por la corriente de deep sleep (microamperios), muy por
debajo de los ~85 mA continuos de la arquitectura anterior con servidor
siempre disponible. La autonomía real depende sobre todo de la autodescarga
de las pilas alcalinas más que del consumo activo del ESP32.

**Para pruebas de campo**: bajar `CAPTURE_INTERVAL_MS` a 30000 (30 s) para no
esperar 5 minutos entre cada verificación, y devolverlo a 300000 antes de
dejar la cámara desatendida con pilas.

---

## 6. Compilar y grabar con el Arduino IDE

### 6.1 Requisitos

#### Credenciales

`config.h` **no contiene las credenciales**: están en `secrets.h`, que git
ignora, para poder publicar el repositorio sin regalar el WiFi ni las claves
de la API.

Si acabas de clonar el proyecto, ese fichero no existe y la compilación falla
a propósito con un mensaje claro. Créalo desde la plantilla:

```powershell
cd esp32cam
copy secrets.h.example secrets.h
```

Y rellena los cinco valores: `WIFI_SSID`, `WIFI_PASSWORD`, `DEVICE_ID`,
`API_KEY` y `API_TOKEN`. La red del router debe ser de **2,4 GHz** — el ESP32
no ve las de 5 GHz. `DEVICE_ID`/`API_KEY`/`API_TOKEN` son distintos **para
cada una de las 3 unidades** (`CAM-001`/`CAM-002`/`CAM-003`): confirmar cuál
se está flasheando antes de subir el sketch.

#### Soporte de placas

Solo el soporte de placas ESP32 de Espressif:

> Archivo → Preferencias → *Gestor de URLs Adicionales de Tarjetas*:
> `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
> y luego Herramientas → Placa → Gestor de Tarjetas → buscar **esp32**.

**Ninguna librería que instalar.** `HTTPClient` viene con el core ESP32. Si el
Gestor de Librerías te pide algo, es que falta un fichero del sketch en la
carpeta.

### 6.2 Ajustes en Herramientas

| Opción | Valor |
|---|---|
| Placa | **AI Thinker ESP32-CAM** |
| Partition Scheme | **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)** |
| PSRAM | **Enabled** |
| Upload Speed | 921600 (baja a 115200 si falla) |
| Puerto | el COM/tty de tu adaptador USB-serie |

**Por qué Minimal SPIFFS y no Huge APP** (el valor por defecto de esta placa):
Huge APP tiene una sola partición de app de 3 MB, sin sitio para una segunda
versión, así que no permite OTA. Minimal SPIFFS tiene dos de 1 966 080 bytes
(`app0`/`app1`): el firmware actual (~1,13 MB) ocupa el **57 %** de cada una.
*Default* (1,25 MB por partición) dejaría ~86 %, demasiado justo para crecer.

Cambiar de esquema reescribe la tabla de particiones, cosa que un OTA no
puede hacer: cada placa se flashea por USB **una vez** con el esquema nuevo
y a partir de ahí ya podría actualizarse por WiFi.

Con `arduino-cli` el esquema va en el FQBN (las tareas de VS Code ya lo
llevan):

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32cam:PartitionScheme=min_spiffs --build-path build .
```

**`FW_VERSION`** (`config.h`) es un entero (1, 2, 3…): súbelo en cada
versión que vayas a grabar en otra placa. Sale en el log de arranque junto
a la partición activa y se envía al Fog en cada foto.

### 6.3 Conexión del programador

La ESP32-CAM AI-Thinker **no tiene USB**: necesitas un adaptador USB-serie
(FTDI, CP2102…) a **3,3 V de lógica**, o una placa base ESP32-CAM-MB.

| FTDI | ESP32-CAM |
|---|---|
| `5V` | `5V` |
| `GND` | `GND` |
| `TX` | `U0R` (GPIO3) |
| `RX` | `U0T` (GPIO1) |
| — | **`IO0` ↔ `GND`** ← puente sólo para grabar |

Secuencia: puente `IO0`–`GND` → pulsar **RST** → *Subir* en el IDE → al terminar,
**quitar el puente** y pulsar RST otra vez.

Con la placa base ESP32-CAM-MB nada de esto hace falta: se conecta por USB y ya.

Si el FTDI no da bastante corriente, alimenta la placa aparte y deja sólo
TX/RX/GND al FTDI. **Masa común siempre.**

### 6.4 Subir

Abre `esp32cam.ino` (doble clic) y pulsa **Subir**.

### 6.5 Monitor serie

Herramientas → Monitor Serie, a **115200 baudios**. Ejemplo de un ciclo
completo con `CAPTURE_INTERVAL_MS` bajado a 30 s para pruebas:

```
[     210] === ESP32-CAM ParkVision (CAM-001) ===
[     211] OTA: FW_VERSION=1  particion activa=app0  estado=UNDEFINED (flasheo USB)
[     212] Reset reason: 8  wake_count=12  wifi_fail_streak=0
[     620] CAM: lista (800x600, PSRAM=si)
[     720] WIFI: MAC=24:6F:28:XX:XX:XX
[     720] WIFI: conectando a "ParkVisionFog1"...
[    4380] WIFI: conectado en 3660 ms. IP=192.168.1.42  RSSI=-58 dBm
[    4645] UPLOAD: telemetria fw=1 rssi=-58 wifi_ms=3660 reset=8 awake_ms=4640 heap=172000 wake=12 wifi_fails=0 SVGA q=12
[    4650] UPLOAD: POST http://parkvision.local:8000/api/v1/camera/frames (42050 bytes)
[    5210] UPLOAD: respuesta HTTP 202
[    5210] UPLOAD: ok (202)
[    5215] CAM: sensor apagado (PWDN) y retenido para deep sleep
[    5220] SLEEP: durmiendo 30000 ms
```

- `Reset reason: 8` es el despertar normal del deep sleep; `1` es encendido
  y `9` brownout (pilas o condensador). Con cualquier valor distinto de `8`,
  `wake_count` vuelve a 0.
- Sin PSRAM, `CAM:` avisa de que se degrada a QVGA y aparece
  `UPLOAD: body en RAM interna (... bytes, sin PSRAM)`: la foto se sube
  igual, pero a 320×240 la detección en el Fog pierde precisión.
- Tras un OTA (todavía no implementado) el estado sería `PENDING_VERIFY` y,
  después de la primera subida buena, `OTA: imagen de app1 confirmada`.

---

## 7. Ajustes frecuentes en `config.h`

| Quiero… | Toca |
|---|---|
| Más autonomía / menos tráfico | subir `CAPTURE_INTERVAL_MS` |
| Probar sin esperar 5 min | bajar `CAPTURE_INTERVAL_MS` a 30000 (volver a subirlo después) |
| Imagen más nítida | `CAM_JPEG_QUALITY_SNAPSHOT 10`, `CAM_FRAMESIZE_SNAPSHOT FRAMESIZE_UXGA` |
| Cámara del revés | `CAM_VFLIP 1` y/o `CAM_HMIRROR 1` |
| IP siempre la misma | `WIFI_USE_STATIC_IP 1` + los cuatro `WIFI_STATIC_*` |
| La Pi no resuelve por `.local` | cambiar `FOG_SERVER_HOST` por su IP directamente |
| Reinicios por brownout con pilas | primero el condensador; si persiste, `POWER_DISABLE_BROWNOUT 1` |

Después de tocar `config.h`, **vuelve a subir el sketch**: es código compilado.

---

## 8. Problemas típicos

| Síntoma | Causa habitual |
|---|---|
| `#error "Falta secrets.h..."` | No has creado `secrets.h`, o le falta algún valor. Copia `secrets.h.example` y rellénalo (§6.1). |
| `Sketch too big` al compilar | Comprobar que el *Partition Scheme* es **Minimal SPIFFS** (con *Default* solo hay 1,25 MB). No pasar a Huge APP: no tiene segunda partición y anula el OTA. |
| `esp_camera_init fallo (0x105)` | Cable plano mal insertado o alimentación insuficiente. Prueba con `CAM_XCLK_FREQ_HZ 10000000`. |
| `Camera probe failed` o cuelgue al arrancar | PSRAM sin habilitar en Herramientas. |
| `WIFI: sin conexion tras ... ms` | Router fuera de rango, SSID/password mal en `secrets.h`, o red de 5 GHz (el ESP32 no la ve). |
| `UPLOAD: fallo la conexion` | La Raspberry Pi está apagada/inalcanzable, o `FOG_SERVER_HOST`/`FOG_SERVER_PORT` no coinciden. El firmware agota el timeout y duerme igual. |
| `UPLOAD: fallo (401)` / `(403)` | `API_KEY`/`API_TOKEN` incorrectos, o mezclados entre cámaras (el Fog identifica la cámara solo por ellos). |
| `UPLOAD: fallo (405)` | La URL apunta a `/api/v1/cameras/<id>/frames` (solo `GET` de administración en el Fog); la buena es `/api/v1/camera/frames`. |
| Dos cámaras subiendo como la misma (mismas fotos mezcladas en el Fog) | Comparten `API_KEY`/`API_TOKEN`. Revisar qué `secrets.h` se subió a cada unidad antes de flashear; el `DEVICE_ID` del log de arranque ayuda a identificarla. |
| `Failed to connect… Timed out waiting for packet header` | Falta el puente `IO0`–`GND`, o no pulsaste RST antes de subir. |
| Reinicios al conectar el WiFi | Falta el condensador de 470–1000 µF; o el FTDI no da corriente suficiente. |
| La placa se queda en reset con `arduino-cli monitor` abierto | DTR/RTS activos; usar `--config baudrate=115200,dtr=off,rts=off` (la tarea de VS Code ya lo hace). |

---

## 9. El sketch anterior

`esp32cam_fotos_stream.ino` (fotos a microSD + streaming) está archivado en
`../legacy/`. La versión con dashboard en vivo (streaming MJPEG, servidor HTTP
propio) fue la primera iteración de este mismo proyecto y ya no está: se
migró a esta arquitectura de cliente + deep sleep para la integración con la
Raspberry Pi de ParkVision.
