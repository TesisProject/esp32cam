/* ============================================================================
 *  camera_module.cpp  -  Sensor OV2640 de la ESP32-CAM AI-Thinker
 * ==========================================================================*/
#include "camera_module.h"
#include "../../config.h"

#include "driver/rtc_io.h"

static bool          s_ready         = false;
static bool          s_powered       = false;
static uint32_t      s_last_use_ms   = 0;

/* Resolucion y calidad aplicadas de verdad (las de config.h o las degradadas
 * si no hay PSRAM). Se guardan aqui porque son las que hay que reaplicar al
 * volver de PWDN y las que se reportan al Fog como telemetria. */
static framesize_t   s_framesize     = CAM_FRAMESIZE_SNAPSHOT;
static int           s_jpeg_quality  = CAM_JPEG_QUALITY_SNAPSHOT;

/* ---------------------------------------------------------------------------
 * Ajustes del sensor que se aplican siempre, al iniciar y al volver de PWDN.
 * -------------------------------------------------------------------------*/
static void apply_sensor_defaults() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;

  s->set_vflip(s, CAM_VFLIP);
  s->set_hmirror(s, CAM_HMIRROR);
  s->set_brightness(s, CAM_BRIGHTNESS);
  s->set_contrast(s, CAM_CONTRAST);
  s->set_saturation(s, CAM_SATURATION);

  /* El OV2640 de muchos modulos AI-Thinker sale saturado de luz en el primer
   * frame; dejar el AEC/AGC en automatico lo corrige solo. */
  s->set_gain_ctrl(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_whitebal(s, 1);
}

/* ---------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------*/
bool camera_init() {
#if CAM_PIN_PWDN >= 0
  /* Si el ciclo anterior termino en deep sleep, el pin quedo retenido
   * (rtc_gpio_hold_en en camera_power_down): hay que soltarlo antes de
   * reconfigurarlo, o pinMode/digitalWrite no tendrian efecto. */
  rtc_gpio_hold_dis((gpio_num_t)CAM_PIN_PWDN);
#endif

  camera_config_t cfg = {};

  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;

  cfg.pin_d0       = CAM_PIN_D0;
  cfg.pin_d1       = CAM_PIN_D1;
  cfg.pin_d2       = CAM_PIN_D2;
  cfg.pin_d3       = CAM_PIN_D3;
  cfg.pin_d4       = CAM_PIN_D4;
  cfg.pin_d5       = CAM_PIN_D5;
  cfg.pin_d6       = CAM_PIN_D6;
  cfg.pin_d7       = CAM_PIN_D7;
  cfg.pin_xclk     = CAM_PIN_XCLK;
  cfg.pin_pclk     = CAM_PIN_PCLK;
  cfg.pin_vsync    = CAM_PIN_VSYNC;
  cfg.pin_href     = CAM_PIN_HREF;
  cfg.pin_sccb_sda = CAM_PIN_SIOD;
  cfg.pin_sccb_scl = CAM_PIN_SIOC;
  cfg.pin_pwdn     = CAM_PIN_PWDN;
  cfg.pin_reset    = CAM_PIN_RESET;

  cfg.xclk_freq_hz = CAM_XCLK_FREQ_HZ;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.grab_mode    = CAMERA_GRAB_LATEST;   // siempre el frame mas reciente

  /* Se inicializa con el perfil mas grande (snapshot) para que el driver
   * reserve buffers suficientes; bajar de resolucion despues es gratis. */
  if (psramFound()) {
    s_framesize      = CAM_FRAMESIZE_SNAPSHOT;
    s_jpeg_quality   = CAM_JPEG_QUALITY_SNAPSHOT;
    cfg.fb_count     = CAM_FB_COUNT;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
  } else {
    /* Sin PSRAM no hay sitio para SVGA: se degrada en vez de fallar. */
    s_framesize      = CAM_FRAMESIZE_NO_PSRAM;
    s_jpeg_quality   = CAM_JPEG_QUALITY_NO_PSRAM;
    cfg.fb_count     = 1;
    cfg.fb_location  = CAMERA_FB_IN_DRAM;
    LOG("CAM: aviso, PSRAM no detectada -> se degrada a %s",
        camera_framesize_name());
  }
  cfg.frame_size   = s_framesize;
  cfg.jpeg_quality = s_jpeg_quality;

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    LOG("CAM: esp_camera_init fallo (0x%x)", err);
    s_ready   = false;
    s_powered = false;
    return false;
  }

  s_ready   = true;
  s_powered = true;
  apply_sensor_defaults();

  if (CAM_FLASH_ENABLED) {
    pinMode(CAM_PIN_FLASH_LED, OUTPUT);
    digitalWrite(CAM_PIN_FLASH_LED, LOW);
  }

  LOG("CAM: lista (%s, PSRAM=%s)", camera_resolution_name(),
      psramFound() ? "si" : "no");
  return true;
}

bool camera_ready() { return s_ready; }

/* ---------------------------------------------------------------------------
 * Captura
 * -------------------------------------------------------------------------*/
camera_fb_t *camera_capture() {
  if (!s_ready) return nullptr;
  if (!s_powered) camera_power_up();

  camera_fb_t *fb = esp_camera_fb_get();
  s_last_use_ms = millis();

  if (!fb) {
    LOG("CAM: fallo al capturar frame");
    return nullptr;
  }
  if (fb->format != PIXFORMAT_JPEG) {
    /* No deberia ocurrir: el driver esta en PIXFORMAT_JPEG. */
    LOG("CAM: formato inesperado (%d)", (int)fb->format);
    esp_camera_fb_return(fb);
    return nullptr;
  }
  return fb;
}

