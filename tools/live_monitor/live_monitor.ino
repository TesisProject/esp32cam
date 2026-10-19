/* ============================================================================
 *  live_monitor.ino  -  Bateria, WiFi y camara en vivo de la ESP32-CAM
 *
 *  Herramienta de banco/campo, NO es el firmware de campo: no hay deep sleep,
 *  no sube nada al Fog y no escribe la NVS. Junta en una sola pagina web lo
 *  de battery_monitor y wifi_rssi, y anade la camara en tiempo real (MJPEG):
 *    - Bateria: tension, %, lectura cruda de GPIO13 y curva de las ultimas 6 h;
 *    - WiFi: RSSI del AP (actual, media, min/max), canal, BSSID, desconexiones;
 *    - Camara: video en vivo, foto fija a la resolucion elegida, fps y KB/frame.
 *  Sirve para orientar la camara sobre las plazas (lo que se ve es lo que
 *  vera el Fog) mientras se vigila la senal y la bateria en el sitio real.
 *
 *  La placa se conecta a la red como estacion, igual que el firmware de
 *  campo (asi el RSSI es el del enlace real). La WiFi la lee de la NVS (solo
 *  lectura); si no la tiene, la pide por el serie y NO la guarda.
 *
 *  GPIO13 es ADC2 y el ADC2 no se puede leer con el WiFi encendido. Cada
 *  intervalo de bateria (60 s por defecto, ajustable en la web) la placa
 *  corta el video, para los servidores, apaga la camara y el WiFi, espera
 *  MON_SETTLE_MS, mide y lo vuelve a levantar todo (~3-5 s de hueco, casi
 *  todo es la reconexion WiFi + DHCP). La pagina reabre el video sola.
 *  Apagar tambien la camara deja la medida mas cerca de la del campo, donde
 *  se lee al despertar con el OV2640 en PWDN.
 *
 *  Servidores (esp_http_server del core, cada uno en su tarea):
 *    puerto 80: /, /api, /csv, /capture, /control   (UI y datos)
 *    puerto 81: /stream                             (MJPEG, un cliente a la vez)
 *  El video va en otro puerto porque un servidor atiende una peticion a la
 *  vez: con el stream en el 80, /api no responderia nunca.
 *
 *  Copia a mano (el IDE no incluye ficheros de fuera del sketch) los pines y
 *  ajustes de camara de config.h, la recta y la curva de
 *  src/battery/battery_module.cpp y las claves/blobs de
 *  src/settings/settings_module.*. Si cambian alli, cambiarlas aqui.
 *
 *  Compilar con el MISMO esquema de particiones que el firmware principal,
 *  para no mover la NVS, y con PSRAM (la placa esp32cam ya la trae):
 *    arduino-cli compile --fqbn esp32:esp32:esp32cam:PartitionScheme=min_spiffs --build-path build .
 * ==========================================================================*/
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <driver/rtc_io.h>
#include <esp_system.h>

/* --- Ajustes de la herramienta -------------------------------------------- */
#define MON_SERIAL_BAUD         115200
#define MON_CONNECT_TIMEOUT_MS  20000
#define MON_PROMPT_TIMEOUT_MS   120000  // espera de SSID/clave por serie si la NVS no tiene WiFi
#define MON_BAT_INTERVAL_S      60      // defecto; 0 = no medir (video sin cortes). Ajustable en la web
#define MON_SETTLE_MS           500     // tras apagar WiFi y camara: la celda recupera la caida de la carga
#define MON_HISTORY_LEN         720     // 12 h a 60 s, 6 h a 30 s; ~8,6 KB de RAM
#define MON_RSSI_SAMPLE_MS      200     // el driver actualiza el RSSI con cada baliza (~102 ms)
#define MON_REPORT_MS           5000    // una linea de resumen en el serie
#define MON_STREAM_STOP_MS      3000    // espera maxima a que el stream suelte la camara
#define MON_STREAM_SEND_TIMEOUT_S 2     // un cliente lento no retiene el frame mas que esto
/* Misma potencia que el firmware de campo (WIFI_TX_POWER_DBM de config.h).
 * Con video la radio transmite sin parar: si con la celda + MT3608 la placa
 * se reinicia por brownout, bajar a 11 u 8 (el RSSI medido no cambia, es el
 * de bajada; si baja el caudal del video). */
#define MON_TX_POWER_DBM        17

/* Video: arranca con lo mismo que la foto de campo (SVGA, calidad 12) para
 * que el encuadre y el detalle sean los que recibira el Fog. Se cambia en la
 * web; no se guarda. */
#define MON_STREAM_FRAMESIZE    FRAMESIZE_SVGA
#define MON_STREAM_QUALITY      12

/* Umbrales de las barras de RSSI, dBm (mismos que wifi_rssi). */
#define RSSI_BAR4_DBM           -60
#define RSSI_BAR3_DBM           -67
#define RSSI_BAR2_DBM           -75
#define RSSI_BAR1_DBM           -82

/* --- Copiado de config.h del firmware principal (mantener igual) --------- */
#define WIFI_SSID_MAX_LEN       32
#define WIFI_PASS_MAX_LEN       63
#define CAM_CODE_MAX_LEN        15
#define API_SECRET_MAX_LEN      63

#define CAM_PIN_PWDN            32
#define CAM_PIN_RESET           -1
#define CAM_PIN_XCLK             0
#define CAM_PIN_SIOD            26
#define CAM_PIN_SIOC            27
#define CAM_PIN_D7              35
#define CAM_PIN_D6              34
#define CAM_PIN_D5              39
#define CAM_PIN_D4              36
#define CAM_PIN_D3              21
#define CAM_PIN_D2              19
#define CAM_PIN_D1              18
#define CAM_PIN_D0               5
#define CAM_PIN_VSYNC           25
#define CAM_PIN_HREF            23
#define CAM_PIN_PCLK            22
#define CAM_PIN_FLASH_LED        4      // flash blanco (~150 mA): no se usa
#define CAM_XCLK_FREQ_HZ        20000000
#define CAM_FB_COUNT            2
#define CAM_VFLIP               0
#define CAM_HMIRROR             0
#define CAM_BRIGHTNESS          0
#define CAM_CONTRAST            0
#define CAM_SATURATION          0
#define STATUS_LED_PIN          33      // LED rojo pequeno, LOW enciende

