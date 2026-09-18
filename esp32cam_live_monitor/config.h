/* ============================================================================
 *  config.h  -  CONFIGURACION CENTRAL DEL PROYECTO
 *  ESP32-CAM Live Monitor (AI-Thinker)  -  version Arduino IDE
 *
 *  TODO lo configurable vive aqui. Ningun otro fichero contiene valores
 *  hardcodeados: ni credenciales, ni pines, ni umbrales, ni intervalos.
 *
 *  Este sketch no usa ninguna libreria externa: solo el core ESP32 de
 *  Espressif. El driver del ADS1115 va incluido en battery_module.cpp.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "esp_camera.h"

/* ===========================================================================
 * 1. IDENTIDAD DEL DISPOSITIVO
 * ==========================================================================*/
#define DEVICE_NAME            "ESP32-CAM ParkVision"
#define DEVICE_HOSTNAME        "esp32cam"      // nombre visible en el router

/* Version del firmware. Viaja en la telemetria (campo fw_version): con 3
 * unidades en campo es la unica forma de saber cual lleva que codigo sin ir
 * a leer el monitor serie de cada una. Subirla al cambiar el comportamiento. */
#define FW_VERSION             "1.1.0"

/* ===========================================================================
 * 2. WIFI  (solo modo estacion: un AP propio impediria dormir la radio)
 * ==========================================================================*/
/* --- Credenciales: EXCEPCION a la regla de "todo en config.h" ------------
 *
 *  El SSID/password del router y las credenciales de esta camara (DEVICE_ID,
 *  API_KEY, API_TOKEN) viven en secrets.h, que git NO versiona. Asi se puede
 *  publicar el repositorio sin regalar el WiFi ni las claves de la API.
 *
 *  Es la unica configuracion que no esta en este fichero, y es a proposito.
 *
 *  Si acabas de clonar el repositorio:
 *      copy secrets.h.example secrets.h      (y rellena los cinco valores)
 * ------------------------------------------------------------------------*/
#if defined(__has_include)
  #if __has_include("secrets.h")
    #include "secrets.h"
  #endif
#else
  #include "secrets.h"
#endif

#if !defined(WIFI_SSID) || !defined(WIFI_PASSWORD) ||     !defined(DEVICE_ID) || !defined(API_KEY) || !defined(API_TOKEN)
  #error "Falta secrets.h (o le faltan valores). Copia secrets.h.example a secrets.h y rellena WIFI_SSID, WIFI_PASSWORD, DEVICE_ID, API_KEY y API_TOKEN."
#endif

#define WIFI_CONNECT_TIMEOUT_MS   20000UL   // espera maxima al conectar; si no
                                            // conecta en este tiempo, se aborta
                                            // el ciclo y se vuelve a dormir (no
                                            // reintentar indefinidamente, gasta
                                            // bateria)
#define WIFI_TX_POWER_DBM         17        // 8..20 dBm. Bajarlo ahorra bateria
                                            // a costa de alcance. 20 = maximo.

/* --- IP fija (opcional). 1 = fija, 0 = DHCP -------------------------------*/
#define WIFI_USE_STATIC_IP     0
#define WIFI_STATIC_IP         192, 168, 1, 50
#define WIFI_STATIC_GATEWAY    192, 168, 1, 1
#define WIFI_STATIC_SUBNET     255, 255, 255, 0
#define WIFI_STATIC_DNS        8, 8, 8, 8

/* ===========================================================================
 * 3. RED Y SUBIDA  -  la Raspberry Pi actua de "fog node"
 *
 *  Cada ciclo: capturar 1 foto, subirla por POST a la Pi, dormir. La Pi
 *  procesa las imagenes (deteccion de plazas de parking) y las reenvia como
 *  JSON a la nube; el firmware no sabe nada de eso, solo manda el JPEG.
 * ==========================================================================*/
#define FOG_SERVER_HOST         "parkvision.local"
#define FOG_SERVER_PORT         8000
#define FOG_FRAMES_PATH         "/api/v1/camera/frames"

