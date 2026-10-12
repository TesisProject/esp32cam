/* ============================================================================
 *  config.h  -  CONFIGURACION CENTRAL DEL PROYECTO (valores por defecto)
 *  ESP32-CAM ParkVision (AI-Thinker)  -  version Arduino IDE
 *
 *  TODA constante del firmware vive aqui. Ningun otro fichero contiene
 *  valores hardcodeados: ni pines, ni umbrales, ni intervalos.
 *
 *  Pero lo que es propio de cada placa o de cada despliegue (WiFi, host del
 *  Fog y, mas adelante, credencial y parametros de captura) NO se compila:
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
#define FW_VERSION             1

/* ===========================================================================
 * 2. WIFI  (solo modo estacion: un AP propio impediria dormir la radio)
 * ==========================================================================*/
/* --- Red WiFi: en la NVS, NO aqui ----------------------------------------
 *
 *  El SSID y la contrasena se graban por USB en la NVS de cada placa
 *  (seccion 8 y scripts/provision.ps1). No estan en el binario: el mismo
 *  .bin sirve para cualquier red, y publicar el repo no regala el WiFi.
 *
 * --- Credenciales de la camara: secrets.h (provisional, hasta E7) --------
 *
 *  DEVICE_ID, API_KEY y API_TOKEN siguen en secrets.h, que git NO versiona,
 *  hasta que la camara las obtenga sola con el alta por MAC.
 *
 *  Si acabas de clonar el repositorio:
 *      copy secrets.h.example secrets.h      (y rellena los tres valores)
 * ------------------------------------------------------------------------*/
#if defined(__has_include)
  #if __has_include("secrets.h")
    #include "secrets.h"
  #endif
#else
  #include "secrets.h"
#endif

#if !defined(DEVICE_ID) || !defined(API_KEY) || !defined(API_TOKEN)
  #error "Falta secrets.h (o le faltan valores). Copia secrets.h.example a secrets.h y rellena DEVICE_ID, API_KEY y API_TOKEN."
#endif

/* Un secrets.h antiguo puede traer aun WIFI_SSID/WIFI_PASSWORD. Se anulan
 * para que ningun codigo vuelva a usarlos por descuido (no compilaria) y no
 * acaben dentro del .bin. Borralos de tu secrets.h: ya no hacen nada. */
#ifdef WIFI_SSID
  #undef WIFI_SSID
#endif
#ifdef WIFI_PASSWORD
  #undef WIFI_PASSWORD
#endif

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

#define CAPTURE_INTERVAL_MS     30000UL    // 30 segundos entre capturas (deep sleep)

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

/* ===========================================================================
 * 7. ALMACEN EN NVS  -  la configuracion propia de cada placa
 *
 *  src/settings/settings_module.* es la UNICA puerta a la NVS. El esquema
 *  completo (WiFi activa y pendiente, Fog, credencial, captura, revision,
 *  OTA rechazada) existe desde E6 aunque de momento solo se usen WiFi y Fog.
 *  Tamanos maximos de lo que se guarda: fijan el tamano de los blobs, asi
 *  que cambiarlos invalida lo ya guardado (ver SETTINGS_SCHEMA_VERSION).
 * ==========================================================================*/
#define SETTINGS_NVS_NAMESPACE     "parkvision"   // max. 15 caracteres (limite NVS)
#define SETTINGS_SCHEMA_VERSION    1    // subirlo si cambia la forma de un blob:
                                        // la NVS antigua se borra al arrancar en
                                        // vez de leer basura

#define CAM_CODE_MAX_LEN           15   // "CAM-001"
#define API_SECRET_MAX_LEN         63   // X-API-Key / X-API-Token del Fog
#define CAP_FRAMESIZE_MAX_LEN      7    // "SVGA", "QVGA"... (nombre del enum)

/* ===========================================================================
 * 8. APROVISIONAMIENTO POR USB  -  una vez por placa
 *
 *  Sin WiFi en la NVS la placa no puede hacer nada util: espera por serie
 *  una linea JSON (contrato C, la envia scripts/provision.ps1)
 *      {"wifi":{"ssid":"...","password":"..."},"fogHost":"...","fogPort":8000}
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
#define PROV_LINE_MAX_LEN          512         // la linea completa ocupa ~200 B
#define PROV_SERIAL_RX_BUFFER      1024        // buffer RX de la UART (256 por
                                               // defecto: justo para una linea
                                               // con caracteres escapados)

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
