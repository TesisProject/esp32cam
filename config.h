/* ============================================================================
 *  config.h  -  CONFIGURACION CENTRAL DEL PROYECTO (valores por defecto)
 *  ESP32-CAM ParkVision (AI-Thinker)  -  version Arduino IDE
 *
 *  TODA constante del firmware vive aqui. Ningun otro fichero contiene
 *  valores hardcodeados: ni pines, ni umbrales, ni intervalos.
 *
 *  Pero lo que es propio de cada placa o de cada despliegue (WiFi, host del
 *  Fog, credencial y, mas adelante, parametros de captura) NO se compila:
 *  vive en la NVS de la placa (ver src/settings/), se graba una vez por USB
 *  (scripts/provision.ps1) y la nube lo actualizara via el Fog. Lo que aqui
 *  aparezca para esos datos es solo el valor por defecto cuando la NVS no
 *  tiene uno: un mismo .bin sirve para todas las placas.
 *
 *  Este sketch no usa ninguna libreria externa: solo el core ESP32 de
 *  Espressif.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "esp_camera.h"

/* ===========================================================================
 * 1. IDENTIDAD DEL DISPOSITIVO
 * ==========================================================================*/
#define DEVICE_NAME            "ESP32-CAM ParkVision"
#define DEVICE_HOSTNAME        "esp32cam"      // nombre visible en el router

/* Version del firmware. Entero que sube en 1 con cada version publicada
 * (1, 2, 3...): es lo mas facil de comparar en el futuro cliente OTA ("hay
 * una mayor que la mia?") sin parsear cadenas tipo "1.4.0". Viaja al Fog
 * como fw_version en cada POST y sale en el log de arranque.
 * SUBIRLO antes de compilar cualquier binario que vaya a otra placa. */
#define FW_VERSION             3

/* ===========================================================================
 * 2. WIFI  (solo modo estacion: un AP propio impediria dormir la radio)
 * ==========================================================================*/
/* --- Red WiFi: en la NVS, NO aqui ----------------------------------------
 *
 *  El SSID y la contrasena se graban por USB en la NVS de cada placa
 *  (seccion 8 y scripts/provision.ps1). No estan en el binario: el mismo
 *  .bin sirve para cualquier red, y publicar el repo no regala el WiFi.
 *
 * --- Credencial de la camara: tambien en la NVS ---------------------------
 *
 *  El id de camara (CAM-001...), X-API-Key y X-API-Token se graban por USB
 *  en la misma linea de aprovisionamiento ("camera", seccion 8). Ya no hay
 *  secrets.h: un unico .bin para las tres camaras, sin secretos dentro.
 * ------------------------------------------------------------------------*/

/* Limites de la red que se aceptan al aprovisionar (802.11 / WPA2-PSK):
 * SSID de 1 a 32 bytes; contrasena WPA2 de 8 a 63 caracteres. Las redes
 * abiertas (sin contrasena) se rechazan a proposito: lo decidio el proyecto
 * para toda la configuracion WiFi, tambien en la nube. */
#define WIFI_SSID_MAX_LEN         32
#define WIFI_PASS_MIN_LEN         8
#define WIFI_PASS_MAX_LEN         63

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
/* Host y puerto del Fog: VALORES POR DEFECTO. Los reales se graban por USB
 * en la NVS (fogHost/fogPort del JSON de aprovisionamiento) y mandan sobre
 * estos. Cambiar esto NO afecta a una placa ya aprovisionada con host.
 * El host no se actualiza desde la nube a proposito: si llegara uno
 * equivocado, la camara ya no podria recibir la correccion.
 * El defecto es un nombre generico, no un secreto ni una IP concreta. */
#define FOG_SERVER_HOST_DEFAULT "parkvision.local"
#define FOG_SERVER_PORT_DEFAULT 8000
#define FOG_HOST_MAX_LEN        63     // nombre o IP; mas largo = error al aprovisionar
#define FOG_FRAMES_PATH         "/api/v1/camera/frames"
#define FOG_CONFIG_PATH         "/api/v1/camera/config"   // de aqui sale la oferta de firmware (E5)
#define FOG_PATH_MAX_LEN        63     // url relativa del .bin que da la config
// Endpoint final construido en tiempo de ejecucion:
//   http://<host de la NVS o defecto>:<puerto>/api/v1/camera/frames
//
// "camera" en singular y SIN id: el Fog sabe que camara es a partir de
// X-API-Key/X-API-Token (fog-application/src/vision/camera_router.py).
// /api/v1/cameras/<id>/frames existe en el Fog, pero solo como GET de
// administracion: un POST ahi recibe 405. Responde 202 Accepted (la
// deteccion se procesa despues de responder), no 201.
//
// Si el ESP32 no logra resolver ".local" en estacion, aprovisionar la placa
// con la IP de la Pi en fogHost (p. ej. "192.168.1.20").

