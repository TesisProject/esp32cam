# ESP32-CAM Live Monitor — cliente REST con deep sleep

Firmware para **ESP32-CAM AI-Thinker**, proyecto de tesis UPC ("ParkVision").
Cada ciclo el módulo captura una foto, la sube por `POST` a una Raspberry Pi
local ("fog node") que detecta plazas de parking y reenvía el resultado como
JSON a la nube, y entre captura y captura se apaga casi por completo (deep
sleep) para maximizar la autonomía con baterías Li-ion.

**Sin dashboard. Sin servidor HTTP local. Sin librerías externas**: solo lo
que trae el core ESP32 (`HTTPClient`, `Preferences`, cJSON). Un sketch, y la
WiFi y el host del Fog se graban por USB en cada placa (§6.6), no en el
binario.

```
ESP32-CAM  ─ HTTP POST (JPEG) ─→  Raspberry Pi (fog node)  ─→  nube
   │
   ├─ 5V ←── MT3608 ←── TP4056 ←── una 18650 Li-ion (1S)
   ├─ GPIO13 ←── divisor 100k/100k (tensión del pack)
   └─ SCCB ──→ OV2640
```

Hay **3 unidades físicas** (`CAM-001`, `CAM-002`, `CAM-003`), mismo código
fuente y **un único `.bin`**, sin secretos dentro: la WiFi, la credencial de
la API y el host del Fog de cada una se graban por USB en su NVS (§6.6).

---

## 1. Configuración elegida

| Parámetro | Valor |
|---|---|
| Pack de batería | **una celda 18650 Li-ion (1S, 3,0–4,2 V)** → TP4056 → MT3608 a 5 V; tensión medida por GPIO13 (§4.3) |
| microSD | **No se usa**: su D3 está unido a GPIO13 y falsearía la batería (§3) |
| Fotos no subidas | Se guardan en la **flash interna** (LittleFS, 128 KB): 1–3 fotos, 5–15 min de corte (§2) |
| Red | **Solo estación** — necesita el router para llegar a la Pi; el modo AP impide dormir |
| Intervalo de captura | **5 minutos** (`CAPTURE_INTERVAL_MS`), deep sleep entre medio |
| Subida | `POST` multipart a `http://<fogHost>:<fogPort>/api/v1/camera/frames` (sin id: el Fog identifica la cámara por `X-API-Key`/`X-API-Token`); responde `202 Accepted` |

Las constantes y los valores por defecto viven en `config.h`. Lo propio de
cada placa (WiFi, credencial de la API, host y puerto del Fog) vive en su
**NVS** y se graba por USB (§6.6).

---

## 2. Estructura del sketch

```
esp32cam/
├── esp32cam.ino                Orquesta el ciclo: setup() = un ciclo completo -> deep sleep.
├── config.h                    Constantes y valores por defecto (pines, umbrales, endpoint…)
├── provision.example.json      Plantilla de WiFi + credencial + host del Fog para grabar por USB
├── provision.local.json        La tuya — NO se versiona
├── scripts/provision.ps1       La envía a la placa por el puerto serie
├── src/
│   ├── camera/camera_module.*  OV2640: init, captura, PWDN + hold para deep sleep
│   ├── wifi/wifi_module.*      Conexión en modo estación (timeout, MAC, IP fija)
│   ├── upload/upload_module.*  POST multipart/form-data (foto + telemetría) a la Raspberry Pi
│   ├── ota/ota_module.*        FW_VERSION, rollback y cliente OTA (firma, descarga, instalación)
│   ├── fogconfig/fogconfig_module.*  Config de la cámara del Fog (versión de firmware asignada)
│   ├── battery/battery_module.*  Tensión del pack por GPIO13 y % (antes del WiFi)
│   ├── clock/clock_module.*    Hora real a partir de la cabecera Date del Fog
│   ├── backlog/backlog_module.*  Fotos no subidas en la flash interna (LittleFS)
│   ├── settings/settings_module.*   Única puerta a la NVS (Preferences)
│   └── provision/provision_module.* Aprovisionamiento por serie (línea JSON)
└── README.md
```

