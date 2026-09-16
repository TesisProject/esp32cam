/* ============================================================================
 *  config.h  -  CONFIGURACION CENTRAL DEL PROYECTO
 *  ESP32-CAM Live Monitor (AI-Thinker)  -  version Arduino IDE
 *
 *  TODO lo configurable vive aqui. Ningun otro fichero contiene valores
 *  hardcodeados: ni credenciales, ni pines, ni umbrales, ni intervalos.
 *  El propio dashboard (dashboard_html.h) lee estos valores en tiempo de
 *  ejecucion desde el endpoint /config, asi que tampoco hay nada duplicado
 *  en el HTML.
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
#define DEVICE_NAME            "ESP32-CAM Monitor"
#define DEVICE_HOSTNAME        "esp32cam"      // -> http://esp32cam.local/
#define ENABLE_MDNS            1               // 1 = publicar .local por mDNS

/* ===========================================================================
 * 2. WIFI
 * ==========================================================================*/
/* --- Como se conecta el dispositivo --------------------------------------
 *
 *  WIFI_NET_STATION : solo cliente del router. Menos consumo, hay Internet
 *                     en el movil mientras miras, pero dependes del router.
 *  WIFI_NET_AP      : el ESP32 crea SU PROPIA red. No necesita router y la IP
 *                     es siempre fija, pero la radio NO puede dormir (tiene
 *                     que emitir balizas sin parar): ~50 mA mas de consumo.
 *  WIFI_NET_AP_FALLBACK : intenta el router y, si no lo consigue, levanta su
 *                     propia red. Lo mejor de los dos.   <-- por defecto
 *
 *  NOTA: no uses los nombres WIFI_MODE_STA / WIFI_MODE_AP para esto: ya
 *  existen como enum de ESP-IDF y chocarian.
 * ------------------------------------------------------------------------*/
#define WIFI_NET_STATION        0
#define WIFI_NET_AP             1
#define WIFI_NET_AP_FALLBACK    2

#define WIFI_NET_MODE          WIFI_NET_AP

/* --- Credenciales: EXCEPCION a la regla de "todo en config.h" ------------
 *
 *  Las cuatro credenciales (SSID y contrasena del router, y las de la red
 *  propia del ESP32) viven en secrets.h, que git NO versiona. Asi se puede
 *  publicar el repositorio sin regalar la contrasena del WiFi de casa.
 *
 *  Es la unica configuracion que no esta en este fichero, y es a proposito.
 *
 *  Si acabas de clonar el repositorio:
 *      copy secrets.h.example secrets.h      (y rellena los cuatro valores)
 * ------------------------------------------------------------------------*/
#if defined(__has_include)
  #if __has_include("secrets.h")
    #include "secrets.h"
  #endif
#else
  #include "secrets.h"
#endif

#if !defined(WIFI_SSID) || !defined(WIFI_PASSWORD) ||     !defined(WIFI_AP_SSID) || !defined(WIFI_AP_PASSWORD)
  #error "Falta secrets.h (o le faltan valores). Copia secrets.h.example a secrets.h y rellena WIFI_SSID, WIFI_PASSWORD, WIFI_AP_SSID y WIFI_AP_PASSWORD."
#endif

/* --- Resto de la red propia del ESP32 (modos AP y AP_FALLBACK) -----------
 *  El nombre y la contrasena estan en secrets.h. La contrasena necesita 8
 *  caracteres o mas; con menos (o con "") la red se crea ABIERTA.
 *  Conectate desde el movil a esa red y abre http://192.168.4.1/
 *
 *  Aviso: al no haber Internet, Android suele preguntar si quieres seguir
 *  conectado o volver a los datos moviles. Hay que decirle que se quede.  */
#define WIFI_AP_CHANNEL        1       // 1, 6 u 11 son los que no se solapan
#define WIFI_AP_MAX_CLIENTS    4
#define WIFI_AP_HIDDEN         0       // 1 = no anunciar el nombre de la red

#define WIFI_AP_IP             192, 168, 4, 1
#define WIFI_AP_GATEWAY        192, 168, 4, 1
#define WIFI_AP_SUBNET         255, 255, 255, 0

#define WIFI_CONNECT_TIMEOUT_MS   20000UL   // espera maxima al conectar
#define WIFI_RETRY_INTERVAL_MS    15000UL   // reintento si se cae el enlace
#define WIFI_TX_POWER_DBM         17        // 8..20 dBm. Bajarlo ahorra bateria
                                            // a costa de alcance. 20 = maximo.