// Endpoint final:  http://<FOG_SERVER_HOST>:<FOG_SERVER_PORT><FOG_FRAMES_PATH>
//
// La camara NO se identifica en la URL ni en el cuerpo: el Fog la deduce de
// la pareja X-API-Key / X-API-Token que lleva cada unidad en su secrets.h, y
// devuelve el camera_id en la respuesta 202. La ruta antigua con el id dentro
// (/api/v1/cameras/<id>/frames) responde 405.

/* --- Resolucion del nombre ------------------------------------------------
 *  Un nombre .local sobrevive a que la Pi cambie de IP; una IP fija habria que
 *  ir a cambiarla a mano en las tres camaras. Por eso el host es un nombre.
 *
 *  El coste es que resolverlo cuesta radio encendida, y con pilas eso importa.
 *  De ahi las dos salvaguardas:
 *
 *  1. La IP resuelta se CACHEA en RTC memory, que sobrevive al deep sleep.
 *     Solo el primer ciclo tras un arranque en frio paga la consulta mDNS; los
 *     demas van directos a la IP. Si el POST falla por conexion, la cache se
 *     invalida y el ciclo siguiente vuelve a resolver: asi un cambio de IP de
 *     la Pi se arregla solo, sin tocar el firmware.
 *
 *  2. Si mDNS no contesta, se usa FOG_SERVER_FALLBACK_IP en vez de perder el
 *     ciclo entero. Manten aqui la ultima IP conocida de la Pi.
 *
 *  ESPmDNS viene con el core ESP32: no hay que instalar ninguna libreria.
 *
 *  Si FOG_SERVER_HOST no acaba en ".local" se usa tal cual y no se resuelve
 *  nada: poner una IP ahi arriba sigue funcionando y salta todo este camino. */
#define FOG_MDNS_ENABLED        1
#define FOG_MDNS_TIMEOUT_MS     2000
#define FOG_SERVER_FALLBACK_IP  "192.168.18.157"

/* Respuesta esperada: 202 Accepted con {"frame_id":N,"camera_id":"CAM-00X",
 * "status":"processing"}. La deteccion tarda ~20 s en la Pi y corre despues:
 * el dispositivo NO la espera, suelta el JPEG y se duerme. */
#define UPLOAD_ACCEPTED_CODE    202

/* Reintento inmediato solo para el 400 ("imagen vacia"): el fallo esta en el
 * frame, no en la red, asi que se descarta y se captura otro. Uno como mucho:
 * cada intento son ~0.5 s de radio encendida. 0 = nunca reintentar. */
#define UPLOAD_RETRY_ON_EMPTY   1

/* Errores de credencial o de alta (401 / 403 / 404): reintentar cada 5 min no
 * los arregla, solo gasta pilas. Se duerme este tiempo mas largo en su lugar.
 * El 403 (camara deshabilitada en el Fog) si puede resolverse solo, de ahi que
 * no se apague del todo el dispositivo. */
#define UPLOAD_ERROR_BACKOFF_MS 1800000UL   // 30 min

/* Tiempo en deep sleep entre capturas. 5 min es el valor de campo.
 * Para probar sin esperar, bajarlo a 30000UL (30 s) y VOLVER A SUBIRLO antes
 * de dejar la camara desatendida: a 30 s son 120 ciclos/hora en vez de 12. */
#define CAPTURE_INTERVAL_MS     300000UL   // 5 minutos entre capturas

/* --- Telemetria en el POST ------------------------------------------------
 *  El Fog acepta 13 campos de texto opcionales junto a la imagen, en el mismo
 *  multipart. Van gratis: aprovechan una peticion que ya se hace de todas
 *  formas. Un endpoint aparte costaria ~6 mAh/dia de radio extra.
 *
 *  El contrato acordado con el backend:
 *    - Todos opcionales. Si no hay dato, el campo no se envia (nunca vacio).
 *    - Enteros, no floats: milivoltios y milisegundos.
 *    - Un campo mal formado se descarta con warning; la imagen se guarda igual.
 *    - Campos desconocidos se ignoran: se puede ampliar sin tocar el backend.
 *
 *  Deliberadamente NO se envian battery_percent ni battery_state (son
 *  conclusiones derivadas de una curva que puede estar mal), ni frame_len ni
 *  battery_sag_mv (los calcula el Fog al recibir). Ver el documento de
 *  telemetria para el razonamiento completo.                                */