Arduino compila los ficheros de la raíz del sketch y, **recursivamente, todo
lo que haya dentro de `src/`**. Cualquier otra subcarpeta se ignora, así que
un módulo nuevo va en `src/<nombre>/`. Desde el `.ino` se incluyen con la
ruta (`#include "src/wifi/wifi_module.h"`) y los módulos llegan a `config.h`
con `#include "../../config.h"`.

`config.h` y los `provision.*.json` se quedan en la raíz a propósito: son lo
único que se toca para configurar cada unidad.

> El Arduino IDE 2.x solo muestra como pestañas los ficheros de la raíz: los
> de `src/` compilan igual, pero se editan desde VS Code u otro editor.

> **La carpeta debe llamarse igual que el `.ino`.** Si la renombras, renombra
> también el sketch, o el IDE no lo abrirá.

### Un ciclo completo

No hay `loop()` real: cada despertar del deep sleep ejecuta `setup()` de
principio a fin y termina en `esp_deep_sleep_start()`, que no retorna.

0. Leer de la NVS la WiFi y el Fog. Sin WiFi guardada, la placa solo espera
   el aprovisionamiento por USB (§6.6) y, si no llega, vuelve a dormir.
1. Medir la batería (GPIO13 es ADC2: tiene que ser antes del WiFi). Por
   debajo de 3,3 V, dormir sin encender cámara ni WiFi (§4.3).
2. Inicializar la cámara.
3. Conectar WiFi en modo estación, con timeout (`WIFI_CONNECT_TIMEOUT_MS`).
4. Capturar 1 frame JPEG y subirlo por `POST` a la Pi, con la telemetría
   del ciclo en el mismo multipart (ver abajo). Si la respuesta es 2xx y la
   imagen en ejecución viene de un OTA sin confirmar, confirmarla.
5. Si la subida fue bien: poner el reloj en hora con la cabecera `Date` de
   la respuesta y subir hasta 3 fotos atrasadas. Si no (sin WiFi, error de
   red o 5xx): guardar la foto en la flash interna.
6. Apagar la cámara (PWDN + `rtc_gpio_hold_en`), apagar WiFi, dormir
   `CAPTURE_INTERVAL_MS` y repetir.

Si falla la cámara o el WiFi, el ciclo se aborta y el dispositivo duerme
igual — no se queda colgado reintentando, que gasta batería. Sin WiFi, la
foto se captura de todos modos después del intento de conexión y se guarda.

### Fotos atrasadas

Una foto que no se pudo subir se guarda en la partición `spiffs` de la
flash interna (128 KB, LittleFS) y se sube en el siguiente ciclo con
conexión, **después** de la foto actual, con su hora de captura en
`captured_at`.

- **Capacidad:** 1–3 fotos (30–70 KB cada una), es decir, 5–15 min de corte.
  Si se llena, se descarta la más antigua. Cubre reinicios del router o del
  Fog, no cortes largos.
- **Hora:** el ESP32 no tiene reloj con pila. La toma de la cabecera `Date`
  de cada respuesta del Fog y la mantiene durante el deep sleep. Tras un
  arranque en frío, hasta la primera subida correcta no hay hora y las fotos
  que fallen se pierden.
- **Coste:** nada en los ciclos normales; ~0,5 s con el WiFi apagado al
  guardar una foto y un `POST` más por cada foto reenviada.

Detalle y contrato con el Fog y la nube: `docs/handoff-buffer-fotos-offline.md`.

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
| `battery_valid` | `true` si la lectura de GPIO13 es la de un pack Li-ion conectado (2,5–4,5 V) |
| `battery_mv` | tensión del pack en reposo, mV; solo se envía si `battery_valid` es `true` |
| `captured_at` | solo en fotos atrasadas: hora de captura, ISO 8601 UTC. Esas fotos no llevan `rssi`, `wifi_connect_ms`, `awake_ms`, `free_heap`, `wake_count` ni `wifi_fail_streak` (serían del ciclo de la subida) |

El **porcentaje no viaja**: Fog y nube guardan medidas y la nube deriva el %
al leer, para que cambiar la curva de descarga no invalide el histórico (el
Fog ignora un campo `battery_percent`). El firmware lo calcula solo para el
log serie, con la misma tabla que la nube (`docs/handoff-bateria-cloud.md`).
`battery_load_mv` (tensión durante la transmisión) tampoco: GPIO13 es ADC2 y
no se puede leer con el WiFi encendido.

