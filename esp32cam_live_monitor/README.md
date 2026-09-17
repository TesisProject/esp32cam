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
   ├─ I2C ──→ ADS1115 ─ divisor ─→ pack de 4 pilas AA
   └─ SCCB ──→ OV2640
```

Hay **3 unidades físicas** (`CAM-001`, `CAM-002`, `CAM-003`), mismo código
fuente, cada una con su propio `secrets.h` antes de flashearla.

---

## 1. Configuración elegida

| Parámetro | Valor |
|---|---|
| Pack de batería | **4 × AA alcalinas en serie** (`BATTERY_CHEMISTRY`) |
| microSD | **No se usa** (libera GPIO14/15 para I2C) |
| Medida de batería | ADS1115 por I2C + divisor resistivo **220 k / 100 k** (solo para el log por serie) |
| Red | **Solo estación** — necesita el router para llegar a la Pi; el modo AP impide dormir |
| Intervalo de captura | **5 minutos** (`CAPTURE_INTERVAL_MS`), deep sleep entre medio |
| Subida | `POST` multipart a `http://<FOG_SERVER_HOST>:<FOG_SERVER_PORT>/api/v1/cameras/<DEVICE_ID>/frames` |

Todo esto vive en `config.h`, salvo las credenciales (`secrets.h`, ver §2).

---

## 2. Estructura del sketch

El Arduino IDE compila **todos** los `.cpp` y `.h` que estén en la carpeta del
sketch, y los muestra como pestañas:

```
esp32cam_live_monitor/
├── esp32cam_live_monitor.ino   setup() = un ciclo completo -> deep sleep. No hay loop() real.
├── config.h                    TODA la configuración (pines, umbrales, endpoint…)
├── secrets.h                   WiFi + DEVICE_ID/API_KEY/API_TOKEN — NO se versiona
├── secrets.h.example           Plantilla de secrets.h
├── camera_module.h / .cpp      OV2640: init, captura, PWDN + hold para deep sleep
├── battery_module.h / .cpp     ADS1115: driver I2C propio + curva de descarga
├── upload_module.h / .cpp      POST multipart/form-data a la Raspberry Pi
└── README.md
```

> **La carpeta debe llamarse igual que el `.ino`.** Si la renombras, renombra
> también el sketch, o el IDE no lo abrirá.

### Un ciclo completo

No hay `loop()` real: cada despertar del deep sleep ejecuta `setup()` de
principio a fin y termina en `esp_deep_sleep_start()`, que no retorna.

1. Leer batería (solo para el log por serie; el endpoint no acepta ese dato).
2. Inicializar la cámara.
3. Conectar WiFi en modo estación, con timeout (`WIFI_CONNECT_TIMEOUT_MS`).
4. Capturar 1 frame JPEG y subirlo por `POST` a la Pi.
5. Apagar la cámara (PWDN + `rtc_gpio_hold_en`), apagar WiFi, dormir
   `CAPTURE_INTERVAL_MS` y repetir.

Si falla la cámara o el WiFi, el ciclo se aborta y el dispositivo duerme
igual — no se queda colgado reintentando, que gasta batería con pilas.

---

## 3. ¿Por qué un ADC por I2C y no un divisor a un pin nativo?

Esto merece explicación porque es la parte contraintuitiva del hardware.

**GPIO21 y GPIO22 NO están libres en la AI-Thinker.** En esta placa `GPIO21 = Y5`
(bit de datos de la cámara) y `GPIO22 = PCLK`. Están cableados al OV2640. El bus
I2C hay que llevarlo a otros pines.

El OV2640 ocupa: `0, 5, 18, 19, 21, 22, 23, 25, 26, 27, 32, 34, 35, 36, 39`.

Eso deja el ADC en una situación mala:

- **ADC1** tiene los canales en GPIO32/33/34/35/36/39. La cámara se queda con
  todos **menos GPIO33**.
- **GPIO33** lleva el **LED rojo integrado** de la placa en paralelo. Ese LED y
  su resistencia falsean la lectura del divisor: para usarlo hay que desoldar o
  cortar la pista de la resistencia del LED.
- **ADC2** (GPIO0/2/4/12/13/14/15/25/26/27) es **inutilizable con el WiFi
  encendido**: el driver de radio se apodera de ese ADC y `analogRead()` devuelve
  basura o se bloquea. Esto no es un bug evitable, es una limitación del silicio.

