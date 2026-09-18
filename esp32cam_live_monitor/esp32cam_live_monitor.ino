/* ============================================================================
 *  esp32cam_live_monitor.ino  -  ESP32-CAM cliente REST con deep sleep
 *  Proyecto de tesis UPC "ParkVision"
 *
 *  Cada despertar del deep sleep ejecuta un ciclo completo dentro de setup():
 *    1. Leer bateria: al log por serie y a la telemetria que acompana a la
 *       imagen (en milivoltios; el porcentaje no se envia, ver config.h).
 *    2. Inicializar la camara.
 *    3. Conectar WiFi en modo estacion (con timeout: sin red no hay nada que
 *       subir, no vale la pena reintentar indefinidamente y gastar bateria).
 *    4. Capturar 1 frame JPEG y subirlo por POST multipart a la Raspberry Pi
 *       (fog node), que procesa las plazas de parking y reenvia el resultado
 *       a la nube. El ESP32 no sabe nada de eso: solo manda el JPEG, y no
 *       espera a la deteccion (el Fog responde 202 al instante).
 *       La camara se identifica por su X-API-Key/X-API-Token, no por la URL.
 *    5. Apagar camara (PWDN + hold) y WiFi, y dormir CAPTURE_INTERVAL_MS, o
 *       UPLOAD_ERROR_BACKOFF_MS si el Fog rechazo la credencial.
 *
 *  No hay loop() real: no queda nada despues de esp_deep_sleep_start().
 *
 *  Placa en el IDE: "AI Thinker ESP32-CAM"
 * ==========================================================================*/
#include <Arduino.h>
#include <WiFi.h>

#include <esp_sleep.h>
#include <esp_system.h>
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>

#include "config.h"
#include "camera_module.h"
#include "battery_module.h"
#include "upload_module.h"

/* ===========================================================================
 * Estado que sobrevive al deep sleep
 *
 *  La RAM normal se borra en cada ciclo: setup() arranca de cero y cualquier
 *  contador global volveria a 0. Los 8 KB de RTC slow memory siguen
 *  alimentados durante el deep sleep, que es el mismo dominio que mantiene
 *  vivo el temporizador de despertar.
 *
 *  Ojo: NO sobreviven a un arranque en frio (quitar las pilas, reset por EN,
 *  o un brownout). Falla hacia el lado seguro: los contadores vuelven a 0 y
 *  el dispositivo se comporta como si acabara de empezar. El campo
 *  reset_reason de la telemetria permite distinguir ese caso desde el Fog.
 * ==========================================================================*/
RTC_DATA_ATTR static uint32_t rtc_wake_count       = 0;
RTC_DATA_ATTR static uint32_t rtc_wifi_fail_streak = 0;

/* ===========================================================================
 * WiFi  -  solo estacion: un AP propio obligaria al ESP32 a emitir balizas
 * sin parar, y con eso la radio no podria dormir nunca entre ciclos.
 *
 *  De paso mide dos cosas que solo se pueden medir aqui:
 *    - cuanto tarda en asociarse (degrada antes de fallar del todo)
 *    - el minimo de tension del pack mientras la radio transmite a plena
 *      potencia, que es el dato que anticipa el apagon por brownout
 * ==========================================================================*/
static bool wifi_connect(uint32_t *connect_ms, uint32_t *load_min_mv) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_HOSTNAME);

#if WIFI_USE_STATIC_IP
  IPAddress ip(WIFI_STATIC_IP), gw(WIFI_STATIC_GATEWAY),
            mask(WIFI_STATIC_SUBNET), dns(WIFI_STATIC_DNS);
  if (!WiFi.config(ip, gw, mask, dns)) {
    LOG("WIFI: fallo al fijar la IP estatica, se sigue con DHCP");
  }