/* 5 min en campo. Bajarlo a 30000 solo en el banco: a 30 s el ciclo activo
 * pasa a dominar y el consumo medio se multiplica por ~3-4 (README, seccion 5). */
#define CAPTURE_INTERVAL_MS     300000UL   // 5 minutos entre capturas (deep sleep)

/* ===========================================================================
 * 4. LED DE ESTADO  -  rojo integrado en GPIO33
 *
 *  En la AI-Thinker este LED NO esta cableado directo a alimentacion (por
 *  eso no se ve nada al conectar la placa aunque si llegue corriente): hay
 *  que forzar el pin desde el firmware. Es activo en BAJO.
 *
 *  Se enciende al empezar el ciclo despierto y se apaga justo antes de
 *  esp_deep_sleep_start(), para no gastar bateria durante el deep sleep.
 *  La camara no ocupa GPIO33 (ver seccion 5).
 * ==========================================================================*/
#define STATUS_LED_ENABLED      1
#define STATUS_LED_PIN          33
#define STATUS_LED_ACTIVE_LOW   1      // 1 = LOW enciende (placas AI-Thinker)

/* ===========================================================================
 * 5. CAMARA OV2640  -  PINES AI-THINKER  (NO TOCAR salvo otra placa)
 *
 *  OJO: la camara ocupa GPIO 0,5,18,19,21,22,23,25,26,27,32,34,35,36,39.
 *  Eso incluye GPIO21 (Y5) y GPIO22 (PCLK): NO estan libres en esta placa.
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

/* Degradacion si la PSRAM no se detecta: un frame SVGA no cabe en la RAM
 * interna, asi que se baja a QVGA (un JPEG de ~4-5 KB) en vez de no subir
 * nada. La deteccion en el Fog pierde precision a esta resolucion: evita
 * perder ciclos, no sustituye a arreglar la PSRAM. Lo que se aplique de
 * verdad viaja al Fog como framesize/jpeg_quality.                       */
#define CAM_FRAMESIZE_NO_PSRAM     FRAMESIZE_QVGA
#define CAM_JPEG_QUALITY_NO_PSRAM  15

/* Ajustes del sensor */
#define CAM_VFLIP               0      // 1 = voltear vertical
#define CAM_HMIRROR             0      // 1 = espejo horizontal
#define CAM_BRIGHTNESS          0      // -2..2
#define CAM_CONTRAST            0      // -2..2
#define CAM_SATURATION          0      // -2..2

/* ===========================================================================
 * 6. GESTION DE ENERGIA
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

/* --- Bateria: divisor 100k/100k desde el pack Li-ion 1S hasta GPIO13 -------
 *
 *  GPIO13 es ADC2: el WiFi se lo queda en cuanto arranca, asi que se lee
 *  ANTES de wifi_connect(). Al despertar del deep sleep la radio esta apagada.
 *
 *  Con solo el divisor en GPIO13, V13 = Vbat/2: 1500 mV con 3,0 V y
 *  2100 mV con 4,2 V. Todo el rango cae por debajo de ~2450 mV, donde el
 *  ADC a 12 dB es lineal, y 1 mV de error en el ADC son 2 mV de bateria.
 *  Requiere que la placa NO tenga montada R8 (47k de IO13 a 3V3, pull-up
 *  del slot SD): algunos lotes la traen y la medida no se puede corregir
 *  calibrando. Ver "Hardware" en CLAUDE.md.
 *
 *  Aun asi se calibra con DOS puntos y no con "x2": el ADC del ESP32 tiene
 *  error de ganancia y de offset propios de cada chip (decenas de mV aun con
 *  la calibracion de fabrica del eFuse), y las resistencias del 1 % tambien
 *  desplazan la razon. Precision esperable tras calibrar: +-20-30 mV,
 *  ~+-5 % de carga en la zona plana de la curva.
 *
 *  La calibracion es propia de cada placa: va en la NVS (clave bat_cal,
 *  "batteryCal" del JSON de aprovisionamiento). Estos son solo los DEFECTOS,
 *  la razon nominal 1/2.                                                    */
#define BATTERY_ADC_PIN              13
#define BATTERY_ADC_SAMPLES          32     // promedio: ~70 ms con la pausa
#define BATTERY_ADC_SAMPLE_GAP_MS    2      // deja recargar el 100 nF entre muestras

