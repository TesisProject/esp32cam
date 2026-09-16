/* ============================================================================
 *  esp32cam_live_monitor.ino  -  ESP32-CAM Live Monitor (AI-Thinker)
 *
 *  Dispositivo autosuficiente: no hay Raspberry Pi, ni servidor Python, ni
 *  servicio externo. El propio ESP32-CAM se conecta al WiFi, levanta su
 *  servidor HTTP y sirve el dashboard, la imagen de la camara y la bateria.
 *
 *  Sin librerias externas: solo el core ESP32 de Espressif. El dashboard va
 *  compilado dentro del binario (dashboard_html.h) y el driver del ADS1115
 *  esta escrito en battery_module.cpp. Un solo flasheo y listo.
 *
 *  Placa en el IDE: "AI Thinker ESP32-CAM"
 *  Particion:       "Huge APP (3MB No OTA/1MB SPIFFS)"
 *
 *  setup() : camara -> bateria -> WiFi -> servidor
 *  loop()  : vigila el enlace WiFi, refresca la bateria y aplica las medidas
 *            de ahorro de energia que son compatibles con tener un servidor
 *            escuchando (ver bloque 8 de config.h).
 * ==========================================================================*/
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>

#include "config.h"
#include "camera_module.h"
#include "battery_module.h"
#include "web_server_module.h"

static uint32_t s_last_wifi_check_ms = 0;
static uint32_t s_current_cpu_mhz    = 0;
static bool     s_ap_mode            = false;  // true = red propia del ESP32

/* ===========================================================================
 * WiFi
 * ==========================================================================*/
/* Modem sleep solo existe en modo estacion: es el cliente quien negocia con
 * el router cuando dormir (DTIM). Un punto de acceso tiene que emitir balizas
 * continuamente, asi que su radio no puede apagarse. */
static void wifi_apply_power_save() {
#if POWER_WIFI_MODEM_SLEEP
  if (s_ap_mode) {
    WiFi.setSleep(false);
    LOG("POWER: modo AP -> sin modem sleep (la radio no puede dormir)");
  } else {
    WiFi.setSleep(true);
    LOG("POWER: modem sleep WiFi activado");
  }
#else
  WiFi.setSleep(false);
#endif
}

/* --- Red propia del ESP32 (SoftAP) --------------------------------------*/
static bool wifi_start_ap() {
  WiFi.mode(WIFI_AP);

  IPAddress ip(WIFI_AP_IP), gw(WIFI_AP_GATEWAY), mask(WIFI_AP_SUBNET);
  if (!WiFi.softAPConfig(ip, gw, mask)) {
    LOG("WIFI: no se pudo fijar la IP del AP, se usara la de por defecto");
  }

  /* WPA2 exige 8 caracteres como minimo. Con menos, red abierta (pass=NULL).
   * sizeof incluye el terminador, de ahi el "> 8". */
  const char *pass = (sizeof(WIFI_AP_PASSWORD) > 8) ? WIFI_AP_PASSWORD : NULL;

  if (!WiFi.softAP(WIFI_AP_SSID, pass, WIFI_AP_CHANNEL,
                   WIFI_AP_HIDDEN, WIFI_AP_MAX_CLIENTS)) {
    LOG("WIFI: fallo al crear la red propia");
    return false;
  }

  s_ap_mode = true;
  wifi_apply_power_save();

  LOG("WIFI: red propia \"%s\" activa%s", WIFI_AP_SSID,
      pass ? "" : " (ABIERTA, sin contrasena)");
  LOG("WIFI: conectate desde el movil y abre http://%s/",
      WiFi.softAPIP().toString().c_str());
  return true;
}

/* --- Cliente del router --------------------------------------------------*/
static bool wifi_start_station() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.setAutoReconnect(true);

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
    LOG("WIFI: sin conexion tras %lu ms", (unsigned long)WIFI_CONNECT_TIMEOUT_MS);
    WiFi.disconnect(true);
    return false;
  }

  s_ap_mode = false;
  wifi_apply_power_save();

  LOG("WIFI: conectado. IP=%s  RSSI=%d dBm",
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

/* --- Arranque de red segun WIFI_NET_MODE --------------------------------*/
static bool wifi_begin() {
  bool up = false;

#if WIFI_NET_MODE == WIFI_NET_AP
  up = wifi_start_ap();

#elif WIFI_NET_MODE == WIFI_NET_STATION
  up = wifi_start_station();

#else   /* WIFI_NET_AP_FALLBACK */
  up = wifi_start_station();
  if (!up) {
    LOG("WIFI: el router no responde, levantando la red propia del ESP32");
    up = wifi_start_ap();
  }
#endif

  if (!up) return false;

#if ENABLE_MDNS
  if (MDNS.begin(DEVICE_HOSTNAME)) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    LOG("WIFI: mDNS -> http://%s.local/", DEVICE_HOSTNAME);
  }
#endif
  return true;
}