---

## 3. Pines ocupados

**GPIO21 y GPIO22 NO están libres en la AI-Thinker.** En esta placa `GPIO21 = Y5`
(bit de datos de la cámara) y `GPIO22 = PCLK`. Están cableados al OV2640.

El OV2640 ocupa: `0, 5, 18, 19, 21, 22, 23, 25, 26, 27, 32, 34, 35, 36, 39`.
GPIO33 lleva el LED rojo integrado (LED de estado, `STATUS_LED_PIN`).

GPIO13 mide la batería (divisor, §4.3).

> **Si en el futuro añades algo por GPIO**: GPIO12 es el strapping MTDI y un
> pull-up ahí impide arrancar el chip. GPIO14/15 están libres.

> **microSD: no, ni en modo 1 bit.** El modo 1 bit solo usa GPIO14/15/2,
> pero la pista D3 del slot sigue soldada a GPIO13, y toda microSD tiene en
> DAT3 un pull-up interno (10–90 kΩ a 3,3 V) activo mientras está
> alimentada, que en esta placa es siempre. Con la tarjeta puesta, GPIO13
> pasa de `Vbat/2` (1,50–2,10 V) a ~2,40–2,70 V: la mitad de rango y fuera de
> la zona lineal del ADC. Leer la batería antes de montar la SD no lo evita.
> Para usarla habría que cortar la pista DAT3 o mover el divisor a GPIO33
> (ADC1, quitando el LED rojo).

---

## 4. Cableado

### 4.1 Alimentación

Una celda 18650 (1S, 3,0–4,2 V) → **TP4056** con protección (carga y
corte por descarga profunda) → **MT3608** ajustado a 5,0 V → pin `5V` de la
placa → su LDO **AMS1117** a 3,3 V. Pon un **condensador de 470–1000 µF** lo
más cerca posible del pin de alimentación: los picos de TX del WiFi son de
300–500 mA durante microsegundos y son la causa nº 1 de reinicios por
brownout.

### 4.2 GPIO32 (PWDN) durante el deep sleep

El deep sleep del ESP32 apaga la CPU pero **no** corta la alimentación de la
cámara: comparte el mismo rail de 3,3 V. Antes de dormir, el firmware pone
PWDN (GPIO32) en alto para apagar el sensor y usa `rtc_gpio_hold_en()` para
que ese nivel se mantenga durante todo el deep sleep (GPIO32 es RTC-capable).
Al despertar, `camera_init()` suelta el hold con `rtc_gpio_hold_dis()` antes
de reconfigurar el pin.

### 4.3 Medida de la batería (GPIO13)

Divisor **100 kΩ + 100 kΩ** desde VIN+ del MT3608 (la tensión de la celda)
a GND; el punto medio va a **GPIO13**, con **100 nF** a GND. El divisor
consume ~20 µA permanentes.

**Relación.** En GPIO13 solo está el divisor; no hay nada más que desplace
el nodo (comprobado en la placa real el 2026-10-13), así que `V13 = Vbat/2`:

| Vbat | 3,0 V | 3,3 V | 3,7 V | 4,2 V |
|---|---|---|---|---|
| V13 (nominal) | 1,50 V | 1,65 V | 1,85 V | 2,10 V |

Todo el rango cabe en ~600 mV del ADC y por debajo de ~2,45 V, donde el ADC
del ESP32 a 12 dB es lineal; 1 mV de error en GPIO13 son 2 mV de batería.

**Por qué calibrar igualmente con dos puntos y no multiplicar por 2.** El ADC
de cada ESP32 tiene su propio error de ganancia y de offset (decenas de mV
incluso con la calibración de fábrica del eFuse que usa
`analogReadMilliVolts`), y las resistencias del 1 % también mueven la razón.
Todo es lineal, así que dos puntos medidos corrigen ambos errores.
Precisión esperable tras calibrar: **±20–30 mV, ~±5 %** de carga en la zona
plana de la curva (3,65–3,85 V, donde 100 mV son 15–30 %). La lectura
nocturna (sin el panel cargando) es la más fiable.

**ADC2 y WiFi.** GPIO13 es ADC2, que el WiFi ocupa en cuanto arranca. El
firmware lee al principio del ciclo, con la radio apagada (32 muestras,
~70 ms) y antes de encender la cámara, para medir en reposo.

