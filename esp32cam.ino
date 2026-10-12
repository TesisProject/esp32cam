/* ============================================================================
 *  esp32cam.ino  -  ESP32-CAM cliente REST con deep sleep
 *  Proyecto de tesis UPC "ParkVision"
 *
 *  Cada despertar del deep sleep ejecuta un ciclo completo dentro de setup():
 *    1. Inicializar la camara.
 *    2. Conectar WiFi en modo estacion (con timeout: sin red no hay nada que
 *       subir, no vale la pena reintentar indefinidamente y gastar bateria).
 *    3. Capturar 1 frame JPEG y subirlo por POST multipart a la Raspberry Pi
 *       (fog node), que procesa las plazas de parking y reenvia el resultado
 *       a la nube. El ESP32 no sabe nada de eso: solo manda el JPEG (y la
 *       telemetria del ciclo en el mismo POST).
 *    4. Apagar camara (PWDN + hold) y WiFi, y dormir CAPTURE_INTERVAL_MS.
 *
 *  Junto a la foto viaja la telemetria del ciclo (UploadTelemetry). Si la
 *  imagen en ejecucion viene de un OTA y aun no esta confirmada, solo se
 *  confirma tras una subida 2xx (rollback, ver ota_module.cpp).
 *
 *  No hay loop() real: no queda nada despues de esp_deep_sleep_start().
 *
 *  Placa en el IDE: "AI Thinker ESP32-CAM"
 * ==========================================================================*/
#include <Arduino.h>

#include <esp_sleep.h>
#include <esp_system.h>
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>

#include "config.h"
#include "src/camera/camera_module.h"
#include "src/wifi/wifi_module.h"
#include "src/upload/upload_module.h"
#include "src/ota/ota_module.h"

/* ===========================================================================
 * Estado entre ciclos (memoria RTC lenta)
 *
 *  Cada despertar del deep sleep es un reset: la RAM normal empieza de cero.
 *  RTC_DATA_ATTR deja estas variables en la memoria RTC, que sigue
 *  alimentada durante el deep sleep. Viven aqui porque las decide el ciclo
 *  (no ningun modulo) y viajan al Fog como telemetria.
 * ==========================================================================*/
/* Despertares de deep sleep desde el ultimo arranque en frio (0 en el ciclo
 * justo despues de encender o de cualquier reset que no sea deep sleep). Un
 * salto a 0 con reset_reason != 8 delata un brownout o un cuelgue. */
RTC_DATA_ATTR static uint32_t s_wake_count = 0;

/* Ciclos seguidos sin conseguir WiFi. Se pone a 0 al conectar, pero se
 * reporta el valor de ANTES de ponerlo a 0: si no, el Fog (que solo recibe
 * datos de los ciclos que SI conectan) veria siempre 0. */
RTC_DATA_ATTR static uint32_t s_wifi_fail_streak = 0;

/* ===========================================================================
 * Apagado y deep sleep
 * ==========================================================================*/
static void go_to_sleep() {
  camera_power_down();   // PWDN + rtc_gpio_hold_en: retiene el pin dormido
  wifi_off();

#if STATUS_LED_ENABLED
  digitalWrite(STATUS_LED_PIN, STATUS_LED_ACTIVE_LOW ? HIGH : LOW);   // apagado
#endif

  LOG("SLEEP: durmiendo %lu ms", (unsigned long)CAPTURE_INTERVAL_MS);
  Serial.flush();

  esp_sleep_enable_timer_wakeup((uint64_t)CAPTURE_INTERVAL_MS * 1000ULL);
  esp_deep_sleep_start();
}

/* ===========================================================================
 * setup  -  un ciclo completo por cada despertar
 * ==========================================================================*/
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println();
  LOG("=== %s (%s) ===", DEVICE_NAME, DEVICE_ID);
  ota_log_boot_info();   // FW_VERSION + particion activa

  const esp_reset_reason_t reset_reason = esp_reset_reason();
  /* La memoria RTC no siempre se borra en un reset que no sea deep sleep
   * (depende de la causa), asi que se pone a 0 a mano: "desde el ultimo
   * arranque en frio" queda definido por reset_reason y no por el hardware. */
  if (reset_reason == ESP_RST_DEEPSLEEP) {
    s_wake_count++;
  } else {
    s_wake_count       = 0;
    s_wifi_fail_streak = 0;
  }
  LOG("Reset reason: %d  wake_count=%lu  wifi_fail_streak=%lu",
      (int)reset_reason, (unsigned long)s_wake_count,
      (unsigned long)s_wifi_fail_streak);

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

  /* --- Camara --------------------------------------------------------------*/
  if (!camera_init()) {
    LOG("FATAL: la camara no arranca. Se aborta el ciclo y se duerme igual.");
    go_to_sleep();
  }

  /* --- WiFi ------------------------------------------------------------------*/
  if (!wifi_connect()) {
    s_wifi_fail_streak++;
    go_to_sleep();
  }
  const uint32_t wifi_fail_streak = s_wifi_fail_streak;   // el de antes de conectar
  s_wifi_fail_streak = 0;

  /* --- Captura y subida --------------------------------------------------------*/
  camera_fb_t *fb = camera_capture();
  if (!fb) {
    LOG("FATAL: no se pudo capturar el frame.");
  } else {
    UploadTelemetry t;
    t.fw_version       = FW_VERSION;
    t.rssi             = wifi_rssi();
    t.wifi_connect_ms  = wifi_connect_ms();
    t.reset_reason     = (int)reset_reason;
    t.free_heap        = ESP.getFreeHeap();
    t.wake_count       = s_wake_count;
    t.wifi_fail_streak = wifi_fail_streak;
    t.framesize        = camera_framesize_name();
    t.jpeg_quality     = camera_jpeg_quality();
    /* millis() vuelve a 0 en cada despertar (el deep sleep es un reset), asi
     * que es el tiempo despierto de este ciclo hasta el POST. No incluye la
     * ROM ni el bootloader (unos cientos de ms) ni el propio POST: es una
     * cota inferior, comparable entre ciclos, no el consumo exacto. */
    t.awake_ms         = millis();

    const int code = upload_frame(fb, &t);
    if (code >= 200 && code < 300) {
      LOG("UPLOAD: ok (%d)", code);
      /* Unico punto donde se da por buena una imagen nueva: WiFi conectado y
       * el Fog ha aceptado la foto. Si una imagen recien instalada por OTA
       * (PENDING_VERIFY) llega al deep sleep sin pasar por aqui, el
       * bootloader la descarta al despertar y vuelve a la anterior (ver
       * ota_module.cpp). Por eso NO se valida al arrancar ni al conectar:
       * una version que conecta pero no sabe subir tambien debe caer. */
      ota_confirm_if_pending();
    } else {
      LOG("UPLOAD: fallo (%d)", code);
    }
    camera_release(fb);
  }

  go_to_sleep();
}

/* Nunca se ejecuta: go_to_sleep() termina siempre en esp_deep_sleep_start(),
 * que no retorna. El framework Arduino exige declarar loop() de todos modos. */
void loop() {}
