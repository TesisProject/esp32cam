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
// Endpoint final construido en tiempo de ejecucion:
//   http://<FOG_SERVER_HOST>:<FOG_SERVER_PORT>/api/v1/cameras/<DEVICE_ID>/frames
//
// Si el ESP32 no logra resolver ".local" en estacion, cambiar FOG_SERVER_HOST
// por la IP de la Pi directamente (p. ej. "192.168.1.20").

#define CAPTURE_INTERVAL_MS     30000UL    // 30 segundos entre capturas (deep sleep)

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

/* Correccion fina: mide con un multimetro y ajusta.
 *   calib = V_real / V_leido                                               */
#define BATTERY_CALIBRATION    1.000f

/* Muestreo y filtrado */
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