#define BATTERY_CAL_ADC_A_MV_DEFAULT 1500   // punto A: mV en GPIO13 ...
#define BATTERY_CAL_BAT_A_MV_DEFAULT 3000   //          ... con este pack (mV)
#define BATTERY_CAL_ADC_B_MV_DEFAULT 2100   // punto B
#define BATTERY_CAL_BAT_B_MV_DEFAULT 4200

/* Un punto de calibracion se rechaza al aprovisionar si los dos estan tan
 * cerca que la pendiente sale mal condicionada: con 5 mV de ruido en el ADC
 * y solo 30 mV entre puntos, la pendiente ya tendria un 17 % de error. */
#define BATTERY_CAL_MIN_BAT_SPAN_MV  300
#define BATTERY_CAL_MIN_ADC_SPAN_MV  30
#define BATTERY_CAL_ADC_MAX_MV       3300   // fondo de escala del pin

/* Fuera de esta ventana la lectura no es de un pack Li-ion 1S conectado:
 * placa alimentada por USB sin celda (VIN+ del MT3608 a 0 V: el ADC lee ~0
 * y esas muestras cuentan como fallidas) o sin divisor (GPIO13 al aire, la
 * lectura es ruido). Se envia battery_valid=false y NO se aplica el corte:
 * sin una medida fiable es mejor intentar el ciclo que dormir siempre. El
 * DW01 del TP4056 desconecta la celda por debajo de ~2,5 V. */
#define BATTERY_VALID_MIN_MV         2500
#define BATTERY_VALID_MAX_MV         4500

/* Corte por bateria baja: no se enciende ni la camara ni el WiFi. Con
 * histeresis: una celda Li-ion recupera 50-100 mV en reposo tras un corte,
 * asi que con un solo umbral la placa alternaria ciclo si / ciclo no justo
 * en el limite. Una vez cortada, solo vuelve a transmitir por encima de
 * BATTERY_RESUME_MV (la carga solar de dia la devuelve sola). */
#define BATTERY_CUTOFF_MV            3300
#define BATTERY_RESUME_MV            3400

/* Curva de descarga Li-ion en reposo (mV -> %), interpolada linealmente.
 * Solo para el log serie: al Fog viaja la tension, y la nube deriva el
 * porcentaje al leer, para que recalibrar la curva no invalide el historico.
 * 0 % = BATTERY_CUTOFF_MV: por debajo la camara ya no transmite. La MISMA
 * tabla va en cloud-service (docs/handoff-bateria-cloud.md) y en
 * tools/battery_monitor: cambiarla en los tres sitios a la vez.          */
#define BATTERY_CURVE_MV   { 3300, 3500, 3650, 3750, 3850, 4000, 4200 }
#define BATTERY_CURVE_PCT  {    0,    9,   28,   43,   59,   79,  100 }

/* ===========================================================================
 * 7. ALMACEN EN NVS  -  la configuracion propia de cada placa
 *
 *  src/settings/settings_module.* es la UNICA puerta a la NVS. El esquema
 *  completo (WiFi, Fog, credencial, captura, revision, OTA rechazada) existe
 *  desde E6; de momento se usan WiFi, Fog, credencial y bateria.
 *  Tamanos maximos de lo que se guarda: fijan el tamano de los blobs, asi
 *  que cambiarlos invalida lo ya guardado (ver SETTINGS_SCHEMA_VERSION).
 * ==========================================================================*/
#define SETTINGS_NVS_NAMESPACE     "parkvision"   // max. 15 caracteres (limite NVS)
/* Subirlo solo si cambia la forma de un blob: al arrancar, la placa borra
 * todo su namespace MENOS lo grabado por USB (WiFi, Fog, calibracion,
 * credencial). Con OTA eso pasa en todas las camaras a la vez y se pierde la
 * captura... Preferible: dato nuevo = clave nueva (asi se anadio bat_cal) y
 * dejar esto en 1. */
#define SETTINGS_SCHEMA_VERSION    1

/* CAM_CODE_MAX_LEN y API_SECRET_MAX_LEN fijan el blob "cred", que esta
 * CONGELADO (144 B, ver settings_module.h): no cambiarlos. */
#define CAM_CODE_MAX_LEN           15   // "CAM-001"
#define API_SECRET_MAX_LEN         63   // X-API-Key / X-API-Token del Fog
#define CAP_FRAMESIZE_MAX_LEN      7    // "SVGA", "QVGA"... (nombre del enum)

