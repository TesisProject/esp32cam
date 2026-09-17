/* ============================================================================
 *  esp32cam_live_monitor.ino  -  ESP32-CAM cliente REST con deep sleep
 *  Proyecto de tesis UPC "ParkVision"
 *
 *  Cada despertar del deep sleep ejecuta un ciclo completo dentro de setup():
 *    1. Leer bateria (solo para el log por serie; el endpoint no acepta ese
 *       dato, ver upload_module.cpp).
 *    2. Inicializar la camara.
 *    3. Conectar WiFi en modo estacion (con timeout: sin red no hay nada que
 *       subir, no vale la pena reintentar indefinidamente y gastar bateria).
 *    4. Capturar 1 frame JPEG y subirlo por POST multipart a la Raspberry Pi
 *       (fog node), que procesa las plazas de parking y reenvia el resultado
 *       a la nube. El ESP32 no sabe nada de eso: solo manda el JPEG.
 *    5. Apagar camara (PWDN + hold) y WiFi, y dormir CAPTURE_INTERVAL_MS.
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
 * WiFi  -  solo estacion: un AP propio obligaria al ESP32 a emitir balizas
 * sin parar, y con eso la radio no podria dormir nunca entre ciclos.
 * ==========================================================================*/
static bool wifi_connect() {
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
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - started) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

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
 * Apagado y deep sleep
 * ==========================================================================*/
static void go_to_sleep() {
  camera_power_down();   // PWDN + rtc_gpio_hold_en: retiene el pin dormido
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

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
  LOG("Reset reason: %d", (int)esp_reset_reason());

#if POWER_DISABLE_BROWNOUT
  /* Evita reinicios por los picos de corriente del WiFi con pilas gastadas.
   * Enmascara una alimentacion deficiente: usar solo con buen condensador. */
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  LOG("POWER: detector de brownout DESACTIVADO");
#endif

  setCpuFrequencyMhz(POWER_CPU_FREQ_ACTIVE_MHZ);

  /* --- Bateria: solo para el log por serie, el contrato de la API no tiene
   * campo para este dato (ver config.h, seccion 3). */
  if (battery_init()) {
    battery_update(true);
    const BatteryReading b = battery_get();
    LOG("BAT: %.2f V, %u%% [%s]", (double)b.voltage, (unsigned)b.percent,
        battery_state_name(b.state));
  } else {
    LOG("BAT: sin medida de bateria");
  }

  /* --- Camara --------------------------------------------------------------*/
  if (!camera_init()) {
    LOG("FATAL: la camara no arranca. Se aborta el ciclo y se duerme igual.");
    go_to_sleep();
  }

  /* --- WiFi ------------------------------------------------------------------*/
  if (!wifi_connect()) {
    go_to_sleep();
  }

  /* --- Captura y subida --------------------------------------------------------*/
  camera_fb_t *fb = camera_capture();
  if (!fb) {
    LOG("FATAL: no se pudo capturar el frame.");
  } else {
    const int code = upload_frame(fb);
    if (code >= 200 && code < 300) {
      LOG("UPLOAD: ok (%d)", code);
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
