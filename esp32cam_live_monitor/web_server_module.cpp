/* ============================================================================
 *  web_server_module.cpp  -  Servidor HTTP del ESP32-CAM
 *
 *  Se usa esp_http_server (el mismo del ejemplo oficial CameraWebServer) en
 *  lugar de WebServer.h porque necesita respuestas por chunks para el MJPEG.
 *  El dashboard se sirve desde dashboard_html.h, compilado en el binario.
 *
 *  Dos servidores:
 *   - HTTP_PORT        : dashboard + endpoints JSON + foto suelta
 *   - HTTP_STREAM_PORT : solo /stream
 *  Un handler MJPEG no termina nunca mientras el cliente mire, y el servidor
 *  atiende las peticiones en un unico hilo: si compartieran puerto, el fetch
 *  de /battery se quedaria esperando a que acabase el video.
 * ==========================================================================*/
#include "web_server_module.h"
#include "config.h"
#include "camera_module.h"
#include "battery_module.h"

#include "dashboard_html.h"

#include <WiFi.h>
#include <esp_http_server.h>

static httpd_handle_t s_server_ctrl   = nullptr;
static httpd_handle_t s_server_stream = nullptr;

static volatile int      s_stream_clients   = 0;
static volatile uint32_t s_last_request_ms  = 0;

/* --- Cabeceras MJPEG ------------------------------------------------------*/
#define PART_BOUNDARY "esp32camframeboundary"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART_HDR =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

/* ---------------------------------------------------------------------------
 * Utilidades
 * -------------------------------------------------------------------------*/
static void mark_request() { s_last_request_ms = millis(); }

static void set_json_headers(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

/* snprintf devuelve la longitud que HABRIA hecho falta: si el buffer se
 * queda corto, enviar ese numero leeria fuera del array. Se recorta siempre. */
static esp_err_t send_json(httpd_req_t *req, const char *buf, int written,
                           size_t capacity) {
  if (written < 0) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON invalido");
    return ESP_FAIL;
  }
  const size_t len = ((size_t)written < capacity) ? (size_t)written
                                                  : (capacity - 1);
  return httpd_resp_send(req, buf, len);
}

/* En modo AP la IP buena es la del softAP y WiFi.RSSI() no significa nada. */
static bool net_is_ap() {
  const wifi_mode_t m = WiFi.getMode();
  return (m == WIFI_AP || m == WIFI_AP_STA);
}

static const char *video_mode_name() {
  switch (VIDEO_MODE) {
    case VIDEO_MODE_STREAM:   return "stream";
    case VIDEO_MODE_SNAPSHOT: return "snapshot";
    default:                  return "hybrid";
  }
}

/* ---------------------------------------------------------------------------
 * GET /   ->  dashboard empotrado en el firmware
 *
 * En el ESP32 la flash esta mapeada en memoria, asi que DASHBOARD_HTML se
 * puede enviar directamente desde su puntero: no se copia a RAM ni hace falta
 * pgm_read_byte. httpd_send ya trocea el envio al escribir en el socket.
 * -------------------------------------------------------------------------*/