/* ===========================================================================
 * 8. APROVISIONAMIENTO POR USB  -  una vez por placa
 *
 *  Sin WiFi o sin credencial en la NVS la placa no puede hacer nada util:
 *  espera por serie una linea JSON (contrato C, la envia
 *  scripts/provision.ps1 o se pega en el monitor serie)
 *      {"wifi":{"ssid":"...","password":"..."},
 *       "camera":{"id":"CAM-001","apiKey":"...","apiToken":"..."},
 *       "fogHost":"...","fogPort":8000}
 *  la valida, la guarda y reinicia. Si no llega en PROV_WAIT_MS, duerme
 *  PROV_RETRY_SLEEP_MS y lo vuelve a intentar: en campo, sin USB, esperar
 *  indefinidamente agotaria las pilas.
 *
 *  Coste de una placa sin aprovisionar en campo: ~60 s despierta (~40 mA,
 *  sin radio) cada hora = ~0,7 mAh por hora. Es un estado de error, no de
 *  funcionamiento, pero las pilas duran semanas, no horas.
 *
 *  Re-aprovisionar: SOLO tras un arranque en frio (encendido, boton RST,
 *  pulso RTS del script), nunca al despertar del deep sleep, se abre una
 *  ventana de PROV_WINDOW_MS que acepta la misma linea o {"cmd":"erase"}.
 *  Los ciclos normales no la pagan.
 * ==========================================================================*/
#define PROV_WAIT_MS               60000UL     // NVS sin WiFi: espera de la linea
#define PROV_RETRY_SLEEP_MS        3600000UL   // y si no llega, 1 h de deep sleep
#define PROV_WINDOW_MS             3000UL      // arranque en frio ya aprovisionado.
                                               // El script envia en cuanto ve
                                               // "PROV: READY", asi que 3 s sobran
#define PROV_LINE_MAX_LEN          512         // la linea completa ocupa ~350 B
                                               // con key y token de 63
#define PROV_SERIAL_RX_BUFFER      1024        // buffer RX de la UART (256 por
                                               // defecto: justo para una linea
                                               // con caracteres escapados)

/* ===========================================================================
 * 9. FOTOS ATRASADAS Y RELOJ  -  buffer en la flash interna
 *
 *  Si la foto del ciclo no se puede subir (sin WiFi, Fog caido, 5xx), se
 *  guarda en la particion "spiffs" de Minimal SPIFFS (0x3D0000, 128 KB) con
 *  LittleFS, que viene en el core. Cuando vuelve la conexion se sube primero
 *  la foto actual y despues hasta BACKLOG_MAX_SEND_PER_CYCLE atrasadas, cada
 *  una con su captured_at. Contrato y motivos: docs/handoff-buffer-fotos-offline.md.
 *
 *  No hay microSD: su D3 esta cableado a GPIO13 y el pull-up interno de la
 *  tarjeta (10-90 kOhm a 3,3 V, siempre alimentada) falsea la bateria.
 *
 *  Capacidad: LittleFS asigna bloques enteros de 4 KB. Una foto de 30 KB
 *  ocupa 8, una de 40 KB 10 y una de 70 KB 18, y quedan ~27 bloques libres:
 *  caben 1-3 fotos = 5-15 min de corte. Lleno, se descarta la mas antigua.
 *
 *  El reloj: el ESP32 no tiene RTC con pila. Se pone en hora con la cabecera
 *  Date de cada 2xx del Fog y sigue contando en deep sleep (oscilador RC
 *  interno, deriva de hasta ~5 %). Sin una sincronizacion desde el ultimo
 *  arranque en frio no hay hora, y una foto sin hora no se guarda.
 * ==========================================================================*/
#define BACKLOG_ENABLED             1
#define BACKLOG_PARTITION_LABEL     "spiffs"   // etiqueta en min_spiffs.csv
#define BACKLOG_DIR                 "/q1"      // la version del formato va en el nombre:
                                               // un formato nuevo = directorio nuevo
#define BACKLOG_MAX_FILES           8          // tope de seguridad; manda el espacio
#define BACKLOG_MAX_SEND_PER_CYCLE  3          // acota el ciclo: 3 POST mas (~3 s de radio)
#define BACKLOG_MAX_AGE_S           86400UL    // 24 h, igual que BACKLOG_MAX_AGE_HOURS del Fog:
                                               // mas vieja, el Fog la rechazaria con 422
#define BACKLOG_FREE_MARGIN_BYTES   8192       // 2 bloques libres: LittleFS escribe copias
                                               // nuevas antes de soltar las viejas

