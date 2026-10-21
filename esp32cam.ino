/* ============================================================================
 *  esp32cam.ino  -  ESP32-CAM cliente REST con deep sleep
 *  Proyecto de tesis UPC "ParkVision"
 *
 *  Cada despertar del deep sleep ejecuta un ciclo completo dentro de setup():
 *    0. Leer la configuracion de la placa de la NVS (WiFi, credencial, Fog).
 *       Si falta la WiFi o la credencial, modo aprovisionamiento por USB y
 *       nada mas (ver abajo). El binario no lleva nada propio de una placa:
 *       el mismo .bin sirve para las tres camaras.
 *    1. Medir la bateria (GPIO13 es ADC2: antes del WiFi). Si esta por
 *       debajo del corte, dormir sin encender camara ni WiFi.
 *    2. Inicializar la camara.
 *    3. Conectar WiFi en modo estacion (con timeout: sin red no hay nada que
 *       subir, no vale la pena reintentar indefinidamente y gastar bateria).
 *    4. Capturar 1 frame JPEG y subirlo por POST multipart a la Raspberry Pi
 *       (fog node), que procesa las plazas de parking y reenvia el resultado
 *       a la nube. El ESP32 no sabe nada de eso: solo manda el JPEG (y la
 *       telemetria del ciclo en el mismo POST).
 *    5. Si la subida fue bien: poner el reloj en hora con la cabecera Date
 *       y subir hasta BACKLOG_MAX_SEND_PER_CYCLE fotos atrasadas.
 *       Si no (sin WiFi, Fog caido, 5xx): guardar la foto en la flash
 *       interna para subirla en un ciclo con conexion (src/backlog/).
 *    6. Cada OTA_CHECK_EVERY_CYCLES ciclos con subida correcta, pedir la
 *       config al Fog y, si ofrece una version nueva con firma valida,
 *       instalarla y reiniciar (src/ota/).
 *    7. Apagar camara (PWDN + hold) y WiFi, y dormir CAPTURE_INTERVAL_MS.
 *
 *  Sin WiFi la foto se captura DESPUES del intento de conexion, igual que en
 *  un ciclo normal: esos segundos son los que dan tiempo al AEC/AGC del
 *  OV2640 a ajustar la exposicion. Capturar justo tras camera_init() daria
 *  un primer frame mal expuesto.
 *
 *  Junto a la foto viaja la telemetria del ciclo (UploadTelemetry). Si la
 *  imagen en ejecucion viene de un OTA y aun no esta confirmada, solo se
 *  confirma tras una subida 2xx (rollback, ver ota_module.cpp).
 *
 *  Aprovisionamiento (src/provision/, scripts/provision.ps1):
 *    - NVS sin WiFi o sin credencial: se espera la linea JSON por serie
 *      PROV_WAIT_MS; si no llega, deep sleep PROV_RETRY_SLEEP_MS (sin USB no
 *      hay quien la mande).
 *    - Ya aprovisionada y arranque en frio (encendido / RST / pulso RTS del
 *      script): ventana corta PROV_WINDOW_MS para reescribir o borrar.
 *    - Despertar de deep sleep: ninguna espera. Los ciclos normales no pagan.
 *
 *  No hay loop() real: no queda nada despues de esp_deep_sleep_start().
 *
 *  Placa en el IDE: "AI Thinker ESP32-CAM"
 * ==========================================================================*/
#include <Arduino.h>

#include <esp_sleep.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>

#include "config.h"
#include "src/camera/camera_module.h"
#include "src/wifi/wifi_module.h"
#include "src/upload/upload_module.h"
#include "src/ota/ota_module.h"
#include "src/settings/settings_module.h"
#include "src/provision/provision_module.h"
#include "src/battery/battery_module.h"
#include "src/clock/clock_module.h"
#include "src/backlog/backlog_module.h"
#include "src/fogconfig/fogconfig_module.h"

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

/* true desde que un ciclo se corto por bateria baja hasta que la bateria
 * supera BATTERY_RESUME_MV: es la memoria de la histeresis del corte. */
RTC_DATA_ATTR static bool s_battery_low = false;

/* Ciclos con subida correcta que faltan para volver a pedir la config al
 * Fog (oferta de firmware). 0 = en el proximo: tras un arranque en frio se
 * pregunta enseguida. */
RTC_DATA_ATTR static uint32_t s_config_countdown = 0;