void camera_release(camera_fb_t *fb) {
  if (fb) esp_camera_fb_return(fb);
}

/* ---------------------------------------------------------------------------
 * Flash
 * -------------------------------------------------------------------------*/
void camera_flash(bool on) {
#if CAM_FLASH_ENABLED
  digitalWrite(CAM_PIN_FLASH_LED, on ? HIGH : LOW);
#else
  (void)on;
#endif
}

/* ---------------------------------------------------------------------------
 * Ahorro de energia del sensor
 *
 *  PWDN alto apaga el bloque analogico del OV2640 (~40-60 mA). El driver de
 *  Espressif no expone esto, asi que se maneja el GPIO directamente. Al
 *  despertar hay que darle tiempo al sensor a re-enganchar el reloj.
 * -------------------------------------------------------------------------*/
void camera_power_down() {
#if CAM_PIN_PWDN >= 0
  /* Sin camera_init() (placa sin aprovisionar, o init fallido) el sensor
   * sigue alimentado y PWDN queda como entrada sin nivel definido: tambien
   * hay que apagarlo, o una placa que reintenta aprovisionar cada hora en
   * campo dormiria con la camara encendida. Solo se salta si ya esta apagado. */
  if (s_ready && !s_powered) return;
  pinMode(CAM_PIN_PWDN, OUTPUT);
  digitalWrite(CAM_PIN_PWDN, HIGH);

  /* El deep sleep del ESP32 apaga la CPU pero NO corta la alimentacion de la
   * camara (comparte el rail de 3.3V) ni conserva el nivel de los pines por
   * defecto. GPIO32 es RTC-capable: rtc_gpio_hold_en() fija el nivel HIGH
   * (sensor apagado) durante todo el deep sleep, hasta que se suelte en el
   * siguiente camera_init(). */
  rtc_gpio_hold_en((gpio_num_t)CAM_PIN_PWDN);

  s_powered = false;
  LOG("CAM: sensor apagado (PWDN) y retenido para deep sleep");
#endif
}

void camera_power_up() {
#if CAM_PIN_PWDN >= 0
  if (!s_ready || s_powered) return;
  rtc_gpio_hold_dis((gpio_num_t)CAM_PIN_PWDN);
  pinMode(CAM_PIN_PWDN, OUTPUT);
  digitalWrite(CAM_PIN_PWDN, LOW);
  s_powered = true;
  delay(120);                 // estabilizacion del sensor

  /* Al volver de PWDN el OV2640 pierde su configuracion de registros. */
  apply_sensor_defaults();
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    /* Las aplicadas en camera_init, no las de config.h: sin PSRAM, volver
     * a SVGA aqui pediria un frame que no cabe en la RAM interna. */
    s->set_framesize(s, s_framesize);
    s->set_quality(s, s_jpeg_quality);
  }
  /* Descarta el primer frame, que sale con la exposicion sin converger. */
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);
#endif
}

bool camera_is_powered() { return s_powered; }

uint32_t camera_last_use_ms() { return s_last_use_ms; }

/* ---------------------------------------------------------------------------
 * Nombre de la resolucion activa
 * -------------------------------------------------------------------------*/
const char *camera_resolution_name() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return "n/d";

  switch (s->status.framesize) {
    case FRAMESIZE_QQVGA: return "160x120";
    case FRAMESIZE_QVGA:  return "320x240";
    case FRAMESIZE_CIF:   return "400x296";
    case FRAMESIZE_VGA:   return "640x480";
    case FRAMESIZE_SVGA:  return "800x600";
    case FRAMESIZE_XGA:   return "1024x768";
    case FRAMESIZE_SXGA:  return "1280x1024";
    case FRAMESIZE_UXGA:  return "1600x1200";
    default:              return "?";
  }
}

/* ---------------------------------------------------------------------------
 * Resolucion y calidad aplicadas, para la telemetria
 *
 *  El nombre del enum (SVGA, QVGA...) y no "800x600": es lo que espera el
 *  campo framesize del Fog y lo que se escribe en config.h, asi que se
 *  compara de un vistazo con CAM_FRAMESIZE_SNAPSHOT.
 * -------------------------------------------------------------------------*/
const char *camera_framesize_name() {
  switch (s_framesize) {
    case FRAMESIZE_96X96:   return "96X96";
    case FRAMESIZE_QQVGA:   return "QQVGA";
    case FRAMESIZE_128X128: return "128X128";
    case FRAMESIZE_QCIF:    return "QCIF";
    case FRAMESIZE_HQVGA:   return "HQVGA";
    case FRAMESIZE_240X240: return "240X240";
    case FRAMESIZE_QVGA:    return "QVGA";
    case FRAMESIZE_320X320: return "320X320";
    case FRAMESIZE_CIF:     return "CIF";
    case FRAMESIZE_HVGA:    return "HVGA";
    case FRAMESIZE_VGA:     return "VGA";
    case FRAMESIZE_SVGA:    return "SVGA";
    case FRAMESIZE_XGA:     return "XGA";
    case FRAMESIZE_HD:      return "HD";
    case FRAMESIZE_SXGA:    return "SXGA";
    case FRAMESIZE_UXGA:    return "UXGA";
    default:                return "OTRO";   // el OV2640 no pasa de UXGA
  }
}

int camera_jpeg_quality() { return s_jpeg_quality; }