static esp_err_t index_handler(httpd_req_t *req) {
  mark_request();
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ---------------------------------------------------------------------------
 * GET /config  ->  parametros que el dashboard necesita
 *
 * Asi index.html no repite ni un solo valor de config.h: los pide al arrancar.
 * -------------------------------------------------------------------------*/
static esp_err_t config_handler(httpd_req_t *req) {
  mark_request();
  set_json_headers(req);

  char json[512];
  const int n = snprintf(
      json, sizeof(json),
      "{"
      "\"device\":\"%s\","
      "\"mode\":\"%s\","
      "\"batteryRefreshMs\":%lu,"
      "\"snapshotRefreshMs\":%lu,"
      "\"streamPort\":%d,"
      "\"routes\":{\"battery\":\"%s\",\"snapshot\":\"%s\",\"stream\":\"%s\","
      "\"status\":\"%s\"},"
      "\"battery\":{\"enabled\":%d,\"lowPercent\":%d,\"criticalPercent\":%d,"
      "\"fullVolts\":%.2f,\"emptyVolts\":%.2f,\"capacityMah\":%d,"
      "\"cells\":%d}"
      "}",
      DEVICE_NAME, video_mode_name(),
      (unsigned long)DASHBOARD_BATTERY_REFRESH_MS,
      (unsigned long)SNAPSHOT_REFRESH_MS,
      (int)HTTP_STREAM_PORT,
      ROUTE_BATTERY, ROUTE_SNAPSHOT, ROUTE_STREAM, ROUTE_STATUS,
      (int)BATTERY_ENABLED, (int)BATTERY_LOW_PERCENT,
      (int)BATTERY_CRITICAL_PERCENT,
      (double)BATTERY_VOLT_FULL, (double)BATTERY_VOLT_EMPTY,
      (int)BATTERY_CAPACITY_MAH, (int)BATTERY_CELLS_SERIES);

  return send_json(req, json, n, sizeof(json));
}

/* ---------------------------------------------------------------------------
 * GET /battery
 * -------------------------------------------------------------------------*/
static esp_err_t battery_handler(httpd_req_t *req) {
  mark_request();
  set_json_headers(req);

  const BatteryReading b = battery_get();

  char json[256];
  const int n = snprintf(
      json, sizeof(json),
      "{\"valid\":%s,\"voltage\":%.3f,\"rawVoltage\":%.3f,\"percent\":%u,"
      "\"state\":\"%s\",\"ageMs\":%lu,\"uptimeS\":%lu}",
      b.valid ? "true" : "false",
      (double)b.voltage, (double)b.raw_voltage, (unsigned)b.percent,
      battery_state_name(b.state), (unsigned long)b.age_ms,
      (unsigned long)(millis() / 1000UL));

  return send_json(req, json, n, sizeof(json));
}

/* ---------------------------------------------------------------------------
 * GET /status  ->  diagnostico
 * -------------------------------------------------------------------------*/
static esp_err_t status_handler(httpd_req_t *req) {
  mark_request();
  set_json_headers(req);

  const bool   ap = net_is_ap();
  const String ip = ap ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  char json[416];
  const int n = snprintf(
      json, sizeof(json),
      "{\"device\":\"%s\",\"uptimeS\":%lu,\"heapFree\":%lu,\"psramFree\":%lu,"
      "\"netMode\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"apClients\":%d,"
      "\"cameraReady\":%s,\"resolution\":\"%s\","
      "\"streamClients\":%d,\"cpuMhz\":%lu}",
      DEVICE_NAME, (unsigned long)(millis() / 1000UL),
      (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram(),
      ap ? "ap" : "sta", ap ? 0 : (int)WiFi.RSSI(), ip.c_str(),
      ap ? (int)WiFi.softAPgetStationNum() : 0,
      camera_ready() ? "true" : "false", camera_resolution_name(),
      s_stream_clients, (unsigned long)getCpuFrequencyMhz());

  return send_json(req, json, n, sizeof(json));
}

/* ---------------------------------------------------------------------------
 * GET /test  ->  prueba directa de la camara, SIN JavaScript
 *
 * El dashboard pide la foto con fetch/Image() desde JS. Si algo falla ahi no
 * se distingue "la camara no va" de "el JS no va". Esta pagina es un <img>
 * pelado: si aqui se ve la imagen, la camara y el servidor funcionan y el
 * problema esta en el navegador o en el dashboard.
 * -------------------------------------------------------------------------*/
static const char *TEST_HTML =
    "<!doctype html><meta charset=utf-8><title>Test camara</title>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<body style=\"font-family:system-ui;background:#111;color:#eee;"
    "margin:0;padding:16px;text-align:center\">"
    "<h2 style=\"font-size:1.1rem\">Prueba directa de la camara</h2>"
    "<p style=\"color:#8b949e;font-size:.85rem\">Esta pagina no usa "
    "JavaScript. Si ves la foto, la camara y el servidor funcionan.</p>"
    "<img src=\"" ROUTE_SNAPSHOT "\" alt=\"Si ves este texto en vez de una "
    "foto, la camara NO esta devolviendo imagen\" "
    "style=\"max-width:100%;border:2px solid #333;border-radius:8px\">"
    "<p style=\"font-size:.85rem\">"
    "<a style=color:#6cf href=\"" ROUTE_TEST "\">Recargar</a> &middot; "
    "<a style=color:#6cf href=\"" ROUTE_STATUS "\">/status</a> &middot; "
    "<a style=color:#6cf href=\"" ROUTE_INDEX "\">dashboard</a></p>";

static esp_err_t test_handler(httpd_req_t *req) {
  mark_request();
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, TEST_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ---------------------------------------------------------------------------
 * GET /capture  ->  una foto JPEG
 * -------------------------------------------------------------------------*/
static esp_err_t capture_handler(httpd_req_t *req) {
  mark_request();

  /* Si hay alguien viendo el stream no se toca el perfil del sensor: cambiar
   * de resolucion en caliente cortaria el video. */
  if (s_stream_clients == 0) camera_use_profile(CAM_PROFILE_SNAPSHOT);

  camera_fb_t *fb = camera_capture();
  if (!fb) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Fallo de captura");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition",
                     "inline; filename=snapshot.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  const esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  camera_release(fb);
  return res;
}

/* ---------------------------------------------------------------------------
 * GET /stream  ->  MJPEG multipart  (puerto HTTP_STREAM_PORT)
 * -------------------------------------------------------------------------*/
#if VIDEO_MODE != VIDEO_MODE_SNAPSHOT
static esp_err_t stream_handler(httpd_req_t *req) {
  mark_request();

  if (s_stream_clients >= STREAM_MAX_CLIENTS) {
    /* El enum httpd_err_code_t de ESP-IDF no incluye el 503, asi que la
     * respuesta se compone a mano con httpd_resp_set_status(). Se devuelve
     * ESP_OK porque la respuesta se ha enviado entera y correctamente: con
     * ESP_FAIL el servidor cerraria el socket sin que llegara el cuerpo. */
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "Stream ocupado: ya hay un cliente viendo el directo",
                    HTTPD_RESP_USE_STRLEN);
    LOG("WEB: stream rechazado, limite de %d cliente/s", STREAM_MAX_CLIENTS);
    return ESP_OK;
  }

  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "60");

  s_stream_clients++;
  camera_use_profile(CAM_PROFILE_STREAM);
  LOG("WEB: stream iniciado (%d cliente/s)", s_stream_clients);

  const uint32_t min_frame_ms =
      (STREAM_MAX_FPS > 0) ? (1000UL / (uint32_t)STREAM_MAX_FPS) : 0UL;
  uint32_t frames = 0;
  const uint32_t started = millis();

  char part_buf[64];

  while (true) {
    const uint32_t frame_start = millis();

    camera_fb_t *fb = camera_capture();
    if (!fb) { res = ESP_FAIL; break; }

    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      const size_t hlen = snprintf(part_buf, sizeof(part_buf),
                                   STREAM_PART_HDR, (unsigned)fb->len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    }
    camera_release(fb);

    /* El navegador cierra el socket al quitar el <img>: aqui es donde se
     * detecta y se sale del bucle. */
    if (res != ESP_OK) break;

    frames++;

    /* Limitador de fps: menos frames = menos JPEG = menos radio encendida. */
    if (min_frame_ms > 0) {
      const uint32_t elapsed = millis() - frame_start;
      if (elapsed < min_frame_ms) delay(min_frame_ms - elapsed);
    } else {
      delay(1);   // cede CPU al scheduler
    }
  }

  s_stream_clients--;
  if (s_stream_clients < 0) s_stream_clients = 0;

  const uint32_t secs = (millis() - started) / 1000UL;
  LOG("WEB: stream cerrado (%lu frames, %lu s)",
      (unsigned long)frames, (unsigned long)secs);

  /* Vuelta al perfil de foto fija para el siguiente /capture. */
  if (s_stream_clients == 0) camera_use_profile(CAM_PROFILE_SNAPSHOT);

  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}
#endif  /* VIDEO_MODE != VIDEO_MODE_SNAPSHOT */

/* ---------------------------------------------------------------------------
 * Registro de rutas y arranque
 * -------------------------------------------------------------------------*/
static const httpd_uri_t uri_index = {
    .uri = ROUTE_INDEX, .method = HTTP_GET, .handler = index_handler,
    .user_ctx = nullptr};
static const httpd_uri_t uri_config = {
    .uri = ROUTE_CONFIG, .method = HTTP_GET, .handler = config_handler,
    .user_ctx = nullptr};
static const httpd_uri_t uri_battery = {
    .uri = ROUTE_BATTERY, .method = HTTP_GET, .handler = battery_handler,
    .user_ctx = nullptr};
static const httpd_uri_t uri_status = {
    .uri = ROUTE_STATUS, .method = HTTP_GET, .handler = status_handler,
    .user_ctx = nullptr};
static const httpd_uri_t uri_test = {
    .uri = ROUTE_TEST, .method = HTTP_GET, .handler = test_handler,
    .user_ctx = nullptr};
static const httpd_uri_t uri_capture = {
    .uri = ROUTE_SNAPSHOT, .method = HTTP_GET, .handler = capture_handler,
    .user_ctx = nullptr};
#if VIDEO_MODE != VIDEO_MODE_SNAPSHOT
static const httpd_uri_t uri_stream = {
    .uri = ROUTE_STREAM, .method = HTTP_GET, .handler = stream_handler,
    .user_ctx = nullptr};
#endif

bool webserver_start() {
  /* --- Servidor de control (puerto 80) --- */
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port      = HTTP_PORT;
  cfg.ctrl_port        = 32768;
  cfg.max_open_sockets = HTTP_MAX_CLIENTS;
  cfg.stack_size       = HTTP_STACK_SIZE;
  cfg.lru_purge_enable = true;

  if (httpd_start(&s_server_ctrl, &cfg) != ESP_OK) {
    LOG("WEB: no se pudo arrancar el servidor en el puerto %d", HTTP_PORT);
    s_server_ctrl = nullptr;
    return false;
  }

  httpd_register_uri_handler(s_server_ctrl, &uri_index);
  httpd_register_uri_handler(s_server_ctrl, &uri_config);
  httpd_register_uri_handler(s_server_ctrl, &uri_battery);
  httpd_register_uri_handler(s_server_ctrl, &uri_status);
  httpd_register_uri_handler(s_server_ctrl, &uri_capture);
  httpd_register_uri_handler(s_server_ctrl, &uri_test);
  {
    const String ip = net_is_ap() ? WiFi.softAPIP().toString()
                                  : WiFi.localIP().toString();
    LOG("WEB: control en http://%s:%d/", ip.c_str(), HTTP_PORT);
  }

#if VIDEO_MODE != VIDEO_MODE_SNAPSHOT
  /* --- Servidor de streaming (puerto 81) ---
   * ctrl_port distinto: es el socket UDP interno de control de httpd y no
   * puede repetirse entre instancias. */
  httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
  scfg.server_port      = HTTP_STREAM_PORT;
  scfg.ctrl_port        = 32769;
  scfg.max_open_sockets = STREAM_MAX_CLIENTS + 1;
  scfg.stack_size       = HTTP_STACK_SIZE;
  scfg.lru_purge_enable = true;

  if (httpd_start(&s_server_stream, &scfg) == ESP_OK) {
    httpd_register_uri_handler(s_server_stream, &uri_stream);
    const String sip = net_is_ap() ? WiFi.softAPIP().toString()
                                   : WiFi.localIP().toString();
    LOG("WEB: stream en http://%s:%d%s", sip.c_str(), HTTP_STREAM_PORT,
        ROUTE_STREAM);
  } else {
    LOG("WEB: no se pudo arrancar el servidor de stream (%d)",
        HTTP_STREAM_PORT);
    s_server_stream = nullptr;
  }
#else
  LOG("WEB: modo snapshot, /stream desactivado");
#endif

  return true;
}

void webserver_stop() {
  if (s_server_stream) { httpd_stop(s_server_stream); s_server_stream = nullptr; }
  if (s_server_ctrl)   { httpd_stop(s_server_ctrl);   s_server_ctrl   = nullptr; }
  s_stream_clients = 0;
}

bool webserver_stream_active() { return s_stream_clients > 0; }
int  webserver_stream_clients() { return s_stream_clients; }
uint32_t webserver_last_request_ms() { return s_last_request_ms; }