#define BATTERY_ADC_PIN              13
#define BATTERY_ADC_SAMPLES          32
#define BATTERY_ADC_SAMPLE_GAP_MS    2
#define BATTERY_CAL_ADC_A_MV_DEFAULT 1500
#define BATTERY_CAL_BAT_A_MV_DEFAULT 3000
#define BATTERY_CAL_ADC_B_MV_DEFAULT 2100
#define BATTERY_CAL_BAT_B_MV_DEFAULT 4200
#define BATTERY_CAL_MIN_BAT_SPAN_MV  300
#define BATTERY_CAL_MIN_ADC_SPAN_MV  30
#define BATTERY_CAL_ADC_MAX_MV       3300
#define BATTERY_VALID_MIN_MV         2500
#define BATTERY_VALID_MAX_MV         4500
#define BATTERY_CUTOFF_MV            3300
static const int32_t kCurveMv[]  = { 3300, 3500, 3650, 3750, 3850, 4000, 4200 };
static const int     kCurvePct[] = {    0,    9,   28,   43,   59,   79,  100 };
static const size_t  kCurveLen   = sizeof(kCurveMv) / sizeof(kCurveMv[0]);

/* --- Copiado de settings_module (forma de los blobs en la NVS) ----------- */
#define SETTINGS_NVS_NAMESPACE  "parkvision"
#define SETTINGS_SCHEMA_VERSION 1
struct NvsWifi   { char ssid[WIFI_SSID_MAX_LEN + 1]; char password[WIFI_PASS_MAX_LEN + 1]; };
struct NvsBatCal { uint16_t adc_a_mv, bat_a_mv, adc_b_mv, bat_b_mv; };
struct NvsCred   { char camera_code[CAM_CODE_MAX_LEN + 1];
                   char api_key[API_SECRET_MAX_LEN + 1];
                   char api_token[API_SECRET_MAX_LEN + 1]; };
static_assert(sizeof(NvsWifi) == 97, "debe coincidir con SettingsWifi");
static_assert(sizeof(NvsBatCal) == 8, "debe coincidir con SettingsBatteryCal");
static_assert(sizeof(NvsCred) == 144, "debe coincidir con SettingsCredential");

/* --- Estado ----------------------------------------------------------------*/
struct Sample {
  uint32_t t_s;        // segundos desde el arranque
  uint16_t adc_mv;
  int16_t  bat_mv;
  int8_t   pct;
  bool     valid;
};

static Sample    s_hist[MON_HISTORY_LEN];
static size_t    s_hist_count = 0;
static size_t    s_hist_next  = 0;
static Sample    s_bat        = {};
static bool      s_bat_have   = false;
static uint32_t  s_bat_fails  = 0;     // lecturas del ADC2 que fallaron en la ultima medida
static uint32_t  s_bat_ms     = 0;
static uint32_t  s_bat_interval_s = MON_BAT_INTERVAL_S;
static volatile bool s_bat_now = false;  // "Medir ahora" desde la web o el serie

static NvsBatCal s_cal        = { BATTERY_CAL_ADC_A_MV_DEFAULT, BATTERY_CAL_BAT_A_MV_DEFAULT,
                                  BATTERY_CAL_ADC_B_MV_DEFAULT, BATTERY_CAL_BAT_B_MV_DEFAULT };
static bool      s_cal_nvs    = false;
static NvsWifi   s_wifi       = {};
static char      s_cam_code[CAM_CODE_MAX_LEN + 1] = "?";

/* RSSI desde el ultimo reinicio de la estadistica. Las desconexiones
 * provocadas por la medida de bateria no cuentan. */
static int       s_rssi = 0, s_rssi_min = 0, s_rssi_max = -127;
static int64_t   s_rssi_sum = 0;
static uint32_t  s_rssi_n = 0, s_drops = 0, s_stats_since_ms = 0;

/* Camara. Los ajustes se tocan desde la tarea del servidor 80 y se
 * reaplican desde loop() al reiniciar la camara tras cada medida. */
static bool          s_cam_ok      = false;
static framesize_t   s_framesize   = MON_STREAM_FRAMESIZE;
static int           s_quality     = MON_STREAM_QUALITY;
static framesize_t   s_max_frame   = FRAMESIZE_UXGA;  // el driver reserva buffers para este

/* Stream: lo escribe la tarea del servidor 81, lo lee loop()/api. */
static volatile bool     s_pause     = false;   // pide al stream que suelte la camara
static volatile int      s_streaming = 0;
static volatile uint32_t s_frames = 0, s_frame_bytes = 0;
static float             s_fps = 0, s_kb_frame = 0;

static volatile bool s_measuring = false;
static httpd_handle_t s_http = nullptr, s_stream = nullptr;

/* ========================================================================== */

static void led_set(bool on) { digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH); }

static int bars_for(int rssi) {
  if (rssi >= RSSI_BAR4_DBM) return 4;
  if (rssi >= RSSI_BAR3_DBM) return 3;
  if (rssi >= RSSI_BAR2_DBM) return 2;
  if (rssi >= RSSI_BAR1_DBM) return 1;
  return 0;
}

static const char *label_for(int bars) {
  static const char *kLabels[] = { "muy mala", "mala", "justa", "buena", "excelente" };
  return kLabels[bars];
}

static void rssi_reset() {
  s_rssi_min = 0; s_rssi_max = -127; s_rssi_sum = 0; s_rssi_n = 0; s_drops = 0;
  s_stats_since_ms = millis();
}

/* --- NVS (solo lectura) ----------------------------------------------------*/
static bool cal_valid(const NvsBatCal &c) {
  return c.adc_a_mv <= BATTERY_CAL_ADC_MAX_MV && c.adc_b_mv <= BATTERY_CAL_ADC_MAX_MV &&
         c.bat_a_mv >= BATTERY_VALID_MIN_MV && c.bat_a_mv <= BATTERY_VALID_MAX_MV &&
         c.bat_b_mv >= BATTERY_VALID_MIN_MV && c.bat_b_mv <= BATTERY_VALID_MAX_MV &&
         (int)c.bat_b_mv - (int)c.bat_a_mv >= BATTERY_CAL_MIN_BAT_SPAN_MV &&
         (int)c.adc_b_mv - (int)c.adc_a_mv >= BATTERY_CAL_MIN_ADC_SPAN_MV;
}