**Lectura no válida.** Si la recta da algo fuera de 2,5–4,5 V (placa
alimentada por USB sin celda: GPIO13 a ~0 V; o sin divisor: GPIO13 al aire),
se envía `battery_valid=false`, no se envía `battery_mv` y
**no** se aplica el corte.

**Corte por batería baja.** Por debajo de **3,3 V** la placa vuelve a dormir
sin encender cámara ni WiFi (evita los picos de TX con la celda casi vacía).
Con **histéresis**: una celda Li-ion recupera 50–100 mV en reposo, así que
una vez cortada no vuelve a transmitir hasta superar **3,4 V**. Umbrales en
`config.h` (`BATTERY_CUTOFF_MV`, `BATTERY_RESUME_MV`).

En el monitor serie, cada ciclo:

```
[     412] BAT: ADC IO13: 1855 mV | Bateria: 3.71 V | 37 % (cal. defecto)
```

#### Calibrar (una vez por placa)

Los defectos de `config.h` son la razón nominal 1/2; cada placa se desvía
(ADC y tolerancia del divisor), así que conviene calibrarla:

1. Con todo montado, abre el monitor serie y anota `ADC IO13` de la línea
   `BAT:`. En ese mismo momento mide con el multímetro la tensión de las
   celda (sus bornes, no el punto medio). Mejor sin el panel cargando.
2. Repite con otro nivel de carga separado **al menos 300 mV**, p. ej. celda
   llena (~4,15 V) y a media carga (~3,7 V).
3. Añade los dos puntos a tu `provision.local.json` y reaprovisiona (§6.6):

   ```json
   "batteryCal": { "adcA": 1838, "batA": 3700, "adcB": 2061, "batB": 4150 }
   ```

   `adcA`/`adcB` son los mV de la línea `BAT:`; `batA`/`batB`, los del
   multímetro. Se guarda en la NVS de esa placa: no hay que recompilar, y el
   mismo `.bin` sirve para las tres cámaras.

---

## 5. Autonomía con deep sleep

A diferencia de un dispositivo que sirve un dashboard permanentemente (donde
el deep sleep es inviable porque se pierde la asociación WiFi), aquí **no hay
nada que servir entre capturas**: el dispositivo despierta, sube una foto y
vuelve a dormir.

Estimación **sin medir todavía** (hay que confirmarla con un amperímetro en
serie con la celda). Corrientes vistas desde la celda (3,7 V), con el MT3608
a ~85 % de rendimiento:

| Estado | En el pin 5V | Desde la celda | Duración por ciclo | Carga por ciclo |
|---|---|---|---|---|
| Deep sleep | ~3–6 mA | **~6–12 mA** | ~296 s | ~0,5–1,0 mAh |
| Despierto: cámara + WiFi + POST | ~150–200 mA | ~250–320 mA | ~3–5 s | ~0,25–0,45 mAh |

**El deep sleep no es de microamperios en esta placa.** El chip ESP32
duerme en ~10 µA, pero la AI-Thinker lleva un AMS1117 (varios mA de
corriente de reposo) y la PSRAM y el sensor siguen alimentados por el mismo
rail de 3,3 V; el MT3608, con tan poca carga, además rinde mal. Por eso, a 5
minutos, **el reposo pesa más que el ciclo activo**. Ninguna optimización del
firmware baja de ahí: haría falta cambiar el hardware (un regulador LDO de
baja corriente de reposo, o cortar la alimentación de la placa entre ciclos
con un temporizador externo tipo TPL5110).

Consumo medio estimado: **~9–17 mAh por hora (~220–400 mAh/día)**.

| Batería (18650 de ~2500 mAh, ~90 % útil hasta el corte de 3,3 V) | A 5 min | A 30 s (pruebas) |
|---|---|---|
| 1 celda (1S1P) | ~5–10 días | ~1,5–3 días |
| 4 celdas en paralelo (1S4P) | ~3–6 semanas | ~6–12 días |

