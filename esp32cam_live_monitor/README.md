# ESP32-CAM Live Monitor

Dashboard web autosuficiente sobre **ESP32-CAM AI-Thinker**: el propio módulo se
conecta al WiFi, levanta su servidor HTTP y sirve una página con la imagen de la
cámara en vivo y el nivel de batería.

**Sin Raspberry Pi. Sin servidor Python. Sin servicios en la nube. Sin librerías
externas.** Solo el core ESP32 de Espressif en el Arduino IDE, un sketch, un
flasheo.

```
Navegador  ←─ HTTP ─→  ESP32-CAM  ─ I2C ─→  ADS1115 ─ divisor ─→  pack 18650
                          │
                          └─ SCCB ──→ OV2640
```

---

## 1. Configuración elegida

| Parámetro | Valor |
|---|---|
| Pack de batería | **4 × AA en serie** (`BATTERY_CHEMISTRY`: alcalina / NiMH / Li-ion) |
| microSD | **No se usa** (libera GPIO14/15 para I2C) |
| Medida de batería | ADS1115 por I2C + divisor resistivo **220 k / 100 k** |
| Refresco del dashboard | **60 s** (`DASHBOARD_BATTERY_REFRESH_MS`) |
| Red | **AP de respaldo**: intenta el router y, si falla, crea su propia red |
| Modo de vídeo | **Híbrido**: foto fija cada 5 s + botón *Ver en vivo* (MJPEG bajo demanda) |

Todo esto vive en `config.h`. Ningún otro fichero —ni siquiera el HTML del
dashboard— repite un solo valor: la página pide `/config` al arrancar y se
configura sola.

---

## 2. Estructura del sketch

El Arduino IDE compila **todos** los `.cpp` y `.h` que estén en la carpeta del
sketch, y los muestra como pestañas. Por eso el proyecto es modular sin dejar de
ser un sketch normal:

```
esp32cam_live_monitor/
├── esp32cam_live_monitor.ino   setup()/loop(), WiFi y gestión de energía
├── config.h                    TODA la configuración (WiFi, pines, umbrales…)
├── secrets.h                   Credenciales WiFi — NO se versiona
├── secrets.h.example           Plantilla de secrets.h
├── camera_module.h / .cpp      OV2640: init, perfiles, captura, PWDN
├── battery_module.h / .cpp     ADS1115: driver I2C propio + curva Li-ion
├── web_server_module.h / .cpp  HTTP: dashboard, /battery, /capture, /stream
├── dashboard_html.h            El dashboard (13 kB de HTML+CSS+JS en flash)
└── README.md
```

> **La carpeta debe llamarse igual que el `.ino`.** Si la renombras, renombra
> también el sketch, o el IDE no lo abrirá.

### ¿Por qué el HTML va dentro del firmware?

La alternativa es LittleFS/SPIFFS, que exige instalar un **plugin extra** en el
Arduino IDE y hacer **una segunda subida** cada vez. Metiéndolo en
`dashboard_html.h` como cadena en flash, el módulo queda todavía más
autocontenido: un binario, un flasheo, cero herramientas adicionales.

El coste: para tocar el dashboard hay que editar `dashboard_html.h` y regrabar el
sketch. En el ESP32 la flash está mapeada en memoria, así que la cadena se sirve
directamente desde su puntero — no consume RAM.

### Endpoints

| Ruta | Puerto | Devuelve |
|---|---|---|
| `/` | 80 | Dashboard HTML |
| `/config` | 80 | JSON de configuración para el dashboard |
| `/battery` | 80 | `{"valid":true,"voltage":3.92,"percent":68,"state":"ok",…}` |
| `/capture` | 80 | Una foto JPEG (SVGA 800×600) |
| `/status` | 80 | Uptime, heap, RSSI, IP, modo de red, resolución, CPU |
| `/test` | 80 | **Página de diagnóstico sin JavaScript**: un `<img>` pelado |
| `/stream` | **81** | MJPEG multipart (VGA 640×480, máx. 8 fps) |

> **¿Por qué el stream en otro puerto?** El handler MJPEG no termina nunca
> mientras el cliente mire, y `esp_http_server` atiende las peticiones de un
> puerto en un único hilo. Si `/stream` y `/battery` compartieran puerto, el
> `fetch` de batería se quedaría colgado esperando a que acabase el vídeo. Es el
> mismo reparto que usa el ejemplo oficial `CameraWebServer` de Espressif.

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