#define UPLOAD_TELEMETRY_ENABLED 1

/* ===========================================================================
 * 4. LED DE ESTADO  -  rojo integrado en GPIO33
 *
 *  En la AI-Thinker este LED NO esta cableado directo a alimentacion (por
 *  eso no se ve nada al conectar la placa aunque si llegue corriente): hay
 *  que forzar el pin desde el firmware. Es activo en BAJO.
 *
 *  Se enciende al empezar el ciclo despierto y se apaga justo antes de
 *  esp_deep_sleep_start(), para no gastar bateria durante el deep sleep.
 *  Mismo GPIO33 que aparece en las secciones 5 y 6 como el unico ADC1 libre:
 *  no hay conflicto porque aqui se usa como salida digital, no como entrada
 *  analogica, y la camara no lo ocupa.
 * ==========================================================================*/
#define STATUS_LED_ENABLED      1
#define STATUS_LED_PIN          33
#define STATUS_LED_ACTIVE_LOW   1      // 1 = LOW enciende (placas AI-Thinker)

/* ===========================================================================
 * 5. CAMARA OV2640  -  PINES AI-THINKER  (NO TOCAR salvo otra placa)
 *
 *  OJO: la camara ocupa GPIO 0,5,18,19,21,22,23,25,26,27,32,34,35,36,39.
 *  Eso incluye GPIO21 (Y5) y GPIO22 (PCLK): NO estan libres en esta placa.
 *  Tambien ocupa TODOS los pines de ADC1 menos GPIO33 (LED rojo integrado),
 *  y ADC2 es inutilizable con WiFi activo -> de ahi el ADC externo por I2C.
 * ==========================================================================*/
#define CAM_PIN_PWDN           32
#define CAM_PIN_RESET          -1
#define CAM_PIN_XCLK            0
#define CAM_PIN_SIOD           26      // SCCB SDA (bus propio de la camara)
#define CAM_PIN_SIOC           27      // SCCB SCL
#define CAM_PIN_D7             35      // Y9
#define CAM_PIN_D6             34      // Y8
#define CAM_PIN_D5             39      // Y7
#define CAM_PIN_D4             36      // Y6
#define CAM_PIN_D3             21      // Y5
#define CAM_PIN_D2             19      // Y4
#define CAM_PIN_D1             18      // Y3
#define CAM_PIN_D0              5      // Y2
#define CAM_PIN_VSYNC          25
#define CAM_PIN_HREF           23
#define CAM_PIN_PCLK           22

#define CAM_PIN_FLASH_LED       4      // LED blanco de potencia (~150 mA)
#define CAM_FLASH_ENABLED       0      // 0 = nunca encender el flash (bateria)

#define CAM_XCLK_FREQ_HZ       20000000   // 20 MHz. 10 MHz = menos consumo

/* --- Calidad e imagen -----------------------------------------------------
 *  framesize: FRAMESIZE_QVGA(320x240) VGA(640x480) SVGA(800x600)
 *             XGA(1024x768) SXGA(1280x1024) UXGA(1600x1200)
 *  jpeg_quality: 10 (mejor) .. 63 (peor). Menos calidad = menos bytes = menos
 *  tiempo de radio encendida = mas bateria.                                */
#define CAM_FRAMESIZE_SNAPSHOT FRAMESIZE_SVGA   // foto que se sube a la Pi
#define CAM_JPEG_QUALITY_SNAPSHOT 12
#define CAM_FB_COUNT              2             // 2 = doble buffer en PSRAM

/* Ajustes del sensor */
#define CAM_VFLIP               0      // 1 = voltear vertical
#define CAM_HMIRROR             0      // 1 = espejo horizontal
#define CAM_BRIGHTNESS          0      // -2..2
#define CAM_CONTRAST            0      // -2..2
#define CAM_SATURATION          0      // -2..2