**Varias celdas: siempre en paralelo.** El firmware no cambia: la tensión
sigue siendo la de una celda (3,0–4,2 V), así que el divisor, la
calibración, la curva de porcentaje y el corte valen igual; solo cambia la
capacidad. Las celdas deben unirse con la misma tensión (diferencia menor de
~0,1 V), o la más cargada descarga a la otra con corrientes muy altas.
En serie no: 4S son 12–16,8 V, el TP4056 solo carga 1S, el MT3608 solo
eleva tensión y el divisor pondría 6–8,4 V en GPIO13 (el máximo es 3,6 V).
Con 4 celdas el TP4056 (1 A) tarda ~10–12 h en una carga completa.

**Para pruebas de campo**: bajar `CAPTURE_INTERVAL_MS` a 30000 (30 s) para no
esperar 5 minutos entre cada verificación, y devolverlo a 300000 antes de
dejar la cámara desatendida con batería.

---

## 6. Compilar y grabar con el Arduino IDE

### 6.1 Requisitos

#### Credenciales

El firmware **no contiene credenciales**: se compila y se flashea el mismo
`.bin` en las tres cámaras. La WiFi, la credencial de la API (id de cámara,
`apiKey`, `apiToken`) y el host del Fog se graban después de flashear, por
USB (§6.6).

> Ya no hay `secrets.h`. Si te queda uno de una versión anterior, bórralo:
> nada lo incluye (git lo sigue ignorando por si acaso).

#### Soporte de placas

Solo el soporte de placas ESP32 de Espressif:

> Archivo → Preferencias → *Gestor de URLs Adicionales de Tarjetas*:
> `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
> y luego Herramientas → Placa → Gestor de Tarjetas → buscar **esp32**.

**Ninguna librería que instalar.** `HTTPClient`, `Preferences` y cJSON vienen
con el core ESP32. Si el
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
(`app0`/`app1`): el firmware actual (~1,15 MB) ocupa el **58 %** de cada una.
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
[     210] === ESP32-CAM ParkVision ===
[     211] OTA: FW_VERSION=2  particion activa=app0  estado=UNDEFINED (flasheo USB)
[     212] Reset reason: 8  wake_count=12  wifi_fail_streak=0
[     230] NVS: camera=CAM-001  wifi="ParkVisionFog1"  fog=parkvision.local:8000 (NVS)
[     620] CAM: lista (800x600, PSRAM=si)
[     720] WIFI: MAC=24:6F:28:XX:XX:XX
[     720] WIFI: conectando a "ParkVisionFog1"...
[    4380] WIFI: conectado en 3660 ms. IP=192.168.1.42  RSSI=-58 dBm
[    4645] UPLOAD: telemetria fw=2 rssi=-58 wifi_ms=3660 reset=8 awake_ms=4640 heap=172000 wake=12 wifi_fails=0 SVGA q=12
[    4650] UPLOAD: POST http://parkvision.local:8000/api/v1/camera/frames (42050 bytes)
[    5210] UPLOAD: respuesta HTTP 202
[    5210] UPLOAD: ok (202)
[    5215] CAM: sensor apagado (PWDN) y retenido para deep sleep
[    5220] SLEEP: durmiendo 30000 ms
```

- `Reset reason: 8` es el despertar normal del deep sleep; `1` es encendido
  y `9` brownout (batería o condensador). Con cualquier valor distinto de `8`,
  `wake_count` vuelve a 0.
- Sin PSRAM, `CAM:` avisa de que se degrada a QVGA y aparece
  `UPLOAD: body en RAM interna (... bytes, sin PSRAM)`: la foto se sube
  igual, pero a 320×240 la detección en el Fog pierde precisión.
- Tras un OTA el estado es `PENDING_VERIFY` y, después de la primera subida
  buena, `OTA: imagen de app1 confirmada`. El cliente OTA pregunta al Fog
  una vez por hora si hay versión nueva; solo instala versiones mayores con
  la firma de la nube válida (ver `CLAUDE.md`, "Cliente OTA (E5)").
- Tras encender o pulsar RST (`Reset reason: 1`) aparece además
  `PROV: READY modo=reaprovisionar ventana=3000 ms`: son 3 s en los que se
  puede reescribir la configuración (§6.6). Al despertar del deep sleep no
  aparece.

### 6.6 Aprovisionar: WiFi, credencial y host del Fog (una vez por placa)