/* Ninguna hora anterior a esta es real (2026-01-01T00:00:00Z): un reloj que
 * marca menos no se ha sincronizado nunca, y una Date que marca menos es un
 * Fog con la hora mal (la Pi no tiene RTC: sin NTP arranca con hora vieja). */
#define CLOCK_MIN_VALID_EPOCH       1767225600UL
/* La deriva medida entre dos sincronizaciones solo se aplica si es creible:
 * el RC de 150 kHz se desvia como mucho ~5 %; mas de un 10 % es otra cosa
 * (un Fog que cambio de hora) y es mejor no tocar las horas guardadas. */
#define CLOCK_MAX_DRIFT_PCT         10
#define CLOCK_MIN_SPAN_S            60         // intervalo minimo para medir la deriva

/* ===========================================================================
 * 10. OTA (E5)  -  actualizacion de firmware desde el Fog
 *
 *  Cada OTA_CHECK_EVERY_CYCLES ciclos con subida correcta se pide
 *  GET /api/v1/camera/config. Si su bloque "firmware" ofrece una version
 *  mayor que FW_VERSION y no rechazada antes (ota_rej), se verifica la firma
 *  ANTES de descargar nada, se descarga por streaming a la otra ranura
 *  comprobando tamano y SHA-256, y se reinicia en ella. La imagen nueva solo
 *  se confirma tras una subida 2xx (ota_confirm_if_pending): si no, el
 *  bootloader vuelve a esta al siguiente arranque y esta apunta la version
 *  en ota_rej para no volver a instalarla.
 *
 *  Lo firmado (ECDSA P-256 + SHA-256, firma DER en Base64) es la cadena
 *    parkvision-fw/1|<OTA_HARDWARE_ID>|<version>|<size>|<sha256 hex minusculas>
 *  y no el binario: ata la version a la imagen, asi un binario antiguo y
 *  legitimo no se puede presentar como una version mayor. La arman igual la
 *  nube (FirmwareManifest.canonicalString) y el Fog (firmware/signing.py).
 * ==========================================================================*/
#define OTA_ENABLED                1
#define OTA_HARDWARE_ID            "esp32cam-aithinker"
#define OTA_CHECK_EVERY_CYCLES     12          // 12 x 5 min = una vez por hora
/* Descargar y escribir 1,2 MB son ~10-20 s con la radio encendida. Con la
 * celda baja un brownout a media escritura no rompe nada (la ranura nueva no
 * se activa hasta el final), pero gasta la descarga entera. Sin medida
 * fiable (USB en el banco) no se bloquea. */
#define OTA_MIN_BATTERY_MV         3600
#define OTA_HTTP_TIMEOUT_MS        10000UL     // sin recibir nada en este tiempo: se aborta
#define OTA_DOWNLOAD_TIMEOUT_MS    120000UL    // tope de la descarga completa
#define OTA_BUFFER_SIZE            4096        // un sector: lo que se lee y escribe de cada vez
#define OTA_SIGNATURE_B64_MAX_LEN  128         // DER de P-256 <= 72 B = 96 en Base64

/* Clave PUBLICA de firma (PEM, SubjectPublicKeyInfo P-256). Es publica, no un
 * secreto: va compilada a proposito, porque es el ancla de confianza y no
 * puede llegar por la misma red que el firmware. Es la misma de
 * FIRMWARE_SIGNING_PUBLIC_KEY_PATH en el Fog. Cambiarla (rotacion) exige
 * publicar antes un firmware firmado con la vieja que traiga la nueva.
 * Vacia = OTA desactivado (se loguea al recibir una oferta).
 * signingKeyId (16 hex del SHA-256 del DER): aed67c7249a5c365. */
#define OTA_SIGNING_PUBLIC_KEY_PEM \
  "-----BEGIN PUBLIC KEY-----\n" \
  "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEKdQ6eqRdUkWhwnPIH/bzQKsRtHGe\n" \
  "BFBFlCuq7lwxmrdwBOTZaq8DBqLPklAMemqCaqZvd/1rZ8TRaSSCvvWb/w==\n" \
  "-----END PUBLIC KEY-----\n"

/* ===========================================================================
 * 11. DEPURACION
 * ==========================================================================*/
#define SERIAL_BAUD            115200
#define DEBUG_ENABLED          1

#if DEBUG_ENABLED
  #define LOG(fmt, ...)  Serial.printf("[%8lu] " fmt "\n", millis(), ##__VA_ARGS__)
#else
  #define LOG(fmt, ...)  do {} while (0)
#endif