/* --- IP fija (opcional). 1 = fija, 0 = DHCP -------------------------------*/
#define WIFI_USE_STATIC_IP     0
#define WIFI_STATIC_IP         192, 168, 1, 50
#define WIFI_STATIC_GATEWAY    192, 168, 1, 1
#define WIFI_STATIC_SUBNET     255, 255, 255, 0
#define WIFI_STATIC_DNS        8, 8, 8, 8

/* ===========================================================================
 * 3. SERVIDOR WEB
 *
 *  Se usan DOS servidores HTTP, igual que el ejemplo oficial CameraWebServer:
 *   - Puerto 80  : dashboard, /battery, /capture, /config, /status
 *   - Puerto 81  : /stream (MJPEG)
 *  Motivo: un handler MJPEG bloquea el hilo del servidor mientras emite. Si
 *  compartiera puerto con /battery, el fetch de bateria se quedaria colgado.
 * ==========================================================================*/
#define HTTP_PORT              80
#define HTTP_STREAM_PORT       81

#define HTTP_MAX_CLIENTS       6        // sockets simultaneos en el puerto 80
                                        // Un navegador abre hasta 6 conexiones
                                        // en paralelo; con menos, httpd las va
                                        // cerrando y reabriendo (mas latencia).
#define HTTP_STACK_SIZE        8192

/* Rutas (si las cambias aqui, el HTML las recoge solo via /config) */
#define ROUTE_INDEX            "/"
#define ROUTE_BATTERY          "/battery"
#define ROUTE_SNAPSHOT         "/capture"
#define ROUTE_STREAM           "/stream"
#define ROUTE_CONFIG           "/config"
#define ROUTE_STATUS           "/status"
#define ROUTE_TEST             "/test"   // pagina de prueba sin JavaScript

/* El dashboard va compilado DENTRO del firmware (dashboard_html.h), no en un
 * sistema de ficheros: un unico flasheo y ningun plugin extra en el IDE.
 * Para editarlo se toca dashboard_html.h y se vuelve a grabar el sketch. */

/* ===========================================================================
 * 4. MODO DE VIDEO  (el factor n.1 en la autonomia)
 *
 *   VIDEO_MODE_SNAPSHOT : solo imagenes fijas bajo demanda. Sin /stream.
 *   VIDEO_MODE_STREAM   : MJPEG continuo en cuanto se abre la pagina.
 *   VIDEO_MODE_HYBRID   : snapshot periodico + boton "LIVE" que activa el
 *                         stream solo mientras lo estas mirando.  <-- elegido
 * ==========================================================================*/
#define VIDEO_MODE_SNAPSHOT    0
#define VIDEO_MODE_STREAM      1
#define VIDEO_MODE_HYBRID      2

#define VIDEO_MODE             VIDEO_MODE_HYBRID

/* Cada cuanto refresca el dashboard la imagen fija (modo snapshot/hibrido) */
#define SNAPSHOT_REFRESH_MS    5000UL

/* Limite de FPS del stream MJPEG. 0 = sin limite (mas fluido, mas consumo).
 * Con 8 fps el consumo medio baja ~20 % frente a stream libre.             */
#define STREAM_MAX_FPS         8
#define STREAM_MAX_CLIENTS     1        // clientes MJPEG simultaneos

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
#define CAM_FRAMESIZE_SNAPSHOT FRAMESIZE_SVGA   // foto fija del dashboard
#define CAM_FRAMESIZE_STREAM   FRAMESIZE_VGA    // video en vivo (mas ligero)
#define CAM_JPEG_QUALITY_SNAPSHOT 12
#define CAM_JPEG_QUALITY_STREAM   15
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
 *  Pack elegido: 2 x 18650 Li-ion EN PARALELO (1S2P) -> 3.7 V nom, ~6000 mAh.
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

/* Tensiones de pack (solo informativas, para el dashboard). El porcentaje
 * real sale de la curva de battery_module.cpp, no de estos dos valores. */
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

#define BATTERY_LOW_PERCENT    20       // aviso amarillo en el dashboard
#define BATTERY_CRITICAL_PERCENT 10     // aviso rojo

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
 * 7. DASHBOARD  (valores que el HTML lee desde /config)
 * ==========================================================================*/