/* Descargas de firmware fallidas seguidas: marca la espera hasta reintentar
 * (1, 2, 4, 8 ciclos y despues la normal). Un corte puntual se recupera en
 * 5 min; un Fog roto de verdad no cuesta una descarga en cada ciclo. */
RTC_DATA_ATTR static uint32_t s_ota_fail_streak = 0;

/* Que build escribio la memoria RTC. Cada compilacion coloca las variables
 * RTC_DATA_ATTR en otras direcciones, y tras un OTA o un rollback el
 * arranque llega como "despertar de deep sleep": sin esta marca, la imagen
 * nueva leeria como contadores, hora o fotos pendientes lo que dejo la
 * otra. Si no coincide, la memoria RTC se trata como tras un arranque en frio. */
RTC_DATA_ATTR static uint32_t s_rtc_owner = 0;

/* Primeros 4 bytes del SHA-256 del ELF de esta imagen: distinto en cada build. */
static uint32_t build_id() {
  const uint8_t *h = esp_app_get_description()->app_elf_sha256;
  return ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
}

/* ===========================================================================
 * Apagado y deep sleep
 * ==========================================================================*/
static void go_to_sleep(uint32_t sleep_ms = CAPTURE_INTERVAL_MS) {
  camera_power_down();   // PWDN + rtc_gpio_hold_en: retiene el pin dormido
  camera_flash_hold_off();   // IO4 en LOW: el pull-up R11 encenderia el flash a medias
  wifi_off();

#if STATUS_LED_ENABLED
  digitalWrite(STATUS_LED_PIN, STATUS_LED_ACTIVE_LOW ? HIGH : LOW);   // apagado
#endif

  LOG("SLEEP: durmiendo %lu ms", (unsigned long)sleep_ms);
  Serial.flush();

  esp_sleep_enable_timer_wakeup((uint64_t)sleep_ms * 1000ULL);
  esp_deep_sleep_start();
}

/* ===========================================================================
 * Fotos atrasadas
 * ==========================================================================*/
/* Lo que se guarda junto a una foto que no se pudo subir: la telemetria del
 * momento de la captura (la del ciclo de subida no le corresponde). */
static BacklogMeta capture_meta(esp_reset_reason_t reason, const BatteryReading &bat) {
  BacklogMeta m = {};
  m.fw_version    = FW_VERSION;
  m.reset_reason  = (int16_t)reason;
  m.jpeg_quality  = (uint8_t)camera_jpeg_quality();
  m.battery_valid = bat.valid;
  m.battery_mv    = bat.mv;
  strlcpy(m.framesize, camera_framesize_name(), sizeof(m.framesize));
  return m;
}

/* Guarda la foto que no se pudo subir. Primero se apagan sensor y radio: la
 * escritura en flash no debe coincidir con el pico de corriente del WiFi
 * (brownout a media escritura) ni con el DMA de la camara llenando PSRAM. */
static void store_frame(camera_fb_t *fb, uint32_t captured_at, const BacklogMeta &meta) {
  if (!BACKLOG_ENABLED) return;
  if (captured_at == 0) {
    LOG("BACKLOG: sin hora de referencia (ninguna subida desde el arranque en frio): "
        "la foto no se guarda");
    return;
  }
  camera_power_down();
  wifi_off();
  backlog_save(captured_at, fb->buf, fb->len, meta);
}

/* Tras una subida 2xx: hora del Fog y, si hay fotos pendientes, subirlas.
 * Va despues de la foto actual a proposito: asi la respuesta trae la hora
 * para corregir las guardadas antes de enviarlas, y si el Fog cae a mitad,
 * lo que se queda para otro ciclo son fotos viejas, no la actual. */