static void wifi_watchdog() {
  /* En modo AP no hay nada que vigilar: el ESP32 es la propia red. */
  if (s_ap_mode) return;

  if ((millis() - s_last_wifi_check_ms) < WIFI_RETRY_INTERVAL_MS) return;
  s_last_wifi_check_ms = millis();

  if (WiFi.status() == WL_CONNECTED) return;

  LOG("WIFI: enlace caido, reconectando...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

/* ===========================================================================
 * LED de estado (GPIO33, el LED rojo de la placa)
 *
 *  Es la unica forma de saber que hace el modulo cuando funciona con pilas y
 *  no hay monitor serie. Todo el patron es no bloqueante: se calcula a partir
 *  de millis(), sin delay(), para no frenar el servidor.
 * ==========================================================================*/
static uint8_t s_led_blinks = 0;   // 0 = encendido fijo (arrancando)

static void status_led_set(bool on) {
#if STATUS_LED_ENABLED
  const bool level = STATUS_LED_ACTIVE_LOW ? !on : on;
  digitalWrite(STATUS_LED_PIN, level ? HIGH : LOW);
#else
  (void)on;
#endif
}

static void status_led_init() {
#if STATUS_LED_ENABLED
  pinMode(STATUS_LED_PIN, OUTPUT);
  status_led_set(true);            // fijo mientras arranca
#endif
}

static void status_led_tick() {
#if STATUS_LED_ENABLED
  if (s_led_blinks == 0) { status_led_set(true); return; }

  const uint32_t t    = millis() % STATUS_LED_CYCLE_MS;
  const uint32_t slot = STATUS_LED_BLINK_MS + STATUS_LED_GAP_MS;

  bool on = false;
  for (uint8_t i = 0; i < s_led_blinks; i++) {
    const uint32_t start = (uint32_t)i * slot;
    if (t >= start && t < start + STATUS_LED_BLINK_MS) { on = true; break; }
  }
  status_led_set(on);
#endif
}

/* ===========================================================================
 * Energia
 * ==========================================================================*/
static void set_cpu_mhz(uint32_t mhz) {
  if (mhz == s_current_cpu_mhz) return;
  if (setCpuFrequencyMhz(mhz)) {
    s_current_cpu_mhz = mhz;
    LOG("POWER: CPU a %lu MHz", (unsigned long)mhz);
  }
}

static void power_init() {
#if POWER_DISABLE_BROWNOUT
  /* Evita reinicios por los picos de corriente del WiFi con pilas gastadas.
   * Enmascara una alimentacion deficiente: usar solo con buen condensador. */
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  LOG("POWER: detector de brownout DESACTIVADO");
#endif

  set_cpu_mhz(POWER_CPU_FREQ_ACTIVE_MHZ);

#if POWER_AUTO_LIGHT_SLEEP
  /* Light sleep automatico: el planificador duerme el nucleo entre eventos y
   * lo despierta con la interrupcion de red. NO se pierde la asociacion WiFi
   * (a diferencia del deep sleep), asi que el servidor sigue respondiendo.
   * Requiere CONFIG_PM_ENABLE en el build; si no lo esta, la llamada devuelve
   * ESP_ERR_NOT_SUPPORTED y el firmware sigue sin ello. */
  esp_pm_config_esp32_t pm = {};
  pm.max_freq_mhz       = POWER_CPU_FREQ_ACTIVE_MHZ;
  pm.min_freq_mhz       = POWER_CPU_FREQ_IDLE_MHZ;
  pm.light_sleep_enable = true;

  const esp_err_t err = esp_pm_configure(&pm);
  if (err == ESP_OK) {
    LOG("POWER: light sleep automatico activado (%d-%d MHz)",
        POWER_CPU_FREQ_IDLE_MHZ, POWER_CPU_FREQ_ACTIVE_MHZ);
  } else {
    LOG("POWER: light sleep no disponible (0x%x), se sigue sin el", err);
  }
#endif
}

/* Ajusta el consumo segun haya o no alguien mirando.
 *
 * "Ocupado" no es solo tener un stream abierto: una foto suelta tambien hay
 * que comprimirla, y a 80 MHz tarda el doble. Por eso se mantiene la
 * frecuencia alta un rato despues de la ultima peticion HTTP. */
static void power_tick() {
  const bool streaming = webserver_stream_active();
  const bool recent_http =
      (millis() - webserver_last_request_ms()) < POWER_ACTIVE_HOLD_MS;
  const bool busy = streaming || recent_http;

#if POWER_DYNAMIC_CPU_FREQ && !POWER_AUTO_LIGHT_SLEEP
  set_cpu_mhz(busy ? POWER_CPU_FREQ_ACTIVE_MHZ : POWER_CPU_FREQ_IDLE_MHZ);
#endif

#if POWER_CAMERA_SLEEP_IDLE
  /* Apaga el sensor si lleva rato sin usarse. Se despierta solo en la
   * siguiente captura (con ~350 ms de penalizacion). */
  if (!busy && camera_is_powered() &&
      (millis() - camera_last_use_ms()) > POWER_CAMERA_IDLE_MS) {
    camera_power_down();
  }
#endif

  /* Sin esta pausa el loop() acapara la CPU y el idle task nunca llega a
   * activar el modem sleep: el ahorro se esfuma. */
  delay(streaming ? 1 : POWER_IDLE_DELAY_MS);
}

/* Proteccion del pack de litio: por debajo del umbral critico, apagar.
 * Aqui SI se usa deep sleep, porque ya no hay nada que servir. */
static void battery_guard() {
#if BATTERY_ENABLED && BATTERY_CRITICAL_DEEPSLEEP
  if (!battery_critical_confirmed()) return;

  const BatteryReading b = battery_get();
  LOG("BAT: CRITICA (%.2f V, %u%%). Apagando para proteger las celdas.",
      (double)b.voltage, (unsigned)b.percent);
  Serial.flush();

  status_led_set(false);
  webserver_stop();
  camera_power_down();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  /* Deep sleep sin fuente de despertar = apagado permanente hasta que se
   * pulse RESET o se recargue el pack. */
  esp_deep_sleep_start();
#endif
}

/* ===========================================================================
 * setup
 * ==========================================================================*/
void setup() {
  Serial.begin(SERIAL_BAUD);
  status_led_init();
  delay(200);
  Serial.println();
  LOG("=== %s ===", DEVICE_NAME);
  LOG("Reset reason: %d", (int)esp_reset_reason());

  power_init();

  /* --- Camara ------------------------------------------------------------ */
  if (!camera_init()) {
    LOG("FATAL: la camara no arranca. Revisa el cable plano y la alimentacion.");
    LOG("       El servidor arranca igual para poder ver /battery y /status.");
  }

  /* --- Bateria ----------------------------------------------------------- */
  if (!battery_init()) {
    LOG("BAT: sin medida de bateria; el dashboard lo indicara como no disponible");
  }

  /* --- Red y servidor ---------------------------------------------------- */
  if (!wifi_begin()) {
    /* Sin red no hay nada que servir: reiniciar es mejor que quedarse
     * colgado esperando indefinidamente. */
    LOG("FATAL: no hay red (ni router ni AP propio). Reiniciando en 5 s...");
    delay(5000);
    ESP.restart();
  }

  if (!webserver_start()) {
    LOG("FATAL: el servidor HTTP no arranca. Reiniciando en 5 s...");
    delay(5000);
    ESP.restart();
  }

  /* Patron del LED segun como haya quedado todo. Con pilas, esto es lo unico
   * que vas a poder leer. */
  if (!camera_ready())   s_led_blinks = 3;   // la camara no arranco
  else if (s_ap_mode)    s_led_blinks = 2;   // red propia
  else                   s_led_blinks = 1;   // conectado al router

  const String ip = s_ap_mode ? WiFi.softAPIP().toString()
                              : WiFi.localIP().toString();
  LOG("LISTO -> http://%s/   (o http://%s.local/)", ip.c_str(), DEVICE_HOSTNAME);
  LOG("PRUEBA DE CAMARA SIN JAVASCRIPT -> http://%s%s", ip.c_str(), ROUTE_TEST);
}

/* ===========================================================================
 * loop
 * ==========================================================================*/
void loop() {
  wifi_watchdog();

  /* Solo mide cuando toca segun BATTERY_READ_INTERVAL_MS; el resto de
   * llamadas retornan de inmediato. El dashboard lee el valor cacheado. */
  battery_update();
  battery_guard();

  power_tick();
  status_led_tick();
}