Para una tesis, la diferencia de precisión decide: con el ADC del ESP32 el
porcentaje de batería salta ±10 % entre lecturas consecutivas; con el ADS1115 se
mueve décimas.

El driver del ADS1115 está escrito en `battery_module.cpp` (unas 60 líneas sobre
`Wire`), así que **no hay que instalar ninguna librería**. Son dos registros: se
escribe la configuración, se espera 8 ms y se lee el resultado.

> **Si en el futuro añades la microSD**: en modo 1 bit ocupa GPIO2/14/15 (+GPIO4,
> compartido con el flash) y el I2C tendría que irse a **GPIO1/GPIO3**,
> sacrificando el monitor serie. **GPIO12 y GPIO13 no sirven para I2C**: GPIO12
> es el strapping MTDI y un pull-up ahí impide arrancar el chip.

---

## 3.bis Modos de red

`WIFI_NET_MODE` en `config.h` decide cómo se conecta el módulo:

| Modo | Qué hace |
|---|---|
| `WIFI_NET_STATION` | Solo cliente del router. Menor consumo. |
| `WIFI_NET_AP` | El ESP32 **crea su propia red**. No necesita router. |
| `WIFI_NET_AP_FALLBACK` | Intenta el router y, si no lo consigue en 20 s, levanta su red. **Por defecto.** |

### Conectarse a la red propia del ESP32

1. En el móvil, WiFi → conéctate a la red cuyo nombre pusiste en `WIFI_AP_SSID`
   (por defecto en la plantilla: `ESP32CAM-Monitor`)
2. La contraseña es la de `WIFI_AP_PASSWORD`
3. Abre `http://192.168.4.1/`

El nombre y la contraseña están en **`secrets.h`** (ver §6.1); la IP, el canal y
el número máximo de clientes, en `config.h` (`WIFI_AP_IP`, `WIFI_AP_CHANNEL`,
`WIFI_AP_MAX_CLIENTS`). La contraseña necesita **8 caracteres como mínimo**; con
menos, la red se crea abierta.

> **Android**: al no haber Internet en esa red, el móvil preguntará si quieres
> seguir conectado o volver a los datos móviles. Hay que decirle que **se quede**,
> o desactivar temporalmente los datos móviles. Si no, te expulsa solo a los
> pocos segundos y parece que el ESP32 ha fallado.

### El coste en batería

En modo AP el ESP32 *es* el punto de acceso: tiene que emitir balizas cada
~100 ms sin parar, así que **la radio no puede dormir**. El *modem sleep*
(`WiFi.setSleep(true)`), que ahorra ~35 mA, es un mecanismo de cliente y no
existe aquí. El firmware lo detecta y lo desactiva solo.

| | STA | SoftAP |
|---|---|---|
| Consumo en reposo | ~85 mA | ~130–160 mA |
| Autonomía en reposo | ~70 h | **~40 h** |
| Internet en el móvil mientras miras | Sí | No |

Por eso el modo por defecto es **AP_FALLBACK** y no AP puro: en casa se conecta
al router y consume poco; fuera de casa levanta su red sola.

---

## 4. Cableado

### 4.1 ADS1115 y divisor