/* Devuelve true si hay WiFi. La calibracion y el id de la camara son
 * opcionales. Nunca escribe ni borra la NVS. */
static bool load_nvs() {
  Preferences p;
  if (!p.begin(SETTINGS_NVS_NAMESPACE, true)) {
    Serial.println("NVS: sin namespace (placa sin aprovisionar)");
    return false;
  }
  const uint8_t schema = p.getUChar("schema", 0);
  bool wifi_ok = false;
  if (schema != SETTINGS_SCHEMA_VERSION) {
    Serial.printf("NVS: esquema %u, esta herramienta entiende el %u: se ignora la NVS\n",
                  schema, SETTINGS_SCHEMA_VERSION);
  } else {
    if (p.getBytesLength("wifi") == sizeof(NvsWifi) &&
        p.getBytes("wifi", &s_wifi, sizeof(NvsWifi)) == sizeof(NvsWifi)) {
      s_wifi.ssid[WIFI_SSID_MAX_LEN] = '\0';
      s_wifi.password[WIFI_PASS_MAX_LEN] = '\0';
      wifi_ok = s_wifi.ssid[0] != '\0';
    }
    NvsBatCal cal;
    if (p.getBytesLength("bat_cal") == sizeof(NvsBatCal) &&
        p.getBytes("bat_cal", &cal, sizeof(NvsBatCal)) == sizeof(NvsBatCal) && cal_valid(cal)) {
      s_cal = cal;
      s_cal_nvs = true;
    }
    /* Solo interesa el id ("CAM-002") para saber que placa es: la apiKey y
     * el apiToken se borran de la RAM en cuanto se copia. */
    NvsCred cred;
    if (p.getBytesLength("cred") == sizeof(NvsCred) &&
        p.getBytes("cred", &cred, sizeof(NvsCred)) == sizeof(NvsCred)) {
      cred.camera_code[CAM_CODE_MAX_LEN] = '\0';
      strlcpy(s_cam_code, cred.camera_code, sizeof(s_cam_code));
    }
    memset(&cred, 0, sizeof(cred));
  }
  p.end();
  Serial.printf("NVS: camara %s | cal. %s: %u->%u, %u->%u\n", s_cam_code,
                s_cal_nvs ? "NVS" : "defecto",
                s_cal.adc_a_mv, s_cal.bat_a_mv, s_cal.adc_b_mv, s_cal.bat_b_mv);
  return wifi_ok;
}

/* Lee una linea del serie. Fin de linea: CR, LF o CRLF (el monitor del IDE
 * manda solo CR por defecto); el segundo caracter de un CRLF se salta. */
static bool read_line(char *out, size_t cap, uint32_t timeout_ms) {
  size_t n = 0;
  const uint32_t t0 = millis();
  while (millis() - t0 < timeout_ms) {
    while (Serial.available()) {
      const char c = (char)Serial.read();
      if (c == '\r' || c == '\n') {
        if (n == 0) continue;
        out[n] = '\0';
        return true;
      }
      if (n < cap - 1) out[n++] = c;
    }
    delay(10);
  }
  return false;
}

static bool prompt_wifi() {
  Serial.println("WIFI: escribe el SSID y pulsa Enter (no se guarda en la placa)");
  if (!read_line(s_wifi.ssid, sizeof(s_wifi.ssid), MON_PROMPT_TIMEOUT_MS)) return false;
  Serial.println("WIFI: ahora la contrasena y Enter");
  return read_line(s_wifi.password, sizeof(s_wifi.password), MON_PROMPT_TIMEOUT_MS);
}

/* --- Bateria -------------------------------------------------------------- */
static int percent_from_mv(int32_t mv) {
  if (mv <= kCurveMv[0]) return kCurvePct[0];
  for (size_t i = 1; i < kCurveLen; i++) {
    if (mv < kCurveMv[i]) {
      return kCurvePct[i - 1] + (int)((int64_t)(mv - kCurveMv[i - 1]) *
             (kCurvePct[i] - kCurvePct[i - 1]) / (kCurveMv[i] - kCurveMv[i - 1]));
    }
  }
  return kCurvePct[kCurveLen - 1];
}

/* Igual que battery_read() del firmware principal. Llamar con el WiFi APAGADO. */
static void battery_measure() {
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  uint32_t sum = 0, good = 0;
  for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
    const uint32_t mv = analogReadMilliVolts(BATTERY_ADC_PIN);
    if (mv > 0) { sum += mv; good++; }   // 0 = lectura fallida (ADC2 ocupado)
    delay(BATTERY_ADC_SAMPLE_GAP_MS);
  }
  const int32_t adc = good ? (int32_t)(sum / good) : 0;
  const int32_t bat = (int32_t)s_cal.bat_a_mv +
                      (adc - (int32_t)s_cal.adc_a_mv) *
                      ((int32_t)s_cal.bat_b_mv - (int32_t)s_cal.bat_a_mv) /
                      ((int32_t)s_cal.adc_b_mv - (int32_t)s_cal.adc_a_mv);

  s_bat.t_s    = millis() / 1000;
  s_bat.adc_mv = (uint16_t)adc;
  s_bat.bat_mv = (int16_t)constrain(bat, -32000, 32000);
  s_bat.pct    = (int8_t)percent_from_mv(bat);
  s_bat.valid  = good >= BATTERY_ADC_SAMPLES / 2 &&
                 bat >= BATTERY_VALID_MIN_MV && bat <= BATTERY_VALID_MAX_MV;
  s_bat_fails  = BATTERY_ADC_SAMPLES - good;
  s_bat_ms     = millis();
  s_bat_have   = true;

  s_hist[s_hist_next] = s_bat;
  s_hist_next = (s_hist_next + 1) % MON_HISTORY_LEN;
  if (s_hist_count < MON_HISTORY_LEN) s_hist_count++;

  /* Mismo formato que el log del firmware principal: sirve para calibrar. */
  Serial.printf("BAT: ADC IO13: %u mV | Bateria: %.2f V | %d %% %s(cal. %s)%s\n",
                s_bat.adc_mv, s_bat.bat_mv / 1000.0f, s_bat.pct,
                s_bat.valid ? "" : "[NO VALIDA] ", s_cal_nvs ? "NVS" : "defecto",
                s_bat_fails ? " (fallaron lecturas del ADC2)" : "");
}