### El trade-off, en corto

| | Divisor en GPIO33 | **ADS1115 por I2C** (elegido) |
|---|---|---|
| Coste | 2 resistencias (~0,05 €) | ~2 € el módulo |
| Modificación de la placa | **Sí**: cortar la resistencia del LED rojo | No |
| Resolución útil | ~9 bits efectivos, curva no lineal | 16 bits, lineal |
| Precisión típica | ±100–150 mV sin calibrar | ±5 mV |
| Ruido con WiFi transmitiendo | Alto (mismo silicio que la radio) | Aislado |
| Pines | GPIO33 | GPIO14 + GPIO15 |

El driver del ADS1115 está escrito en `battery_module.cpp` (unas 60 líneas sobre
`Wire`), así que **no hay que instalar ninguna librería**. Son dos registros: se
escribe la configuración, se espera 8 ms y se lee el resultado.

> **Si en el futuro añades la microSD**: en modo 1 bit ocupa GPIO2/14/15 (+GPIO4,
> compartido con el flash) y el I2C tendría que irse a **GPIO1/GPIO3**,
> sacrificando el monitor serie. **GPIO12 y GPIO13 no sirven para I2C**: GPIO12
> es el strapping MTDI y un pull-up ahí impide arrancar el chip.

---

## 4. Cableado

### 4.1 ADS1115 y divisor

```
                 ┌──────────────┐
   pack 4×AA +──┤ R_TOP  220k  ├──┬─────────────► A0 del ADS1115
       (VBAT)    └──────────────┘  │
                 ┌──────────────┐  │      ┌────────┐
            ┌────┤ R_BOT  100k  ├──┘      │ 100 nF │  (filtro, opcional
            │    └──────────────┘         └───┬────┘   pero recomendado)
            │                                 │
   GND ─────┴─────────────────────────────────┘
```

| ADS1115 | ESP32-CAM | Nota |
|---|---|---|
| `VDD` | `3V3` | **no** 5 V: la referencia debe ser la del ESP32 |
| `GND` | `GND` | masa común obligatoria con el pack |
| `SCL` | `GPIO15` | strapping MTDO: en alto = arranque normal, el pull-up es seguro |
| `SDA` | `GPIO14` | |
| `ADDR` | `GND` | dirección `0x48` |
| `A0` | punto medio del divisor | |

Los módulos ADS1115 comerciales ya traen pull-ups de 10 k en SDA/SCL. Si el tuyo
no los lleva, añade 4,7 k a 3V3 en ambas líneas.

> **Límite absoluto del ADS1115**: la entrada analógica no puede pasar de
> **VDD + 0,3 V = 3,6 V**. Pasarse no falsea la lectura: estropea el chip.
> Con 4 pilas en serie (hasta 6,4 V) un divisor 1:2 daría 3,2 V — demasiado
> cerca del límite. Por eso el divisor es **220 k / 100 k**, factor 3,2.

| Pack | Máximo | En A0 | Margen |
|---|---|---|---|
| 4 × AA alcalina nuevas | 6,4 V | 2,00 V | holgado |
| 4 × AA NiMH cargadas | 5,6 V | 1,75 V | holgado |
| 1 × 18650 Li-ion | 4,2 V | 1,31 V | holgado |

Fuga del divisor: 20 µA, despreciable.

### 4.2 Alimentación

La ESP32-CAM tiene un LDO **AMS1117** de 5 V → 3,3 V. Con 4 pilas AA en serie
(hasta 6,4 V con pilas nuevas) entrando al pin `5V`, el LDO tira a calor todo
lo que sobra por encima de 3,3 V. Pon un **condensador de 470–1000 µF** lo más
cerca posible del pin de alimentación: los picos de TX del WiFi son de
300–500 mA durante microsegundos y son la causa nº 1 de reinicios por
brownout con pilas.

### 4.3 GPIO32 (PWDN) durante el deep sleep

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
cd esp32cam_live_monitor
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
| Partition Scheme | probar primero **por defecto**; si da `Sketch too big`, pasar a **Huge APP (3MB No OTA/1MB SPIFFS)** |
| PSRAM | **Enabled** |
| Upload Speed | 921600 (baja a 115200 si falla) |
| Puerto | el COM/tty de tu adaptador USB-serie |

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