static void send_backlog(const SettingsFog &fog, const SettingsCredential &cred,
                         const char *date) {
  uint32_t  server_now = 0;
  ClockSync sync       = {};
  const bool synced = clock_parse_http_date(date, &server_now) && clock_sync(server_now, &sync);
  if (!synced) LOG("CLOCK: la respuesta no trae una cabecera Date valida (\"%s\")", date);

  if (!BACKLOG_ENABLED || backlog_pending() == 0) return;
  if (!clock_valid()) {
    LOG("BACKLOG: %lu pendientes, pero sin hora no se pueden fechar: se esperan",
        (unsigned long)backlog_pending());
    return;
  }

  /* La camara ya no hace falta: ~40-60 mA menos durante las subidas extra. */
  camera_power_down();
  if (synced) backlog_retime(sync);

  for (int i = 0; i < BACKLOG_MAX_SEND_PER_CYCLE; i++) {
    BacklogItem item;
    if (!backlog_load_oldest(item)) break;

    char iso[24];
    clock_format_iso(item.captured_at, iso, sizeof(iso));
    UploadTelemetry t = {};
    t.fw_version    = item.meta.fw_version;
    t.reset_reason  = item.meta.reset_reason;
    t.framesize     = item.meta.framesize;
    t.jpeg_quality  = item.meta.jpeg_quality;
    t.battery_valid = item.meta.battery_valid;
    t.battery_mv    = item.meta.battery_mv;
    t.captured_at   = iso;

    const int code = upload_frame(fog.host, fog.port, cred.api_key, cred.api_token,
                                  item.jpeg, item.jpeg_len, &t);
    if (code >= 200 && code < 500) {
      /* 2xx: subida. 4xx: el Fog no la quiere (hora fuera de rango,
       * corrupta...) y no la va a querer nunca: reintentarla en cada ciclo
       * gastaria bateria para nada. */
      LOG("BACKLOG: %s %s (%d)", item.path, code < 300 ? "subida" : "rechazada, se borra", code);
      backlog_remove(item);
    } else {
      /* Error de red o 5xx: el Fog volvera. Se para aqui: si ha caido, las
       * siguientes fallarian igual y cada una cuesta un timeout. */
      LOG("BACKLOG: %s no se pudo subir (%d), se reintentara", item.path, code);
      backlog_release(item);
      break;
    }
  }
  LOG("BACKLOG: quedan %lu pendientes", (unsigned long)backlog_pending());
}

/* Tras una subida 2xx, cada OTA_CHECK_EVERY_CYCLES ciclos: config del Fog y,
 * si trae una version nueva, ota_handle_offer() (que reinicia si instala).
 * Va despues de las fotos atrasadas: un OTA reinicia y las dejaria sin subir. */
static void check_firmware(const SettingsFog &fog, const SettingsCredential &cred,
                           const BatteryReading &bat) {
  if (!OTA_ENABLED) return;
  if (s_config_countdown > 0) {
    s_config_countdown--;
    return;
  }
  s_config_countdown = OTA_CHECK_EVERY_CYCLES - 1;

  FogConfig cfg;
  if (!fogconfig_fetch(fog.host, fog.port, cred.api_key, cred.api_token, cfg)) return;
  if (!cfg.has_firmware) {
    LOG("OTA: el Fog no tiene firmware asignado a esta camara");
    return;
  }
  /* Descargar 1,2 MB con el sensor encendido gastaria ~40-60 mA mas y
   * escribiria la flash con su DMA activo. */
  camera_power_down();
  const bool battery_ok = !bat.valid || bat.mv >= OTA_MIN_BATTERY_MV;
  const OtaResult r =
      ota_handle_offer(fog.host, fog.port, cred.api_key, cred.api_token, cfg.firmware, battery_ok);
  if (r == OTA_RETRY) {
    s_ota_fail_streak++;
    const uint32_t shift = s_ota_fail_streak - 1 < 3 ? s_ota_fail_streak - 1 : 3;
    const uint32_t wait  = min((uint32_t)1 << shift, (uint32_t)OTA_CHECK_EVERY_CYCLES);
    s_config_countdown = wait - 1;
    LOG("OTA: fallo %lu seguido, se reintenta en %lu ciclos", (unsigned long)s_ota_fail_streak,
        (unsigned long)wait);
  } else {
    s_ota_fail_streak = 0;
  }
}

/* ===========================================================================
 * setup  -  un ciclo completo por cada despertar
 * ==========================================================================*/