/* --- Camara ----------------------------------------------------------------*/
static const char *framesize_name(framesize_t f) {
  switch (f) {
    case FRAMESIZE_QVGA: return "QVGA";
    case FRAMESIZE_VGA:  return "VGA";
    case FRAMESIZE_SVGA: return "SVGA";
    case FRAMESIZE_XGA:  return "XGA";
    case FRAMESIZE_SXGA: return "SXGA";
    case FRAMESIZE_UXGA: return "UXGA";
    default:             return "?";
  }
}

static bool framesize_from_name(const char *n, framesize_t *out) {
  static const framesize_t kAll[] = { FRAMESIZE_QVGA, FRAMESIZE_VGA, FRAMESIZE_SVGA,
                                      FRAMESIZE_XGA, FRAMESIZE_SXGA, FRAMESIZE_UXGA };
  for (framesize_t f : kAll) {
    if (strcmp(n, framesize_name(f)) == 0 && f <= s_max_frame) { *out = f; return true; }
  }
  return false;
}

static void camera_apply() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  s->set_framesize(s, s_framesize);
  s->set_quality(s, s_quality);
  s->set_vflip(s, CAM_VFLIP);
  s->set_hmirror(s, CAM_HMIRROR);
  s->set_brightness(s, CAM_BRIGHTNESS);
  s->set_contrast(s, CAM_CONTRAST);
  s->set_saturation(s, CAM_SATURATION);
  s->set_gain_ctrl(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_whitebal(s, 1);
}

/* Igual que camera_init() del firmware principal, salvo que reserva los
 * buffers para UXGA (con PSRAM) y asi se puede subir de resolucion en vivo. */
static bool camera_start() {
  rtc_gpio_hold_dis((gpio_num_t)CAM_PIN_PWDN);
  camera_config_t cfg = {};
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0 = CAM_PIN_D0; cfg.pin_d1 = CAM_PIN_D1; cfg.pin_d2 = CAM_PIN_D2;
  cfg.pin_d3 = CAM_PIN_D3; cfg.pin_d4 = CAM_PIN_D4; cfg.pin_d5 = CAM_PIN_D5;
  cfg.pin_d6 = CAM_PIN_D6; cfg.pin_d7 = CAM_PIN_D7;
  cfg.pin_xclk = CAM_PIN_XCLK; cfg.pin_pclk = CAM_PIN_PCLK;
  cfg.pin_vsync = CAM_PIN_VSYNC; cfg.pin_href = CAM_PIN_HREF;
  cfg.pin_sccb_sda = CAM_PIN_SIOD; cfg.pin_sccb_scl = CAM_PIN_SIOC;
  cfg.pin_pwdn = CAM_PIN_PWDN; cfg.pin_reset = CAM_PIN_RESET;
  cfg.xclk_freq_hz = CAM_XCLK_FREQ_HZ;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.grab_mode    = CAMERA_GRAB_LATEST;   // el video siempre muestra el frame mas reciente
  if (psramFound()) {
    s_max_frame     = FRAMESIZE_UXGA;
    cfg.fb_count    = CAM_FB_COUNT;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
    cfg.jpeg_quality = 10;                 // el buffer se dimensiona con la peor calidad usada
  } else {
    /* Sin PSRAM solo cabe QVGA en la RAM interna, como en el firmware de campo. */
    s_max_frame     = FRAMESIZE_QVGA;
    cfg.fb_count    = 1;
    cfg.fb_location = CAMERA_FB_IN_DRAM;
    cfg.jpeg_quality = 15;
    if (s_framesize > FRAMESIZE_QVGA) s_framesize = FRAMESIZE_QVGA;
  }
  cfg.frame_size = s_max_frame;
  const esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("CAM: esp_camera_init fallo (0x%x)\n", err);
    return false;
  }
  camera_apply();
  return true;
}

/* El driver deja PWDN como salida; se fuerza en alto para que el OV2640 no
 * consuma durante la medida de bateria. */
static void camera_stop() {
  if (s_cam_ok) esp_camera_deinit();
  s_cam_ok = false;
  pinMode(CAM_PIN_PWDN, OUTPUT);
  digitalWrite(CAM_PIN_PWDN, HIGH);
}

/* --- WiFi ------------------------------------------------------------------*/
static bool wifi_up() {
  WiFi.persistent(false);              // el driver no guarda copia de la clave
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                // sin modem sleep: video fluido y RSSI de cada baliza
  WiFi.setTxPower((wifi_power_t)(MON_TX_POWER_DBM * 4));
  WiFi.begin(s_wifi.ssid, s_wifi.password);
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < MON_CONNECT_TIMEOUT_MS) {
    led_set((millis() / 100) % 2);     // parpadeo rapido mientras conecta
    delay(50);
  }
  led_set(false);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("WIFI: sin conexion tras %lu ms (status %d)\n",
                  (unsigned long)MON_CONNECT_TIMEOUT_MS, (int)WiFi.status());
    return false;
  }
  Serial.printf("WIFI: conectado en %lu ms, IP %s, BSSID %s, canal %d\n",
                (unsigned long)(millis() - t0), WiFi.localIP().toString().c_str(),
                WiFi.BSSIDstr().c_str(), (int)WiFi.channel());
  return true;
}

/* El driver WiFi tiene que soltar el ADC2: no basta con desconectar. */
static void wifi_down() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