La WiFi, la credencial de la cámara y el host del Fog **no están en el
binario**: el mismo `.bin` sirve para cualquier placa y cualquier red. Se graban en la NVS de la placa (una
zona de la flash que sobrevive a reinicios, al deep sleep y a reflashear el
sketch) con una línea JSON por el puerto serie:

1. Copia la plantilla y rellénala (git ignora la copia):

   ```powershell
   copy provision.example.json provision.local.json
   ```

   ```json
   {
     "wifi": { "ssid": "ParkVisionFog1", "password": "…" },
     "camera": { "id": "CAM-001", "apiKey": "…", "apiToken": "…" },
     "fogHost": "192.168.1.20",
     "fogPort": 8000
   }
   ```

   `camera` es **distinto para cada unidad** (`CAM-001`/`CAM-002`/`CAM-003`):
   `apiKey`/`apiToken` son los que el Fog dio de alta para esa cámara (van
   en `X-API-Key`/`X-API-Token`; el Fog identifica la cámara solo por
   ellos). `id` solo sale en el log de arranque. Los tres van juntos; key y
   token, 1–63 caracteres ASCII sin espacios.

   La red debe ser de **2,4 GHz** (el ESP32 no ve las de 5 GHz) y WPA2 con
   contraseña de 8 a 63 caracteres ASCII: las redes abiertas se rechazan.
   `fogHost` es un nombre o una IP, **sin** `http://` ni `:puerto`.
   `fogHost`/`fogPort` son opcionales: sin ellos se usan los defectos de
   `config.h` (`parkvision.local:8000`). También es opcional `batteryCal`,
   la calibración de la medida de batería (§4.3).

2. Cierra el Monitor serie (el puerto no se puede abrir dos veces) y ejecuta:

   ```powershell
   .\scripts\provision.ps1 -Port COM5
   ```

   o, en VS Code, la tarea **Arduino: Aprovisionar**. El script valida el
   JSON, reinicia la placa con un pulso de RTS, espera `PROV: READY`, envía
   la línea y muestra la respuesta y el ciclo siguiente:

   ```
   Reiniciando la placa en COM5 (pulso RTS)...
     | PROV: READY modo=aprovisionar ventana=60000 ms. Enviar una linea JSON
   Enviando: camera.id=CAM-001 wifi.ssid="ParkVisionFog1" fogHost=192.168.1.20 fogPort=8000
     | PROV: OK guardado: camera=CAM-001 wifi="ParkVisionFog1" fog=192.168.1.20:8000 bat=... Reiniciando
   OK: la placa guardo la configuracion y reinicia.
   ```

   La contraseña, la `apiKey` y el `apiToken` no se imprimen nunca, ni en
   el PC ni en la placa.

**Cuándo escucha la placa:**

| Situación | Qué hace |
|---|---|
| NVS sin WiFi o sin credencial (placa nueva, borrada, o aprovisionada con un firmware anterior a `FW_VERSION` 2) | espera la línea **60 s** (`PROV_WAIT_MS`); si no llega, duerme **1 h** (`PROV_RETRY_SLEEP_MS`) y repite. En campo, sin USB, no puede esperar indefinidamente: así gasta ~0,7 mAh por hora |
| Ya aprovisionada, tras encender / RST / pulso RTS | ventana de **3 s** (`PROV_WINDOW_MS`) para reescribir o borrar |
| Despertar del deep sleep | nada: los ciclos normales no esperan |

**Reescribir o borrar.** Volver a ejecutar el script reescribe la WiFi, la
credencial y el Fog (la placa cambia solo lo que venga en la línea; el
script exige siempre `wifi` y `camera`). Para que la placa lo olvide
todo y vuelva a pedir aprovisionamiento:

```powershell
.\scripts\provision.ps1 -Port COM5 -Erase      # o la tarea "Arduino: Borrar NVS"
```

Sin la base ESP32-CAM-MB (un FTDI sin línea RTS), el script avisa a los 4 s:
pulsa **RST** en la placa en ese momento.

**Desde el Monitor serie, sin script.** Se pega la línea JSON entera (una
sola línea) y se pulsa Enter; vale cualquier fin de línea (CR, LF o CRLF).
Con una placa sin aprovisionar hay 60 s desde `PROV: READY`, de sobra. Ya
aprovisionada, la ventana es de 3 s tras RST: más cómodo mandar primero
`{"cmd":"erase"}` (o usar el script), y la placa queda esperando 60 s. Con
`arduino-cli monitor`, `dtr=off,rts=off` (§8). Por el serie la línea se valida
igual que con el script, pero sin los avisos de «valor de la plantilla».