/* ===========================================================================
 * 6. BATERIA  -  ADC EXTERNO ADS1115 POR I2C
 *
 *  Pack: 4 pilas AA alcalinas EN SERIE (1S), un solo uso.
 *
 *  Por que ADC externo y no un divisor a un pin nativo:
 *  la camara ocupa 32/34/35/36/39 (todo ADC1 util) y ADC2 no funciona con
 *  WiFi encendido. El unico ADC1 libre seria GPIO33, pero lleva el LED rojo
 *  de la placa en paralelo y falsea la lectura salvo que desueldes su
 *  resistencia. El ADS1115 evita ese destrozo y ademas da 16 bits reales
 *  frente a los ~9 bits utiles y no lineales del ADC del ESP32.
 *
 *  I2C en GPIO14 (SDA) y GPIO15 (SCL): libres porque NO usamos microSD.
 *  Nota: GPIO15 es pin de strapping (MTDO); en alto = arranque normal, asi
 *  que el pull-up de 4k7 del bus I2C es seguro. GPIO12 NO valdria (strapping
 *  MTDI: un pull-up ahi impide arrancar).
 * ==========================================================================*/
#define BATTERY_ENABLED         1

#define BATTERY_I2C_SDA        14
#define BATTERY_I2C_SCL        15
#define BATTERY_I2C_FREQ_HZ    100000UL
#define BATTERY_ADS_ADDRESS    0x48    // ADDR a GND = 0x48
#define BATTERY_ADS_CHANNEL    0       // entrada A0 del ADS1115

/* Ganancia del amplificador interno (PGA) -> fondo de escala.
 *
 *    valor   fondo de escala   mV por bit
 *      0       +-6.144 V        0.1875
 *      1       +-4.096 V        0.125      <-- elegido
 *      2       +-2.048 V        0.0625
 *      3       +-1.024 V        0.03125
 *      4       +-0.512 V        0.015625
 *      5       +-0.256 V        0.0078125
 *
 *  Con el divisor 1:2 la entrada maxima es 4.2/2 = 2.10 V, asi que el PGA 1
 *  (4.096 V) es el seguro: el 2 recortaria a 2.048 V.
 *  Si cambias el PGA, cambia TAMBIEN el LSB de la tabla.                   */
#define BATTERY_ADS_PGA        1
#define BATTERY_ADS_LSB_MV     0.125f

/* Velocidad de conversion del ADS1115.
 *  0=8 SPS  1=16  2=32  3=64  4=128  5=250  6=475  7=860 SPS
 *  128 SPS (7.8 ms por muestra) es el valor por defecto del chip y va sobrado
 *  para medir una bateria.                                                 */
#define BATTERY_ADS_DATA_RATE  4

/* --- Divisor resistivo de entrada al ADS1115 ------------------------------
 *   VBAT ---[ R_TOP ]---+---[ R_BOTTOM ]--- GND
 *                       |
 *                       +---> A0 del ADS1115
 *
 *  ATENCION - limite absoluto del ADS1115: la entrada analogica NO puede
 *  pasar de VDD + 0.3 V. Con VDD = 3.3 V eso son 3.6 V. Pasarse estropea
 *  el chip, no solo falsea la lectura.
 *
 *  Con 220k/100k el factor es 3.2:
 *      4 pilas alcalinas nuevas  6.4 V -> 2.00 V en A0   (margen de sobra)
 *      4 NiMH recien cargadas    5.6 V -> 1.75 V en A0
 *      1 celda Li-ion llena      4.2 V -> 1.31 V en A0
 *  Sirve para las tres quimicas sin tocar nada.
 *
 *  Fuga: 6.4 V / 320k = 20 uA. Despreciable.
 *
 *  (Si vuelves a un pack de 1 celda Li-ion y quieres mas resolucion, puedes
 *   volver a 100k/100k -> factor 2.0, pero NUNCA con 4 pilas en serie.)   */
#define BATTERY_R_TOP_OHM      220000.0f
#define BATTERY_R_BOTTOM_OHM   100000.0f