void setup() {
  /* Antes de begin(): el buffer RX solo se puede fijar ahi. La linea de
   * aprovisionamiento puede pasar de los 256 B por defecto si va escapada. */
  Serial.setRxBufferSize(PROV_SERIAL_RX_BUFFER);
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println();
  LOG("=== %s ===", DEVICE_NAME);   // el id de camara sale con la NVS, mas abajo
  ota_log_boot_info();   // FW_VERSION + particion activa

  const esp_reset_reason_t reset_reason = esp_reset_reason();
  /* La memoria RTC no siempre se borra en un reset que no sea deep sleep
   * (depende de la causa), asi que se pone a 0 a mano: "desde el ultimo
   * arranque en frio" queda definido por reset_reason y no por el hardware.
   * rtc_reason es la causa a efectos de la memoria RTC: un despertar cuya
   * memoria escribio otro build (OTA, rollback) cuenta como arranque en frio.
   * Al Fog viaja siempre reset_reason, la real. */
  const bool rtc_ours = s_rtc_owner == build_id();
  const esp_reset_reason_t rtc_reason =
      (reset_reason == ESP_RST_DEEPSLEEP && rtc_ours) ? ESP_RST_DEEPSLEEP : ESP_RST_UNKNOWN;
  if (reset_reason == ESP_RST_DEEPSLEEP && !rtc_ours) {
    LOG("POWER: la memoria RTC es de otro build (OTA o rollback): se reinicia");
  }
  s_rtc_owner = build_id();
  if (rtc_reason == ESP_RST_DEEPSLEEP) {
    s_wake_count++;
  } else {
    s_wake_count       = 0;
    s_wifi_fail_streak = 0;
    s_battery_low      = false;
    s_config_countdown = 0;
    s_ota_fail_streak  = 0;
  }
  LOG("Reset reason: %d  wake_count=%lu  wifi_fail_streak=%lu",
      (int)reset_reason, (unsigned long)s_wake_count,
      (unsigned long)s_wifi_fail_streak);
  clock_begin(rtc_reason);
  backlog_begin(rtc_reason);

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

  /* --- Configuracion de la placa (NVS) y aprovisionamiento -------------------
   * Antes que la camara: si no hay WiFi no se va a subir nada, y arrancar el
   * sensor solo gastaria. */
  settings_begin();
  if (!settings_is_provisioned()) {
    LOG("PROV: la NVS no tiene WiFi o credencial: modo aprovisionamiento");
    provision_listen(PROV_WAIT_MS);   // con una linea valida reinicia
    LOG("PROV: no llego configuracion en %lu ms", (unsigned long)PROV_WAIT_MS);
    go_to_sleep(PROV_RETRY_SLEEP_MS);
  }
  /* Solo ESP_RST_POWERON (encendido, boton RST o pulso RTS del script): es
   * el unico arranque en el que puede haber alguien al otro lado del USB.
   * Ni el despertar del deep sleep ni un brownout/cuelgue en campo abren la
   * ventana, asi que no alarga ningun ciclo normal. Tampoco ESP_RST_SW, que
   * es el reinicio que sigue a un aprovisionamiento correcto. */
  if (reset_reason == ESP_RST_POWERON) {
    provision_listen(PROV_WINDOW_MS);
    /* Solo en frio: no gasta nada en los ciclos normales y basta para saber,
     * tras cada encendido, que la verificacion de firmas funciona. */
    if (OTA_ENABLED) ota_self_test();
  }

  SettingsWifi wifi;
  settings_get_wifi(wifi);
  SettingsCredential cred;
  settings_get_credential(cred);
  SettingsFog fog;
  const bool fog_from_nvs = settings_get_fog(fog);
  LOG("NVS: camera=%s  wifi=\"%s\"  fog=%s:%u (%s)", cred.camera_code, wifi.ssid,
      fog.host, (unsigned)fog.port, fog_from_nvs ? "NVS" : "defecto de config.h");

  /* --- Bateria ------------------------------------------------------------------
   * Antes que la camara (lectura en reposo, sin los ~100 mA del sensor) y,
   * sobre todo, antes que el WiFi: GPIO13 es ADC2. Despues del
   * aprovisionamiento a proposito: una placa por USB sin celda (o con una
   * celda casi vacia) podria leer por debajo del corte, y si el corte fuera
   * antes no se podria volver a aprovisionar. */
  SettingsBatteryCal bat_cal;
  const bool bat_cal_from_nvs = settings_get_battery_cal(bat_cal);
  BatteryReading bat;
  battery_read(bat_cal, bat);
  LOG("BAT: ADC IO13: %lu mV | Bateria: %.2f V | %d %% %s(cal. %s)",
      (unsigned long)bat.adc_mv, bat.mv / 1000.0f,
      bat.percent, bat.valid ? "" : "NO VALIDA ", bat_cal_from_nvs ? "NVS" : "defecto");

  if (bat.valid) {
    const int32_t threshold = s_battery_low ? BATTERY_RESUME_MV : BATTERY_CUTOFF_MV;
    if (bat.mv < threshold) {
      s_battery_low = true;
      LOG("BAT: %ld mV < %ld mV: bateria baja, se duerme sin camara ni WiFi",
          (long)bat.mv, (long)threshold);
      go_to_sleep();
    }
    s_battery_low = false;
  }

  /* --- Camara --------------------------------------------------------------*/
  if (!camera_init()) {
    LOG("FATAL: la camara no arranca. Se aborta el ciclo y se duerme igual.");
    go_to_sleep();
  }

  /* --- WiFi ------------------------------------------------------------------*/
  const bool connected = wifi_connect(wifi.ssid, wifi.password);
  memset(wifi.password, 0, sizeof(wifi.password));   // ya no hace falta en RAM
  if (!connected) {
    s_wifi_fail_streak++;
    /* Sin red no se sube, pero la foto se puede guardar para otro ciclo. Sin
     * hora ni se captura: no se podria fechar y el sensor gastaria de mas. */
    if (BACKLOG_ENABLED && clock_valid()) {
      camera_fb_t *fb = camera_capture();
      if (fb) {
        store_frame(fb, clock_now(), capture_meta(reset_reason, bat));
        camera_release(fb);
      }
    } else if (BACKLOG_ENABLED) {
      LOG("BACKLOG: sin hora de referencia: la foto de este ciclo se pierde");
    }
    go_to_sleep();
  }
  const uint32_t wifi_fail_streak = s_wifi_fail_streak;   // el de antes de conectar
  s_wifi_fail_streak = 0;

  /* --- Captura y subida --------------------------------------------------------*/
  camera_fb_t *fb = camera_capture();
  if (!fb) {
    LOG("FATAL: no se pudo capturar el frame.");
  } else {
    /* Hora de la captura, por si hay que guardarla: antes del POST, que
     * puede tardar hasta WIFI_CONNECT_TIMEOUT_MS en fallar. */
    const uint32_t captured_at = clock_now();
    UploadTelemetry t = {};
    t.fw_version       = FW_VERSION;
    t.rssi             = wifi_rssi();
    t.wifi_connect_ms  = wifi_connect_ms();
    t.reset_reason     = (int)reset_reason;
    t.free_heap        = ESP.getFreeHeap();
    t.wake_count       = s_wake_count;
    t.wifi_fail_streak = wifi_fail_streak;
    t.framesize        = camera_framesize_name();
    t.jpeg_quality     = camera_jpeg_quality();
    t.battery_valid    = bat.valid;
    t.battery_mv       = bat.mv;
    /* millis() vuelve a 0 en cada despertar (el deep sleep es un reset), asi
     * que es el tiempo despierto de este ciclo hasta el POST. No incluye la
     * ROM ni el bootloader (unos cientos de ms) ni el propio POST: es una
     * cota inferior, comparable entre ciclos, no el consumo exacto. */
    t.awake_ms         = millis();

    /* La foto ya esta en PSRAM: el sensor no hace falta durante el POST
     * (~40-60 mA menos). store_frame() y send_backlog() lo encuentran ya
     * apagado; mientras la camara siga lista, nada lo vuelve a encender. */
    camera_power_down();

    char date[40];
    const int code = upload_frame(fog.host, fog.port, cred.api_key, cred.api_token,
                                  fb->buf, fb->len, &t, date, sizeof(date));
    const bool uploaded = code >= 200 && code < 300;
    if (uploaded) {
      LOG("UPLOAD: ok (%d)", code);
      /* Unico punto donde se da por buena una imagen nueva: WiFi conectado y
       * el Fog ha aceptado la foto. Si una imagen recien instalada por OTA
       * (PENDING_VERIFY) llega al deep sleep sin pasar por aqui, el
       * bootloader la descarta al despertar y vuelve a la anterior (ver
       * ota_module.cpp). Por eso NO se valida al arrancar ni al conectar:
       * una version que conecta pero no sabe subir tambien debe caer. */
      ota_confirm_if_pending();
    } else if (code > 0 && code < 500) {
      /* 4xx: credencial mala, foto rechazada... Reintentarla mas tarde daria
       * lo mismo: no se guarda. */
      LOG("UPLOAD: fallo (%d), el Fog rechaza la foto: no se guarda", code);
    } else {
      LOG("UPLOAD: fallo (%d)", code);
      store_frame(fb, captured_at, capture_meta(reset_reason, bat));
    }
    camera_release(fb);
    if (uploaded) {
      send_backlog(fog, cred, date);
      check_firmware(fog, cred, bat);
    }
  }

  go_to_sleep();
}

/* Nunca se ejecuta: go_to_sleep() termina siempre en esp_deep_sleep_start(),
 * que no retorna. El framework Arduino exige declarar loop() de todos modos. */
void loop() {}