---

## 7. Ajustes frecuentes en `config.h`

| Quiero… | Toca |
|---|---|
| Más autonomía / menos tráfico | subir `CAPTURE_INTERVAL_MS` |
| Probar sin esperar 5 min | bajar `CAPTURE_INTERVAL_MS` a 30000 (volver a subirlo después) |
| Imagen más nítida | `CAM_JPEG_QUALITY_SNAPSHOT 10`, `CAM_FRAMESIZE_SNAPSHOT FRAMESIZE_UXGA` |
| Cámara del revés | `CAM_VFLIP 1` y/o `CAM_HMIRROR 1` |
| IP siempre la misma | `WIFI_USE_STATIC_IP 1` + los cuatro `WIFI_STATIC_*` |
| Reinicios por brownout | primero el condensador; si persiste, `POWER_DISABLE_BROWNOUT 1` |

Después de tocar `config.h`, **vuelve a subir el sketch**: es código compilado.

Lo que **no** se cambia aquí, sino reaprovisionando la placa (§6.6), sin
recompilar: la red WiFi, la credencial de la cámara y el host/puerto del Fog. Por ejemplo, si la Pi no
resuelve por `.local`, aprovisiona `fogHost` con su IP. `FOG_SERVER_HOST_DEFAULT`
de `config.h` solo se usa en placas sin host guardado.

---

## 8. Problemas típicos

| Síntoma | Causa habitual |
|---|---|
| `Sketch too big` al compilar | Comprobar que el *Partition Scheme* es **Minimal SPIFFS** (con *Default* solo hay 1,25 MB). No pasar a Huge APP: no tiene segunda partición y anula el OTA. |
| `esp_camera_init fallo (0x105)` | Cable plano mal insertado o alimentación insuficiente. Prueba con `CAM_XCLK_FREQ_HZ 10000000`. |
| `Camera probe failed` o cuelgue al arrancar | PSRAM sin habilitar en Herramientas. |
| `WIFI: sin conexion tras ... ms` | Router fuera de rango, SSID/contraseña mal aprovisionados (reaprovisiona, §6.6), o red de 5 GHz (el ESP32 no la ve). |
| `UPLOAD: fallo la conexion` | La Raspberry Pi está apagada/inalcanzable, o el host/puerto del Fog aprovisionados no son los suyos (mira la línea `NVS: ... fog=` del arranque). El firmware agota el timeout y duerme igual. |
| `PROV: la NVS no tiene WiFi o credencial` y la placa duerme 1 h | Placa sin aprovisionar (nueva, tras `-Erase`, o aprovisionada antes de `FW_VERSION` 2, sin `camera`). Ejecuta el script (§6.6). |
| El script dice `no dijo 'PROV: READY'` | Puerto equivocado, firmware sin aprovisionamiento, `IO0` puenteado a GND (modo descarga), o adaptador sin RTS (pulsa RST cuando avise). |
| El script no puede abrir el puerto | Está abierto en el Monitor serie del IDE o de VS Code: ciérralo. |
| `PROV: ERROR ...` | El motivo viene en la misma línea (SSID de más de 32 bytes, contraseña fuera de 8–63, `fogHost` con `http://`, campo desconocido…). El script valida lo mismo antes de enviar. |
| `UPLOAD: fallo (401)` / `(403)` | `camera.apiKey`/`apiToken` aprovisionados incorrectos, o mezclados entre cámaras (el Fog identifica la cámara solo por ellos). Reaprovisiona (§6.6). |
| `UPLOAD: fallo (405)` | La URL apunta a `/api/v1/cameras/<id>/frames` (solo `GET` de administración en el Fog); la buena es `/api/v1/camera/frames`. |
| Dos cámaras subiendo como la misma (mismas fotos mezcladas en el Fog) | Comparten `apiKey`/`apiToken`: se aprovisionaron con el mismo `provision.local.json`. El `camera=` de la línea `NVS:` del arranque dice qué credencial tiene cada placa. |
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