/* Correccion fina del divisor resistivo.
 *
 *  El ADS1115 no es el problema: lleva referencia interna y su error de
 *  ganancia es del 0.1%. Lo que hay que corregir es la TOLERANCIA DE LAS
 *  RESISTENCIAS: con un 5% el factor 3.2 puede irse un +-7%, y es un error
 *  multiplicativo fijo, asi que un solo punto de referencia lo elimina casi
 *  entero.
 *
 *    calib = V_real / V_leido
 *
 *  Con multimetro: mides el pack y divides. Sin multimetro, sirve un pack
 *  RECIEN COMPRADO como referencia (ver BATTERY_CALIB_HELPER).            */
#define BATTERY_CALIBRATION    1.000f

/* Ayuda de calibracion sin multimetro.
 *
 *  Con 1, cada lectura imprime por serie que valor deberia tener
 *  BATTERY_CALIBRATION *si el pack que hay puesto estuviera recien estrenado*,
 *  tomando como referencia BATTERY_VOLT_FULL (tension en circuito abierto de
 *  una celda nueva x el numero de celdas).
 *
 *  Como usarlo:
 *    1. Pack nuevo, en reposo, sin haber transmitido todavia.
 *    2. Lee la linea "BATTERY_CALIBRATION sugerido" del monitor serie.
 *    3. Copia ese numero aqui arriba y vuelve a subir el sketch.
 *    4. Pon este flag a 0 cuando ya no lo necesites.
 *
 *  Precision: la referencia arrastra la dispersion de la propia pila
 *  (+-0.03 V/celda), asi que quedas en un error de ~3% en vez del ~8% de
 *  partida. No es calibracion de laboratorio, pero es mucho mejor que 1.000.
 *
 *  OJO: si el valor sugerido se aleja mucho de 1 (por debajo de 0.7 o por
 *  encima de 1.4) NO lo copies: eso no es tolerancia, es que BATTERY_R_TOP_OHM
 *  /BATTERY_R_BOTTOM_OHM no coinciden con las resistencias soldadas. Calibrar
 *  ahi taparia el error de verdad.                                         */
#define BATTERY_CALIB_HELPER   1

/* Muestreo y filtrado */
/* --- Medida bajo carga ----------------------------------------------------
 *  La tension en reposo engana: el pack lleva 5 min descansando y se ha
 *  recuperado. Lo que de verdad predice el fin de vida es cuanto se HUNDE
 *  durante un pico de TX del WiFi, porque esa caida es proporcional a la
 *  resistencia interna, que sube de forma monotona y no se recupera.
 *
 *  Se muestrea buscando el MINIMO mientras el WiFi se asocia (es cuando la
 *  radio transmite a plena potencia), no durante el POST: http.POST() es
 *  bloqueante y no deja hueco para leer el I2C sin montar una tarea aparte.
 *
 *  El ADS1115 se pone al maximo (860 SPS, 1.16 ms por conversion) solo para
 *  esto: a los 128 SPS normales cada conversion promedia 7.8 ms y se comeria
 *  el pico, que dura 1-2 ms.                                                */
#define BATTERY_LOAD_SAMPLING   1
#define BATTERY_LOAD_DATA_RATE  7        // 860 SPS, el mas rapido del chip

#define BATTERY_SAMPLES        8        // lecturas promediadas por medida
#define BATTERY_SAMPLE_DELAY_MS 5
#define BATTERY_READ_INTERVAL_MS 5000UL // cada cuanto relee el firmware
#define BATTERY_EMA_ALPHA      0.25f    // suavizado exponencial (0..1)

/* --- Quimica del pack ----------------------------------------------------
 *  Cada quimica tiene una curva de descarga COMPLETAMENTE distinta. Usar la
 *  del litio con pilas AA (o al reves) da porcentajes sin ningun sentido:
 *  con la curva Li-ion, 4 pilas marcarian 100 % hasta morirse.
 *
 *    BATTERY_CHEM_ALKALINE : pilas AA/AAA de un solo uso. 1.5 V por pila.
 *                            Curva inclinada, facil de medir. Se hunde mucho
 *                            bajo los picos de corriente del WiFi.
 *    BATTERY_CHEM_NIMH     : AA/AAA recargables. 1.2 V por pila. Curva MUY
 *                            plana: el porcentaje sera orientativo.
 *    BATTERY_CHEM_LIION    : 18650, LiPo. 3.7 V por celda.
 * ------------------------------------------------------------------------*/