/* --- Web: pagina -----------------------------------------------------------*/
static const char kPage[] = R"HTML(<!doctype html>
<html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Monitor ESP32-CAM</title>
<style>
body{font-family:system-ui,sans-serif;margin:0 auto;padding:16px;background:#111;color:#eee;max-width:900px}
h1{font-size:1.1rem;font-weight:600;color:#aaa;margin:0 0 10px}
h2{font-size:.95rem;font-weight:600;color:#aaa;margin:18px 0 8px}
.cam{position:relative;background:#000;border-radius:8px;overflow:hidden;min-height:180px}
.cam img{display:block;width:100%;height:auto}
#camst{position:absolute;left:8px;top:8px;background:#000a;padding:2px 8px;border-radius:4px;font-size:.85rem}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:8px 0}
select,button{font-size:1rem;padding:6px 10px;background:#222;color:#eee;border:1px solid #444;border-radius:6px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:8px}
.card{background:#1d1d1d;border-radius:8px;padding:10px}
.card b{display:block;font-size:1.3rem}
.muted{color:#888;font-size:.85rem}
.bar{height:14px;background:#333;border-radius:4px;overflow:hidden;margin-top:6px}
.fill{height:100%;transition:width .5s}
svg{width:100%;height:180px;background:#1d1d1d;border-radius:8px;margin-top:8px}
a{color:#6af}
</style></head><body>
<h1>Monitor ESP32-CAM &middot; <span id="code">--</span></h1>
<div class="cam"><img id="img" alt=""><span id="camst">conectando...</span></div>
<div class="row">
 <select id="fs" onchange="ctl('framesize',this.value)"></select>
 <select id="q" onchange="ctl('quality',this.value)"></select>
 <button onclick="window.open('/capture?t='+Date.now())">Foto fija</button>
 <span class="muted" id="cami"></span>
</div>
<h2>Bater&iacute;a</h2>
<div class="grid">
 <div class="card"><span class="muted">Tensi&oacute;n</span><b id="v">--</b>
  <div class="bar"><div class="fill" id="fill" style="width:0"></div></div></div>
 <div class="card"><span class="muted">Porcentaje</span><b id="pct">--</b></div>
 <div class="card"><span class="muted">ADC en GPIO13</span><b id="adc">--</b></div>
 <div class="card"><span class="muted">&Uacute;ltima medida</span><b id="age">--</b></div>
</div>
<div class="row">
 <span class="muted">Medir cada</span>
 <select id="bi" onchange="ctl('bat_interval',this.value)">
  <option value="0">nunca (v&iacute;deo sin cortes)</option><option value="30">30 s</option>
  <option value="60">60 s</option><option value="300">5 min</option></select>
 <button onclick="ctl('bat_now',1)">Medir ahora</button>
 <a href="/csv">CSV</a>
</div>
<svg id="chart" viewBox="0 0 600 180" preserveAspectRatio="none"></svg>
<p class="muted" id="bati"></p>
<h2>WiFi</h2>
<div class="grid">
 <div class="card"><span class="muted">RSSI</span><b id="rssi">--</b></div>
 <div class="card"><span class="muted">Media (min / max)</span><b id="ravg">--</b></div>
 <div class="card"><span class="muted">Desconexiones</span><b id="drops">--</b></div>
 <div class="card"><span class="muted">Canal / BSSID</span><b id="ch" style="font-size:1rem">--</b></div>
</div>
<div class="row"><button onclick="ctl('rssi_reset',1)">Reiniciar estad&iacute;stica</button>
 <span class="muted" id="sys"></span></div>
<p class="muted">Cada medida de bater&iacute;a apaga c&aacute;mara y WiFi unos segundos (GPIO13 es ADC2):
el v&iacute;deo se corta y vuelve solo.</p>
<script>
const $=id=>document.getElementById(id);let last=null,down=false,first=true;
const C=['#d33','#e80','#db0','#7c3','#2b5'];
function fmt(s){s=Math.round(s);return s<120?s+' s':s<7200?Math.round(s/60)+' min':(s/3600).toFixed(1)+' h'}
function stream(){$('img').src='http://'+location.hostname+':81/stream?t='+Date.now()}
$('img').onerror=()=>{$('camst').textContent='sin vídeo, reintentando...';setTimeout(stream,2000)};
$('img').onload=()=>{$('camst').textContent='en vivo'};
async function ctl(k,v){try{await fetch('/control?'+k+'='+encodeURIComponent(v),{cache:'no-store'});poll()}catch(e){}}
function opts(sel,list,cur){if(sel.options.length)return;for(const o of list)sel.add(new Option(o[1],o[0]));sel.value=cur}
async function poll(){
 try{const d=await (await fetch('/api',{cache:'no-store'})).json();d.at=Date.now();last=d;
  if(down||first){down=false;first=false;stream();chart()}render()}
 catch(e){if(!down){down=true;$('camst').textContent='midiendo batería...'}}
}
function render(){const d=last;
 $('code').textContent=d.camera;
 opts($('fs'),d.framesizes.map(f=>[f,f+(f=='SVGA'?' (campo)':'')]),d.framesize);
 opts($('q'),[10,12,15,20,30].map(q=>[q,'calidad '+q+(q==12?' (campo)':'')]),d.quality);
 $('fs').value=d.framesize;$('q').value=d.quality;$('bi').value=d.bat_interval_s;
 $('cami').textContent=d.cam_ok?d.fps.toFixed(1)+' fps · '+d.kb_frame.toFixed(1)+' KB/frame':'cámara no iniciada';
 if(d.bat){const b=d.bat,ok=b.valid,col=b.pct<15?'#e44':b.pct<40?'#eb4':'#4c6';
  $('v').textContent=(b.mv/1000).toFixed(2)+' V';$('v').style.color=ok?'':'#e44';
  $('pct').textContent=ok?b.pct+' %':'no válida';
  $('fill').style.width=(ok?b.pct:0)+'%';$('fill').style.background=col;
  $('adc').textContent=b.adc_mv+' mV';
  $('bati').textContent='Calibración: '+d.cal+' ('+d.cal_pts.join(', ')+')'+(b.fails?' · '+b.fails+' lecturas ADC fallidas':'');}
 const r=d.wifi;$('rssi').textContent=r.rssi+' dBm · '+r.label;$('rssi').style.color=C[r.bars];
 $('ravg').textContent=r.n?r.avg.toFixed(1)+' ('+r.min+' / '+r.max+')':'--';
 $('drops').textContent=r.drops;$('ch').textContent=r.ch+' / '+r.bssid;
 $('sys').textContent='encendida hace '+fmt(d.uptime_s)+' · heap '+Math.round(d.heap/1024)+' KB · reset '+d.reset_reason;
}
setInterval(()=>{if(last&&last.bat)$('age').textContent='hace '+fmt(last.bat.age_s+(Date.now()-last.at)/1000)},1000);
async function chart(){
 try{const t=await (await fetch('/csv',{cache:'no-store'})).text();
  const rows=t.trim().split('\n').slice(1).map(l=>l.split(',').map(Number)).filter(r=>r[4]);
  const W=600,H=180,lo=2900,hi=4300,y=v=>H-(v-lo)/(hi-lo)*H;let s='';
  for(const v of [3000,3500,4000])s+=`<line x1=0 x2=${W} y1=${y(v)} y2=${y(v)} stroke=#333 /><text x=4 y=${y(v)-3} fill=#666 font-size=11>${v/1000} V</text>`;
  s+=`<line x1=0 x2=${W} y1=${y(3300)} y2=${y(3300)} stroke=#e44 stroke-dasharray=4 /><text x=${W-70} y=${y(3300)-3} fill=#e44 font-size=11>corte 3,3 V</text>`;
  if(rows.length>1){const t0=rows[0][0],t1=rows[rows.length-1][0];
   s+='<polyline fill=none stroke=#6af stroke-width=2 points="'+rows.map(r=>((r[0]-t0)/Math.max(1,t1-t0)*W).toFixed(1)+','+y(r[2]).toFixed(1)).join(' ')+'"/>';}
  $('chart').innerHTML=s}catch(e){}
}
poll();setInterval(poll,1000);setInterval(chart,60000);
</script></body></html>)HTML";

/* --- Web: handlers del puerto 80 -------------------------------------------*/
static esp_err_t send_no_store(httpd_req_t *req, const char *type, const char *body, ssize_t len) {
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body, len);
}

static esp_err_t handle_root(httpd_req_t *req) {
  return send_no_store(req, "text/html; charset=utf-8", kPage, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_api(httpd_req_t *req) {
  char bat[200] = "null";
  if (s_bat_have) {
    snprintf(bat, sizeof(bat),
             "{\"adc_mv\":%u,\"mv\":%d,\"pct\":%d,\"valid\":%s,\"fails\":%lu,\"age_s\":%lu}",
             s_bat.adc_mv, s_bat.bat_mv, s_bat.pct, s_bat.valid ? "true" : "false",
             (unsigned long)s_bat_fails, (unsigned long)((millis() - s_bat_ms) / 1000));
  }
  const int bars = bars_for(s_rssi);
  char fs_list[64] = "";
  for (int f = FRAMESIZE_QVGA; f <= (int)s_max_frame; f++) {
    const char *n = framesize_name((framesize_t)f);
    if (strcmp(n, "?") == 0) continue;   // tamanos intermedios (CIF, HVGA...) no se ofrecen
    char item[12];
    snprintf(item, sizeof(item), "%s\"%s\"", fs_list[0] ? "," : "", n);
    strlcat(fs_list, item, sizeof(fs_list));
  }
  char buf[900];
  snprintf(buf, sizeof(buf),
           "{\"camera\":\"%s\",\"uptime_s\":%lu,\"heap\":%lu,\"psram_free\":%lu,\"reset_reason\":%d,"
           "\"bat\":%s,\"bat_interval_s\":%lu,\"cal\":\"%s\",\"cal_pts\":[%u,%u,%u,%u],"
           "\"cutoff_mv\":%d,\"samples\":%u,"
           "\"wifi\":{\"rssi\":%d,\"bars\":%d,\"label\":\"%s\",\"avg\":%.2f,\"min\":%d,\"max\":%d,"
           "\"n\":%lu,\"secs\":%lu,\"drops\":%lu,\"ch\":%d,\"bssid\":\"%s\"},"
           "\"cam_ok\":%s,\"framesize\":\"%s\",\"quality\":%d,\"framesizes\":[%s],"
           "\"fps\":%.2f,\"kb_frame\":%.2f,\"streaming\":%d}",
           s_cam_code, (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap(),
           (unsigned long)ESP.getFreePsram(), (int)esp_reset_reason(),
           bat, (unsigned long)s_bat_interval_s, s_cal_nvs ? "NVS" : "defecto",
           s_cal.adc_a_mv, s_cal.bat_a_mv, s_cal.adc_b_mv, s_cal.bat_b_mv,
           BATTERY_CUTOFF_MV, (unsigned)s_hist_count,
           s_rssi, bars, label_for(bars), s_rssi_n ? (double)s_rssi_sum / s_rssi_n : 0.0,
           s_rssi_n ? s_rssi_min : 0, s_rssi_n ? s_rssi_max : 0,
           (unsigned long)s_rssi_n, (unsigned long)((millis() - s_stats_since_ms) / 1000),
           (unsigned long)s_drops, (int)WiFi.channel(), WiFi.BSSIDstr().c_str(),
           s_cam_ok ? "true" : "false", framesize_name(s_framesize), s_quality, fs_list,
           s_fps, s_kb_frame, (int)s_streaming);
  return send_no_store(req, "application/json", buf, HTTPD_RESP_USE_STRLEN);
}

/* Historial de bateria, del mas antiguo al mas reciente, por trozos (no se
 * monta el CSV entero en el heap). */
static esp_err_t handle_csv(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/csv");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_sendstr_chunk(req, "t_s,adc_mv,bat_mv,pct,valid\n");
  const size_t start = (s_hist_next + MON_HISTORY_LEN - s_hist_count) % MON_HISTORY_LEN;
  char chunk[1024];
  size_t used = 0;
  for (size_t i = 0; i < s_hist_count; i++) {
    const Sample &h = s_hist[(start + i) % MON_HISTORY_LEN];
    used += snprintf(chunk + used, sizeof(chunk) - used, "%lu,%u,%d,%d,%d\n",
                     (unsigned long)h.t_s, h.adc_mv, h.bat_mv, h.pct, h.valid ? 1 : 0);
    if (sizeof(chunk) - used < 48) { httpd_resp_send_chunk(req, chunk, used); used = 0; }
  }
  if (used) httpd_resp_send_chunk(req, chunk, used);
  return httpd_resp_send_chunk(req, nullptr, 0);
}

/* Una foto a la resolucion y calidad actuales: la que se guardaria para
 * marcar las ROI. */
static esp_err_t handle_capture(httpd_req_t *req) {
  if (!s_cam_ok) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camara no iniciada");
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fallo al capturar");
  char name[64];
  snprintf(name, sizeof(name), "inline; filename=%s-%s.jpg", s_cam_code, framesize_name(s_framesize));
  httpd_resp_set_hdr(req, "Content-Disposition", name);
  const esp_err_t res = send_no_store(req, "image/jpeg", (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

/* /control?framesize=VGA | quality=15 | bat_interval=60 | bat_now=1 | rssi_reset=1 */
static esp_err_t handle_control(httpd_req_t *req) {
  char q[96], v[16];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta el parametro");
  bool ok = false;
  if (httpd_query_key_value(q, "framesize", v, sizeof(v)) == ESP_OK) {
    framesize_t f;
    if (framesize_from_name(v, &f)) {
      s_framesize = f;
      sensor_t *s = esp_camera_sensor_get();
      if (s_cam_ok && s) s->set_framesize(s, f);
      Serial.printf("CAM: resolucion %s\n", v);
      ok = true;
    }
  } else if (httpd_query_key_value(q, "quality", v, sizeof(v)) == ESP_OK) {
    const int qv = atoi(v);
    if (qv >= 10 && qv <= 63) {
      s_quality = qv;
      sensor_t *s = esp_camera_sensor_get();
      if (s_cam_ok && s) s->set_quality(s, qv);
      Serial.printf("CAM: calidad %d\n", qv);
      ok = true;
    }
  } else if (httpd_query_key_value(q, "bat_interval", v, sizeof(v)) == ESP_OK) {
    const long s = atol(v);
    if (s == 0 || (s >= 10 && s <= 3600)) {
      s_bat_interval_s = (uint32_t)s;
      Serial.printf("BAT: medir cada %ld s%s\n", s, s ? "" : " (desactivado)");
      ok = true;
    }
  } else if (httpd_query_key_value(q, "bat_now", v, sizeof(v)) == ESP_OK) {
    s_bat_now = true;
    ok = true;
  } else if (httpd_query_key_value(q, "rssi_reset", v, sizeof(v)) == ESP_OK) {
    rssi_reset();
    Serial.println("STATS: RSSI reiniciada (web)");
    ok = true;
  }
  if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "parametro no valido");
  return send_no_store(req, "text/plain", "ok", HTTPD_RESP_USE_STRLEN);
}

/* --- Web: stream del puerto 81 ---------------------------------------------*/
#define PART_BOUNDARY "pvframe"
static const char kStreamType[]     = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char kStreamBoundary[] = "\r\n--" PART_BOUNDARY "\r\n";

/* Corre en la tarea del servidor 81 hasta que el cliente cierra o loop()
 * pide la camara (s_pause) para medir la bateria. */
static esp_err_t handle_stream(httpd_req_t *req) {
  if (!s_cam_ok || s_pause) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camara ocupada");
  httpd_resp_set_type(req, kStreamType);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  s_streaming = s_streaming + 1;
  Serial.println("CAM: cliente de video conectado");
  esp_err_t res = ESP_OK;
  char part[64];
  while (!s_pause) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { Serial.println("CAM: fallo al capturar frame"); res = ESP_FAIL; break; }
    const int hlen = snprintf(part, sizeof(part),
                              "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                              (unsigned)fb->len);
    res = httpd_resp_send_chunk(req, kStreamBoundary, strlen(kStreamBoundary));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    if (res == ESP_OK) { s_frames = s_frames + 1; s_frame_bytes = s_frame_bytes + fb->len; }
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;   // el cliente cerro la pagina
  }
  if (res == ESP_OK) httpd_resp_send_chunk(req, nullptr, 0);
  s_streaming = s_streaming - 1;
  Serial.println("CAM: cliente de video desconectado");
  return res;
}

static void servers_start() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port   = 32768;
  cfg.stack_size  = 8192;            // /api arma ~900 B en la pila
  cfg.lru_purge_enable = true;       // un movil que cambia de red no deja sockets colgados
  if (httpd_start(&s_http, &cfg) == ESP_OK) {
    const httpd_uri_t uris[] = {
      { "/",        HTTP_GET, handle_root,    nullptr },
      { "/api",     HTTP_GET, handle_api,     nullptr },
      { "/csv",     HTTP_GET, handle_csv,     nullptr },
      { "/capture", HTTP_GET, handle_capture, nullptr },
      { "/control", HTTP_GET, handle_control, nullptr },
    };
    for (const httpd_uri_t &u : uris) httpd_register_uri_handler(s_http, &u);
  } else {
    Serial.println("WEB: no se pudo arrancar el servidor del puerto 80");
    s_http = nullptr;
  }

  cfg.server_port = 81;
  cfg.ctrl_port   = 32769;
  /* Acota cuanto puede bloquearse un envio: el stream tiene que devolver su
   * frame antes de que measure_cycle() haga esp_camera_deinit(). */
  cfg.send_wait_timeout = MON_STREAM_SEND_TIMEOUT_S;
  if (httpd_start(&s_stream, &cfg) == ESP_OK) {
    const httpd_uri_t u = { "/stream", HTTP_GET, handle_stream, nullptr };
    httpd_register_uri_handler(s_stream, &u);
  } else {
    Serial.println("WEB: no se pudo arrancar el servidor del puerto 81");
    s_stream = nullptr;
  }
}

static void servers_stop() {
  if (s_stream) { httpd_stop(s_stream); s_stream = nullptr; }
  if (s_http)   { httpd_stop(s_http);   s_http = nullptr; }
}

/* --- Ciclo de medida de bateria --------------------------------------------
 *  Orden: soltar el stream -> parar servidores -> apagar camara y WiFi ->
 *  esperar -> medir -> levantar camara, WiFi y servidores. El stream tiene
 *  que haber devuelto su frame antes de esp_camera_deinit(). */
static void measure_cycle(bool first) {
  s_measuring = true;
  led_set(true);                         // fijo durante la medida
  if (!first) {
    s_pause = true;
    const uint32_t t0 = millis();
    while (s_streaming > 0 && millis() - t0 < MON_STREAM_STOP_MS) delay(10);
    servers_stop();
    camera_stop();
    wifi_down();
    delay(MON_SETTLE_MS);
  }
  battery_measure();

  s_cam_ok = camera_start();
  if (s_cam_ok) Serial.printf("CAM: lista (%s, calidad %d, PSRAM=%s)\n",
                              framesize_name(s_framesize), s_quality, psramFound() ? "si" : "no");
  s_pause = false;
  if (!first) {
    if (wifi_up()) servers_start();     // si no conecta, loop() sigue reintentando
  }
  led_set(false);
  s_measuring = false;
}

/* --- LED: fijo = midiendo; parpadeo rapido = sin WiFi; N destellos cada 2 s
 *  = N barras de RSSI. 3 destellos largos al arrancar = brownout. ----------*/
static void led_update() {
  if (WiFi.status() != WL_CONNECTED) { led_set((millis() / 100) % 2); return; }
  const uint32_t phase = millis() % 2000;
  const uint32_t slot  = phase / 300;
  led_set(slot < (uint32_t)bars_for(s_rssi) && (phase % 300) < 120);
}

/* ========================================================================== */

void setup() {
  Serial.begin(MON_SERIAL_BAUD);
  delay(200);
  Serial.println("\n=== live_monitor (bateria + WiFi + camara en vivo) ===");

  /* El firmware principal duerme con PWDN en alto y retenido: se suelta y se
   * deja en alto hasta medir (la primera medida, como en campo, va con la
   * camara apagada y el WiFi sin arrancar). */
  rtc_gpio_hold_dis((gpio_num_t)CAM_PIN_PWDN);
  pinMode(CAM_PIN_PWDN, OUTPUT);
  digitalWrite(CAM_PIN_PWDN, HIGH);
  pinMode(CAM_PIN_FLASH_LED, OUTPUT);
  digitalWrite(CAM_PIN_FLASH_LED, LOW);
  pinMode(STATUS_LED_PIN, OUTPUT);
  led_set(false);

  const esp_reset_reason_t why = esp_reset_reason();
  Serial.printf("Reset reason: %d%s\n", (int)why,
                why == ESP_RST_BROWNOUT ? " (BROWNOUT: la alimentacion cayo)" : "");
  if (why == ESP_RST_BROWNOUT) {
    for (int i = 0; i < 3; i++) { led_set(true); delay(400); led_set(false); delay(300); }
    delay(500);
  }

  const bool from_nvs = load_nvs();
  measure_cycle(true);                 // bateria antes de encender nada, luego la camara

  if (from_nvs) {
    Serial.printf("NVS: WiFi \"%s\" (de la NVS, solo lectura)\n", s_wifi.ssid);
  } else if (!prompt_wifi()) {
    Serial.println("WIFI: no se recibio la red. Reinicia la placa para volver a intentarlo");
    camera_stop();
    while (true) { led_set((millis() / 100) % 2); delay(50); }
  }
  while (!wifi_up()) delay(2000);
  servers_start();
  rssi_reset();

  Serial.printf("WEB: abre http://%s/ desde un movil o PC en la misma red\n",
                WiFi.localIP().toString().c_str());
  Serial.println("Serie: \"r\" reinicia la estadistica de RSSI, \"b\" mide la bateria ahora");
}

void loop() {
  static uint32_t last_sample = 0, last_report = 0, last_retry = 0;
  static uint32_t last_frames = 0, last_bytes = 0, last_fps_ms = 0;
  static bool was_connected = true;
  const uint32_t now = millis();

  led_update();

  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == 'r' || c == 'R') { rssi_reset(); Serial.println("STATS: RSSI reiniciada"); }
    if (c == 'b' || c == 'B') s_bat_now = true;
  }

  if (s_bat_now || (s_bat_interval_s && now - s_bat_ms >= s_bat_interval_s * 1000UL)) {
    s_bat_now = false;
    measure_cycle(false);
    was_connected = WiFi.status() == WL_CONNECTED;
    return;
  }

  /* fps y KB/frame del stream, cada segundo. */
  if (now - last_fps_ms >= 1000) {
    const uint32_t f = s_frames, b = s_frame_bytes;
    const float dt = (now - last_fps_ms) / 1000.0f;
    s_fps      = (f - last_frames) / dt;
    s_kb_frame = f != last_frames ? (b - last_bytes) / 1024.0f / (f - last_frames) : 0;
    last_frames = f; last_bytes = b; last_fps_ms = now;
  }

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (!connected) {
    if (was_connected) {
      s_drops++;
      Serial.println("WIFI: desconectada, reintentando...");
    }
    was_connected = false;
    if (now - last_retry >= MON_CONNECT_TIMEOUT_MS) {
      last_retry = now;
      WiFi.reconnect();
    }
    delay(10);
    return;
  }
  if (!was_connected) {
    Serial.printf("WIFI: reconectada, IP %s\n", WiFi.localIP().toString().c_str());
    was_connected = true;
    if (!s_http) servers_start();      // la medida no pudo reconectar a tiempo
  }

  if (now - last_sample >= MON_RSSI_SAMPLE_MS) {
    last_sample = now;
    const int rssi = (int)WiFi.RSSI();
    if (rssi < 0) {                    // 0 = el driver aun no tiene dato
      s_rssi = rssi;
      if (rssi < s_rssi_min || s_rssi_n == 0) s_rssi_min = rssi;
      if (rssi > s_rssi_max || s_rssi_n == 0) s_rssi_max = rssi;
      s_rssi_sum += rssi;
      s_rssi_n++;
    }
  }

  if (now - last_report >= MON_REPORT_MS) {
    last_report = now;
    Serial.printf("MON: bat %.2f V %d %%%s | RSSI %d dBm (media %.1f, desc %lu) | "
                  "cam %s q%d %.1f fps %.1f KB | heap %lu KB\n",
                  s_bat.bat_mv / 1000.0f, s_bat.pct, s_bat.valid ? "" : " [NO VALIDA]",
                  s_rssi, s_rssi_n ? (double)s_rssi_sum / s_rssi_n : 0.0, (unsigned long)s_drops,
                  s_cam_ok ? framesize_name(s_framesize) : "OFF", s_quality, s_fps, s_kb_frame,
                  (unsigned long)(ESP.getFreeHeap() / 1024));
  }
  delay(2);
}