#define DASHBOARD_BATTERY_REFRESH_MS  60000UL   // fetch a /battery cada 60 s

/* ===========================================================================
 * 8. GESTION DE ENERGIA
 *
 *  El deep sleep NO es viable mientras el servidor deba responder: al
 *  dormir se apaga la radio y se pierde la asociacion WiFi. Lo que si
 *  se puede hacer sin perder el servidor es lo de abajo.
 * ==========================================================================*/
#define POWER_WIFI_MODEM_SLEEP   1    // WiFi.setSleep(true): la radio duerme
                                      // entre balizas DTIM. Ahorra 30-40 mA.
                                      // Anade ~100 ms de latencia al primer
                                      // paquete. Muy recomendable.

#define POWER_CPU_FREQ_ACTIVE_MHZ  160  // 240/160/80. 160 buen compromiso.
                                        // Sube a 240 si las fotos tardan mucho:
                                        // la compresion JPEG es puro calculo.
#define POWER_CPU_FREQ_IDLE_MHZ     80  // frecuencia cuando no hay nadie mirando
#define POWER_DYNAMIC_CPU_FREQ       1  // 1 = alternar entre las dos de arriba

/* Cuanto se mantiene la CPU a la frecuencia alta despues de la ULTIMA peticion
 * HTTP. Sin esto, cada foto suelta se codificaria a 80 MHz (el doble de lento)
 * porque entre peticion y peticion no hay "actividad" que detectar.
 * Subirlo hace el dashboard mas agil; bajarlo ahorra bateria.              */
#define POWER_ACTIVE_HOLD_MS    15000UL

/* Light sleep automatico (tickless idle). Requiere CONFIG_PM_ENABLE en el
 * build de Arduino; si no esta, el firmware lo detecta y sigue sin ello.
 * Por defecto 0: con la camara inicializada puede provocar frames corruptos
 * en algunos modulos. Activalo solo si usas VIDEO_MODE_SNAPSHOT.           */
#define POWER_AUTO_LIGHT_SLEEP     0

/* Pausa del loop() cuando no hay stream activo: cede CPU al idle task, que
 * es quien dispara el modem sleep.                                         */
#define POWER_IDLE_DELAY_MS       50

/* Apagar el sensor OV2640 (pin PWDN) cuando no se usa. Ahorra ~40-60 mA en
 * modo snapshot, pero anade ~350 ms de arranque a cada foto.
 * Mientras haya un cliente viendo el stream se ignora.                     */
#define POWER_CAMERA_SLEEP_IDLE    0
#define POWER_CAMERA_IDLE_MS    30000UL

/* Desactivar el detector de brownout. En pilas, las caidas de tension de los
 * picos de TX del WiFi pueden dispararlo y reiniciar la placa. Desactivarlo
 * evita los reinicios pero enmascara una alimentacion mala: usalo solo si ya
 * tienes un condensador de 470-1000 uF en el rail de 5 V.                   */
#define POWER_DISABLE_BROWNOUT     0

/* ===========================================================================
 * 8.bis LED DE ESTADO  (el LED ROJO integrado, GPIO33)
 *
 *  Funcionando con pilas no hay monitor serie: si algo falla, te quedas sin
 *  saber que pasa. Este LED cuenta el estado a base de parpadeos, y se lee
 *  sin conectar absolutamente nada.
 *
 *    solido            arrancando (camara + WiFi)
 *    1 parpadeo / 3 s  OK, conectado al router
 *    2 parpadeos / 3 s OK, sirviendo su propia red (modo AP)
 *    3 parpadeos / 3 s la CAMARA no arranco (el servidor web si funciona)
 *
 *  OJO: GPIO33 lleva el LED rojo de la placa, cableado entre 3V3 y el pin.
 *  Por eso es de logica invertida: nivel BAJO = encendido.
 *  Es tambien el unico ADC1 libre; si algun dia mides la bateria con un
 *  divisor ahi en vez de con el ADS1115, pon STATUS_LED_ENABLED a 0.
 * ==========================================================================*/
#define STATUS_LED_ENABLED      1
#define STATUS_LED_PIN         33
#define STATUS_LED_ACTIVE_LOW   1      // 1 = nivel bajo enciende

#define STATUS_LED_BLINK_MS   120      // duracion de cada destello
#define STATUS_LED_GAP_MS     220      // hueco entre destellos
#define STATUS_LED_CYCLE_MS  3000UL    // cada cuanto se repite el patron

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