Abre `esp32cam_live_monitor.ino` (doble clic) y pulsa **Subir**.

### 6.5 Monitor serie

Herramientas → Monitor Serie, a **115200 baudios**. Ejemplo de un ciclo
completo con `CAPTURE_INTERVAL_MS` bajado a 30 s para pruebas:

```
[     210] === ESP32-CAM ParkVision (CAM-001) ===
[     620] CAM: lista (800x600, PSRAM=si)
[     690] BAT: ADS1115 ok (config=0x8583). Divisor x3.200
[     780] BAT: 5.98 V, 88% [ok]
[    3120] WIFI: conectando a "ParkVisionFog1"...
[    4380] WIFI: conectado. IP=192.168.1.42  RSSI=-58 dBm
[    4650] UPLOAD: POST http://parkvision.local:8000/api/v1/cameras/CAM-001/frames (41230 bytes)
[    5210] UPLOAD: respuesta HTTP 201
[    5210] UPLOAD: ok (201)
[    5215] CAM: sensor apagado (PWDN) y retenido para deep sleep
[    5220] SLEEP: durmiendo 30000 ms
```

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
| Corregir el voltaje medido | `BATTERY_CALIBRATION` = V_multímetro / V_mostrado |
| Otro divisor de resistencias | `BATTERY_R_TOP_OHM` / `BATTERY_R_BOTTOM_OHM` |
| Reinicios por brownout con pilas | primero el condensador; si persiste, `POWER_DISABLE_BROWNOUT 1` |

Después de tocar `config.h`, **vuelve a subir el sketch**: es código compilado.

---

## 8. Problemas típicos

| Síntoma | Causa habitual |
|---|---|
| `#error "Falta secrets.h..."` | No has creado `secrets.h`, o le falta algún valor. Copia `secrets.h.example` y rellénalo (§6.1). |
| `Sketch too big` al compilar | Cambiar *Partition Scheme* a **Huge APP**. |
| `esp_camera_init fallo (0x105)` | Cable plano mal insertado o alimentación insuficiente. Prueba con `CAM_XCLK_FREQ_HZ 10000000`. |
| `Camera probe failed` o cuelgue al arrancar | PSRAM sin habilitar en Herramientas. |
| `WIFI: sin conexion tras ... ms` | Router fuera de rango, SSID/password mal en `secrets.h`, o red de 5 GHz (el ESP32 no la ve). |
| `UPLOAD: fallo la conexion` | La Raspberry Pi está apagada/inalcanzable, o `FOG_SERVER_HOST`/`FOG_SERVER_PORT` no coinciden. El firmware agota el timeout y duerme igual. |
| `UPLOAD: fallo (401)` / `(403)` | `API_KEY`/`API_TOKEN` incorrectos para ese `DEVICE_ID`, o mezclados entre cámaras. |
| Dos cámaras compitiendo por el mismo `DEVICE_ID` | Revisar qué `secrets.h` se subió a cada unidad antes de flashear. |
| `ADS1115 no responde en 0x48` | `ADDR` sin conectar (debe ir a GND), faltan pull-ups, o SDA/SCL intercambiados. |
| Voltaje leído = la mitad del real | `BATTERY_R_TOP_OHM`/`BATTERY_R_BOTTOM_OHM` no coinciden con las resistencias reales. |
| Voltaje leído 0,00 V | El divisor no tiene masa común con el ESP32. |
| `Failed to connect… Timed out waiting for packet header` | Falta el puente `IO0`–`GND`, o no pulsaste RST antes de subir. |
| Reinicios al conectar el WiFi | Falta el condensador de 470–1000 µF; o el FTDI no da corriente suficiente. |
| No arranca tras cablear el I2C | Revisa que no hayas usado GPIO12: con pull-up impide el arranque. |

---

## 9. El sketch anterior

`esp32cam_fotos_stream.ino` (fotos a microSD + streaming) está archivado en
`../legacy/`. La versión con dashboard en vivo (streaming MJPEG, servidor HTTP
propio) fue la primera iteración de este mismo proyecto y ya no está: se
migró a esta arquitectura de cliente + deep sleep para la integración con la
Raspberry Pi de ParkVision.