```
                 ┌──────────────┐
   pack 18650 +──┤ R_TOP  100k  ├──┬─────────────► A0 del ADS1115
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

Sirve para las tres sin recablear. Fuga del divisor: 20 µA, despreciable.

### 4.2 Alimentación — esto importa más de lo que parece

La ESP32-CAM tiene un LDO **AMS1117** de 5 V → 3,3 V. Un LDO no es un
convertidor: tira a calor todo lo que sobra. A 5 V de entrada su rendimiento es
**3,3/5 = 66 %**. Si además usas un *boost* de 3,7 V → 5 V (~88 %), el
rendimiento total es **0,88 × 0,66 ≈ 58 %**: pierdes el 42 % del pack en calor.

| Opción | Dónde conectas | Rendimiento | Autonomía relativa |
|---|---|---|---|
| **A** (fácil) | boost 3,7→5 V al pin `5V` | ~58 % | 1,0× |
| **B** (recomendada) | buck-boost 3,7→3,3 V al pin `3V3` | ~90 % | **1,55×** |

La opción B salta el LDO de la placa. Necesitas un buck-**boost** (no un simple
buck) porque la celda va de 4,2 V a 3,3 V y cruza los 3,3 V de salida. Un
TPS63020 o un MT3608+LDO valen.

En cualquiera de las dos, pon un **condensador de 470–1000 µF** lo más cerca
posible del pin de alimentación: los picos de TX del WiFi son de 300–500 mA
durante microsegundos y son la causa nº 1 de reinicios por brownout con pilas.

### 4.3 Protección del pack

Dos 18650 en paralelo deben ir **con BMS 1S** (protección de sobredescarga y
cortocircuito), y antes de ponerlas en paralelo hay que **cargarlas al mismo
voltaje** — si no, la más cargada descarga violentamente sobre la otra.

El firmware añade una segunda red de seguridad: si el pack baja de
`BATTERY_CRITICAL_PERCENT` durante 5 lecturas seguidas, apaga la radio y entra en
deep sleep permanente (`BATTERY_CRITICAL_DEEPSLEEP`).

---

## 5. Autonomía real con esta arquitectura

### 5.1 Por qué el deep sleep no es viable aquí

El deep sleep apaga la radio y la RAM. Al despertar, el ESP32 **arranca desde
cero**: reconecta al WiFi (2–4 s), pide IP por DHCP y vuelve a levantar el
servidor. Durante todo ese tiempo el dispositivo **no tiene IP y no responde a
nada**. Un dashboard que sólo contesta 3 segundos de cada 300 no es un dashboard.

Es incompatible por diseño, no por implementación: *servidor siempre disponible*
y *radio apagada* son objetivos opuestos. Lo que sí se puede hacer es todo lo de
abajo, que este firmware ya aplica.

### 5.2 Lo que sí ahorra (implementado)

| Técnica | Dónde | Ahorro |
|---|---|---|
| **Modem sleep WiFi** (`WiFi.setSleep(true)`) | `.ino` | ~35 mA. La radio duerme entre balizas DTIM del router y despierta a escuchar. **No se pierde la asociación**: el servidor sigue respondiendo, con ~100 ms más de latencia en el primer paquete. |
| **CPU a 80 MHz en reposo** | `power_tick()` | ~20 mA. Sube a 160 MHz sola cuando hay stream. |
| **Snapshot en vez de stream** | modo híbrido | **~120 mA**. Es de lejos el mayor ahorro. |
| **Límite de 8 fps + VGA en el stream** | `config.h` | ~25 mA frente a MJPEG libre en SVGA. |
| **TX a 17 dBm** | `config.h` | ~10 mA de media y picos más suaves. |
| **Flash LED siempre apagado** | `CAM_FLASH_ENABLED 0` | 150 mA cuando se usaba. |
| **PWDN del sensor en reposo** (opcional) | `POWER_CAMERA_SLEEP_IDLE` | 40–60 mA, a cambio de ~350 ms por foto. Off por defecto. |
| **Light sleep automático** (opcional) | `POWER_AUTO_LIGHT_SLEEP` | 30–50 mA más, pero puede corromper frames con la cámara activa. Actívalo sólo con `VIDEO_MODE_SNAPSHOT`. |

### 5.3 Números

Consumo típico en un AI-Thinker con este firmware (raíl de 3,3 V):

| Estado | Corriente | Potencia |
|---|---|---|
| Reposo, nadie mirando (modem sleep, 80 MHz) | ~85 mA | 0,28 W |
| Dashboard abierto, foto cada 5 s | ~110 mA | 0,36 W |
| **Directo** MJPEG VGA a 8 fps | ~215 mA | 0,71 W |
| MJPEG SVGA sin límite de fps | ~290 mA | 0,96 W |

**Autonomía estimada según el pack:**

| Escenario | 4× AA alcalina | 4× AA NiMH | 2× 18650 + buck-boost |
|---|---|---|---|
| Nadie mira el dashboard | ~13 h | ~19 h | ~70 h |
| Dashboard abierto (modo snapshot) | ~10 h | ~15 h | ~55 h |
| **Directo permanente** | **~5 h** | ~7 h | ~28 h |

Las pilas AA rinden bastante menos de lo que sugiere su etiqueta, por dos
motivos que se suman:

**Resistencia interna.** El módulo pide picos de 300–500 mA al transmitir. Una
AA alcalina a esa corriente entrega **menos de la mitad** de los mAh nominales
(efecto Peukert), y su tensión se hunde en cada pico. Las NiMH aguantan los
picos mucho mejor: es la diferencia entre las dos primeras columnas.

**El regulador.** 6 V entrando al AMS1117 para sacar 3,3 V son un **55 % de
rendimiento**: más de la mitad de las pilas se va en calor. Con Li-ion y
buck-boost al pin `3V3` se llega al 90 %, y de ahí la tercera columna.

**Uso realista en modo híbrido** — 30 min de directo + 2 h de dashboard abierto +
el resto en reposo, cada día:

> potencia media = (0,5 × 0,71 + 2 × 0,36 + 21,5 × 0,28) / 24 = **0,30 W**
> → **~67 horas ≈ 2,8 días** por carga.

Frente a las ~28 h del streaming continuo, el modo híbrido **multiplica la
autonomía por 2,4** y sigue dándote vídeo en vivo cuando lo necesitas. Ése es el
motivo de la arquitectura.

### 5.4 Para comparar: qué te perderías

Si renunciaras al dashboard permanente y usaras deep sleep con una foto cada
5 minutos (subida a un servidor externo), el consumo medio caería a ~0,035 W:
**unos 24 días** por carga. Es 10 veces más, pero ya no es este proyecto —
necesitarías el servidor externo que precisamente querías evitar.

Si algún día quieres acercarte a eso sin servidor externo, la vía es
`VIDEO_MODE_SNAPSHOT` + `POWER_AUTO_LIGHT_SLEEP 1` +
`POWER_CAMERA_SLEEP_IDLE 1`: el dashboard sigue vivo y baja a ~45 mA de reposo
(≈ 130 h), a costa de perder el vídeo en vivo y de ~0,5 s de latencia por foto.

---

## 6. Compilar y grabar con el Arduino IDE

### 6.1 Requisitos

#### Credenciales

`config.h` **no contiene las credenciales**: están en `secrets.h`, que git
ignora, para poder publicar el repositorio sin regalar la contraseña del WiFi.

Si acabas de clonar el proyecto, ese fichero no existe y la compilación falla a
propósito con un mensaje claro. Créalo desde la plantilla:

```powershell
cd esp32cam_live_monitor
copy secrets.h.example secrets.h
```

Y rellena los cuatro valores: `WIFI_SSID`, `WIFI_PASSWORD`, `WIFI_AP_SSID` y
`WIFI_AP_PASSWORD`. La red del router debe ser de **2,4 GHz** — el ESP32 no ve
las de 5 GHz.

> Es la única excepción a la regla de «toda la configuración en `config.h`», y
> es deliberada.

#### Soporte de placas

Solo el soporte de placas ESP32 de Espressif, que **ya tienes instalado** si
compilaste el sketch anterior:

> Archivo → Preferencias → *Gestor de URLs Adicionales de Tarjetas*:
> `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
> y luego Herramientas → Placa → Gestor de Tarjetas → buscar **esp32**.