#endif

  /* Menos potencia de TX = menos corriente de pico y menos hundimiento de
   * tension en el pack. Si el router queda lejos, subelo en config.h. */
  WiFi.setTxPower((wifi_power_t)(WIFI_TX_POWER_DBM * 4));

  LOG("WIFI: conectando a \"%s\"...", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t started = millis();
  uint32_t       min_mv  = 0;

  while (WiFi.status() != WL_CONNECTED &&
         (millis() - started) < WIFI_CONNECT_TIMEOUT_MS) {
    /* Sustituye al delay(250) de toda la vida: espera lo mismo, pero midiendo.
     * La asociacion y el DHCP son el rato en el que la radio mas transmite,
     * asi que es la mejor ventana para pillar el hundimiento del pack sin
     * montar una tarea aparte durante el POST (que es bloqueante). */
    const uint32_t mv = battery_sample_min_mv(250);
    if (mv > 0 && (min_mv == 0 || mv < min_mv)) min_mv = mv;
  }

  if (connect_ms)   *connect_ms   = millis() - started;
  if (load_min_mv)  *load_min_mv  = min_mv;

  if (WiFi.status() != WL_CONNECTED) {
    /* Codigo de estado para diagnostico: 1=WL_NO_SSID_AVAIL (no ve la red,
     * problema de alcance/banda), 4=WL_CONNECT_FAILED (password/auth mal),
     * 6=WL_DISCONNECTED (generico). Ver enum wl_status_t en WiFiType.h. */
    LOG("WIFI: sin conexion tras %lu ms (status=%d), se aborta este ciclo",
        (unsigned long)WIFI_CONNECT_TIMEOUT_MS, (int)WiFi.status());
    WiFi.disconnect(true);
    return false;
  }

  LOG("WIFI: conectado. IP=%s  RSSI=%d dBm",
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

/* ===========================================================================
 * Captura y subida  -  devuelve cuantos ms hay que dormir despues
 *
 *  Respuestas del Fog (contrato en config.h, seccion 3):
 *    202  aceptado; la deteccion corre luego en la Pi, no se espera
 *    400  imagen vacia    -> falla el frame, no la red: capturar otro
 *    401  credencial invalida o headers ausentes -> secrets.h mal grabado
 *    403  camara deshabilitada en el Fog -> puede reactivarse sola: backoff
 *    404  la credencial no corresponde a ninguna camara dada de alta
 *    405  URL antigua (con el id dentro)  -> FOG_FRAMES_PATH mal
 *    415  falta el Content-Type de la parte file -> bug del multipart
 *    422  falta el filename de la parte file     -> bug del multipart
 *
 *  Los cuatro ultimos no se arreglan solos: reintentar cada CAPTURE_INTERVAL_MS
 *  seria quemar pilas sin cambiar nada, asi que se duerme el backoff largo.
 * ==========================================================================*/
static uint32_t capture_and_upload(UploadTelemetry &telemetry) {
  bool retry_allowed = UPLOAD_RETRY_ON_EMPTY;

  for (;;) {
    camera_fb_t *fb = camera_capture();
    if (!fb) {
      LOG("FATAL: no se pudo capturar el frame.");
      return CAPTURE_INTERVAL_MS;
    }

    /* Se rellena justo antes del POST para que refleje el ciclo real, no el
     * momento en que se monto la estructura. */
    telemetry.awake_ms  = millis();
    telemetry.free_heap = ESP.getFreeHeap();

    const int code = upload_frame(fb, telemetry);
    camera_release(fb);          // el buffer vuelve al driver antes de decidir

    if (code >= 200 && code < 300) {
      if (code != UPLOAD_ACCEPTED_CODE) {
        LOG("UPLOAD: ok (%d), aunque el contrato promete %d",
            code, UPLOAD_ACCEPTED_CODE);
      } else {
        LOG("UPLOAD: aceptado (%d)", code);
      }
      return CAPTURE_INTERVAL_MS;
    }

    switch (code) {
      case 400:
        if (retry_allowed) {
          LOG("UPLOAD: 400 imagen vacia, se repite la captura una vez");
          retry_allowed = false;
          continue;              // otro frame sin volver a dormir ni reconectar
        }
        LOG("UPLOAD: 400 otra vez, se deja para el proximo ciclo");
        return CAPTURE_INTERVAL_MS;

      case 401:
        LOG("UPLOAD: 401 credencial rechazada. Revisa API_KEY/API_TOKEN de "
            "esta unidad en secrets.h.");
        return UPLOAD_ERROR_BACKOFF_MS;

      case 403:
        LOG("UPLOAD: 403 camara deshabilitada en el Fog.");
        return UPLOAD_ERROR_BACKOFF_MS;

      case 404:
        LOG("UPLOAD: 404 la credencial no corresponde a ninguna camara dada de "
            "alta en el Fog.");
        return UPLOAD_ERROR_BACKOFF_MS;

      case 405:
        LOG("UPLOAD: 405 ruta equivocada. FOG_FRAMES_PATH deberia ser %s",
            FOG_FRAMES_PATH);
        return UPLOAD_ERROR_BACKOFF_MS;

      case 415:
      case 422:
        LOG("UPLOAD: %d multipart mal formado (Content-Type o filename de la "
            "parte file). Es un bug del firmware, no de la red.", code);
        return UPLOAD_ERROR_BACKOFF_MS;

      default:
        /* Timeouts, DNS, 5xx: transitorios. Ciclo normal y a reintentar.
         *
         * Un codigo negativo es fallo de conexion (no llego a haber respuesta
         * HTTP). Puede ser que la Pi este apagada, pero tambien que haya
         * cambiado de IP y la cacheada ya no valga: se olvida para que el
         * proximo ciclo resuelva por mDNS otra vez. Si la Pi simplemente
         * estaba apagada, resolver de nuevo cuesta 2 s una sola vez. */
        if (code < 0) upload_forget_host();
        LOG("UPLOAD: fallo (%d), se reintenta en el proximo ciclo", code);
        return CAPTURE_INTERVAL_MS;
    }
  }
}

/* ===========================================================================
 * Apagado y deep sleep
 * ==========================================================================*/
/* sleep_ms parametrizado: el ciclo normal duerme CAPTURE_INTERVAL_MS, pero un
 * error de credencial o de alta en el Fog duerme UPLOAD_ERROR_BACKOFF_MS. */
static void go_to_sleep(uint32_t sleep_ms) {
  camera_power_down();   // PWDN + rtc_gpio_hold_en: retiene el pin dormido
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

#if STATUS_LED_ENABLED
  digitalWrite(STATUS_LED_PIN, STATUS_LED_ACTIVE_LOW ? HIGH : LOW);   // apagado
#endif

  LOG("SLEEP: durmiendo %lu ms", (unsigned long)sleep_ms);
  Serial.flush();

  esp_sleep_enable_timer_wakeup((uint64_t)sleep_ms * 1000ULL);
  esp_deep_sleep_start();
}

/* ===========================================================================
 * setup  -  un ciclo completo por cada despertar
 * ==========================================================================*/
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println();
  LOG("=== %s (%s) fw %s ===", DEVICE_NAME, DEVICE_ID, FW_VERSION);

  /* 8 = ESP_RST_DEEPSLEEP (ciclo normal), 9 = ESP_RST_BROWNOUT (la
   * alimentacion se hunde: pilas gastadas o falta el condensador),
   * 1 = ESP_RST_POWERON (arranque en frio: los contadores RTC valen 0). */
  const esp_reset_reason_t reset_reason = esp_reset_reason();
  rtc_wake_count++;
  LOG("Reset reason: %d, ciclo #%lu", (int)reset_reason,
      (unsigned long)rtc_wake_count);

  UploadTelemetry telemetry;
  upload_telemetry_clear(&telemetry);
  telemetry.reset_reason = (uint8_t)reset_reason;
  telemetry.wake_count   = rtc_wake_count;

#if STATUS_LED_ENABLED
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, STATUS_LED_ACTIVE_LOW ? LOW : HIGH);   // encendido
#endif

#if POWER_DISABLE_BROWNOUT
  /* Evita reinicios por los picos de corriente del WiFi con pilas gastadas.
   * Enmascara una alimentacion deficiente: usar solo con buen condensador. */
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  LOG("POWER: detector de brownout DESACTIVADO");
#endif

  setCpuFrequencyMhz(POWER_CPU_FREQ_ACTIVE_MHZ);

  /* --- Bateria: al log y a la telemetria del POST (config.h, seccion 3). */
  if (battery_init()) {
    battery_update(true);
    const BatteryReading b = battery_get();
    LOG("BAT: %.2f V, %u%% [%s]", (double)b.voltage, (unsigned)b.percent,
        battery_state_name(b.state));

    /* Al Fog van los milivoltios medidos, no el porcentaje: el porcentaje sale
     * de una curva que ya estuvo equivocada una vez y de un BATTERY_CALIBRATION
     * sin ajustar. Guardar mV permite recalcularlo luego sobre el historico. */
    telemetry.battery_valid = b.valid;
    telemetry.battery_mv    = (uint32_t)lroundf(b.voltage * 1000.0f);
  } else {
    LOG("BAT: sin medida de bateria");
    telemetry.battery_valid = false;
  }

  /* --- Camara --------------------------------------------------------------*/
  if (!camera_init()) {
    LOG("FATAL: la camara no arranca. Se aborta el ciclo y se duerme igual.");
    go_to_sleep(CAPTURE_INTERVAL_MS);
  }

  /* --- WiFi ------------------------------------------------------------------*/
  uint32_t connect_ms = 0, load_mv = 0;
  if (!wifi_connect(&connect_ms, &load_mv)) {
    /* La racha se guarda en RTC memory: es lo unico que permite distinguir un
     * fallo suelto de una caida larga, porque la RAM normal no sobrevive. */
    rtc_wifi_fail_streak++;
    LOG("WIFI: %lu fallos seguidos", (unsigned long)rtc_wifi_fail_streak);
    go_to_sleep(CAPTURE_INTERVAL_MS);
  }

  /* El valor que se reporta es la racha ANTERIOR a este ciclo: si vale 3, el
   * Fog sabe que esta camara estuvo 15 min sin red y acaba de recuperarse. */
  telemetry.wifi_fail_streak = rtc_wifi_fail_streak;
  rtc_wifi_fail_streak       = 0;

  telemetry.wifi_connect_ms = connect_ms;
  telemetry.rssi            = (int32_t)WiFi.RSSI();
  if (load_mv > 0) {
    telemetry.battery_load_mv = load_mv;
    LOG("BAT: minimo bajo carga %lu mV (caida %ld mV)",
        (unsigned long)load_mv,
        (long)telemetry.battery_mv - (long)load_mv);
  }

  /* --- Captura y subida --------------------------------------------------------*/
  go_to_sleep(capture_and_upload(telemetry));
}

/* Nunca se ejecuta: go_to_sleep() termina siempre en esp_deep_sleep_start(),
 * que no retorna. El framework Arduino exige declarar loop() de todos modos. */
void loop() {}