#define BATTERY_CHEM_LIION     0
#define BATTERY_CHEM_NIMH      1
#define BATTERY_CHEM_ALKALINE  2

#define BATTERY_CHEMISTRY      BATTERY_CHEM_ALKALINE   // <-- CAMBIAR AQUI

#define BATTERY_CELLS_SERIES   4        // 4 pilas AA en SERIE
#define BATTERY_CAPACITY_MAH   2000     // capacidad nominal del pack

/* Tensiones de pack (solo informativas). El porcentaje real sale de la curva
 * de battery_module.cpp, no de estos dos valores. */
#if   BATTERY_CHEMISTRY == BATTERY_CHEM_ALKALINE
  #define BATTERY_VOLT_FULL    (1.60f * BATTERY_CELLS_SERIES)   // 6.40 V
  #define BATTERY_VOLT_EMPTY   (0.90f * BATTERY_CELLS_SERIES)   // 3.60 V
#elif BATTERY_CHEMISTRY == BATTERY_CHEM_NIMH
  #define BATTERY_VOLT_FULL    (1.40f * BATTERY_CELLS_SERIES)   // 5.60 V
  #define BATTERY_VOLT_EMPTY   (1.00f * BATTERY_CELLS_SERIES)   // 4.00 V
#else
  #define BATTERY_VOLT_FULL    (4.20f * BATTERY_CELLS_SERIES)
  #define BATTERY_VOLT_EMPTY   (3.30f * BATTERY_CELLS_SERIES)
#endif

#define BATTERY_LOW_PERCENT    20       // aviso en el log por serie
#define BATTERY_CRITICAL_PERCENT 10     // aviso critico

/* Proteccion del pack: si baja del umbral critico durante N lecturas
 * seguidas, el ESP32 entra en deep sleep permanente. 0 = solo avisar.
 *
 *  - Li-ion y NiMH: MANTENLO A 1. Descargarlas a fondo las dana.
 *  - Alcalinas: no hay nada que proteger (son de un solo uso), pero sigue
 *    siendo util para evitar el ciclo de reinicios por brownout cuando la
 *    tension cae y el regulador ya no da los 3.3 V.                        */
#define BATTERY_CRITICAL_DEEPSLEEP   1
#define BATTERY_CRITICAL_CONFIRMATIONS 5

/* ===========================================================================
 * 8. GESTION DE ENERGIA
 *
 *  Cada ciclo es: despertar, capturar, subir, dormir. El deep sleep apaga
 *  la CPU y la radio, pero NO corta la alimentacion de la camara (comparte
 *  el mismo rail de 3.3 V) ni conserva el estado de los pines por defecto:
 *  de ahi el PWDN + hold antes de dormir (ver camera_module.cpp).
 * ==========================================================================*/
#define POWER_CPU_FREQ_ACTIVE_MHZ  160  // 240/160/80. El ciclo es corto: mas
                                        // MHz = fotos/POST mas rapidos = menos
                                        // tiempo con la radio encendida.

/* Desactivar el detector de brownout. En pilas, las caidas de tension de los
 * picos de TX del WiFi pueden dispararlo y reiniciar la placa. Desactivarlo
 * evita los reinicios pero enmascara una alimentacion mala: usalo solo si ya
 * tienes un condensador de 470-1000 uF en el rail de 5 V.                   */
#define POWER_DISABLE_BROWNOUT     0

/* ===========================================================================
 * 9. DEPURACION
 * ==========================================================================*/
#define SERIAL_BAUD            115200
#define DEBUG_ENABLED          1

#if DEBUG_ENABLED
  #define LOG(fmt, ...)  Serial.printf("[%8lu] " fmt "\n", millis(), ##__VA_ARGS__)
#else
  #define LOG(fmt, ...)  do {} while (0)
#endif