**Ninguna librería que instalar.** Si el Gestor de Librerías te pide algo, es que
falta un fichero del sketch en la carpeta.

### 6.2 Ajustes en Herramientas

| Opción | Valor |
|---|---|
| Placa | **AI Thinker ESP32-CAM** |
| Partition Scheme | **Huge APP (3MB No OTA/1MB SPIFFS)** |
| PSRAM | **Enabled** |
| CPU Frequency | 240 MHz (el firmware la baja solo en marcha) |
| Upload Speed | 921600 (baja a 115200 si falla) |
| Puerto | el COM de tu adaptador USB-serie |

El *Partition Scheme* importa: con el esquema por defecto el binario (cámara +
WiFi + servidor HTTP + dashboard) **no cabe**.

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

Si el FTDI no da bastante corriente, alimenta la placa aparte (5 V externos) y
deja sólo TX/RX/GND al FTDI. **Masa común siempre.**

### 6.4 Subir

Abre `esp32cam_live_monitor.ino` (doble clic) y pulsa **Subir**. Eso es todo:
**no hay segunda subida** de sistema de ficheros, el dashboard va dentro.

### 6.5 Monitor serie

Herramientas → Monitor Serie, a **115200 baudios**.

```
[     210] === ESP32-CAM Monitor ===
[     215] POWER: CPU a 160 MHz
[     620] CAM: lista (800x600, PSRAM=si)
[     690] BAT: ADS1115 ok (config=0x8583). Divisor x2.000, pack 6000 mAh, 1 celda(s) en serie
[     780] BAT: 3.987 V (raw 3.985) -> 79% [ok]
[    3120] WIFI: conectado. IP=192.168.1.37  RSSI=-58 dBm
[    3125] POWER: modem sleep WiFi activado
[    3180] WEB: control en http://192.168.1.37:80/
[    3190] WEB: stream en http://192.168.1.37:81/stream
[    3195] LISTO -> http://192.168.1.37/  (o http://esp32cam.local/)
```

---

## 7. Acceder al dashboard

Desde cualquier navegador **de la misma red WiFi**:

```
http://192.168.1.37/        ← la IP que imprime el monitor serie
http://esp32cam.local/      ← mDNS (Windows 10+, macOS, iOS; en Android suele fallar)
```

En el dashboard:

- **Foto fija** que se refresca sola cada 5 s.
- Botón **«Ver en vivo»** → activa el MJPEG. El indicador superior pasa a rojo
  *EN DIRECTO* para recordarte que estás gastando batería.
- Botón **«Detener directo»** → corta la conexión MJPEG y vuelve al modo de bajo
  consumo. El ESP32 lo detecta solo al cerrarse el socket.
- **Nivel de batería**: voltaje, porcentaje (curva real de descarga Li-ion, no
  una regla de tres) y barra que pasa de verde a ámbar (≤20 %) y a rojo (≤10 %).
  Se actualiza cada 60 s.

Para verlo desde fuera de casa necesitarías abrir puertos en el router o un túnel
tipo Tailscale/Cloudflare: **no lo expongas a Internet directamente**, el
servidor no tiene autenticación ni HTTPS.

---

## 8. Ajustes frecuentes en `config.h`

| Quiero… | Toca |
|---|---|
| Más autonomía | `VIDEO_MODE_SNAPSHOT`, `SNAPSHOT_REFRESH_MS 15000`, `POWER_CAMERA_SLEEP_IDLE 1` |
| Vídeo más fluido | `STREAM_MAX_FPS 0`, `CAM_FRAMESIZE_STREAM FRAMESIZE_SVGA` |
| Imagen más nítida | `CAM_JPEG_QUALITY_SNAPSHOT 10`, `CAM_FRAMESIZE_SNAPSHOT FRAMESIZE_UXGA` |
| Cámara del revés | `CAM_VFLIP 1` y/o `CAM_HMIRROR 1` |
| IP siempre la misma | `WIFI_USE_STATIC_IP 1` + los cuatro `WIFI_STATIC_*` |
| Corregir el voltaje medido | `BATTERY_CALIBRATION` = V_multímetro / V_mostrado |
| Otro divisor de resistencias | `BATTERY_R_TOP_OHM` / `BATTERY_R_BOTTOM_OHM` |
| Reinicios por brownout con pilas | primero el condensador; si persiste, `POWER_DISABLE_BROWNOUT 1` |

Después de tocar `config.h`, **vuelve a subir el sketch**: es código compilado.

---

## 9. Problemas típicos

| Síntoma | Causa habitual |
|---|---|
| `#error "Falta secrets.h..."` | No has creado `secrets.h`. Copia `secrets.h.example` y rellénalo (§6.1). |
| `Sketch too big` al compilar | Falta poner *Partition Scheme* → **Huge APP**. |
| `esp_camera_init fallo (0x105)` | Cable plano mal insertado o alimentación insuficiente. Prueba con `CAM_XCLK_FREQ_HZ 10000000`. |
| `Camera probe failed` o cuelgue al arrancar | PSRAM sin habilitar en Herramientas. |
| **No se ve la imagen pero sí el dashboard** | Abre `/test`: es un `<img>` sin JavaScript. Si ahí sale la foto, el firmware va bien y el fallo está en el navegador. Si no sale, mira `cameraReady` en `/status` y la línea `CAM:` del monitor serie. |
| `ADS1115 no responde en 0x48` | `ADDR` sin conectar (debe ir a GND), faltan pull-ups, o SDA/SCL intercambiados. |
| Voltaje leído = la mitad del real | `BATTERY_R_TOP_OHM`/`BATTERY_R_BOTTOM_OHM` no coinciden con las resistencias reales. |
| Voltaje leído 0,00 V | El divisor no tiene masa común con el ESP32. |
| `Failed to connect… Timed out waiting for packet header` | Falta el puente `IO0`–`GND`, o no pulsaste RST antes de subir. |
| Reinicios al conectar el WiFi | Falta el condensador de 470–1000 µF; o el FTDI no da corriente suficiente. |
| El directo se corta a los pocos segundos | Señal WiFi débil (mira `RSSI` en `/status`) o el router limita la sesión. Baja `CAM_FRAMESIZE_STREAM` a QVGA. |
| No arranca tras cablear el I2C | Revisa que no hayas usado GPIO12: con pull-up impide el arranque. |

---

## 10. El sketch anterior

`esp32cam_fotos_stream.ino` (fotos a microSD + streaming) está archivado en
`../legacy/` junto con su README. Este proyecto **no incluye el guardado en
microSD**: esos pines (GPIO14/15) son ahora el bus I2C de la batería.
