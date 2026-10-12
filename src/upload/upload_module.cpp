/* ============================================================================
 *  upload_module.cpp  -  POST multipart/form-data a la Raspberry Pi
 *
 *  HTTPClient (core ESP32) no trae helper de multipart, asi que el cuerpo se
 *  arma a mano: prefijo (partes de texto de la telemetria + cabecera de la
 *  parte "file") + bytes JPEG + cierre, en un unico buffer (PSRAM si la hay,
 *  RAM interna si no).
 *  Con el Content-Length exacto ya calculado, http.POST(buffer, len) manda
 *  todo de una vez y evita la complejidad de un body en streaming/chunked.
 * ==========================================================================*/
#include "upload_module.h"
#include "../../config.h"   // incluye tambien secrets.h

#include <HTTPClient.h>
#include <esp_heap_caps.h>

static const char *kBoundary = "ESP32CAMBoundary7MA4YWxkTrZu0gW";

/* ---------------------------------------------------------------------------
 * Reserva del body: PSRAM primero, RAM interna despues.
 *
 *  Con PSRAM, un SVGA son ~40 KB y no conviene quitarselos a la RAM interna
 *  (la usan la pila WiFi/lwIP y el propio HTTPClient). Sin PSRAM la camara ya
 *  se ha degradado a QVGA (ver camera_module.cpp): el body son ~4-5 KB y la
 *  RAM interna tiene ~260 KB libres, asi que abortar ahi perdia el ciclo
 *  entero por nada. Si fallan las dos, se loguea el tamano pedido y el mayor
 *  bloque interno libre: la fragmentacion, no el total libre, es lo que
 *  decide si cabe.
 * -------------------------------------------------------------------------*/
static uint8_t *alloc_body(size_t len) {
  uint8_t *p = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;

  p = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (p) {
    LOG("UPLOAD: body en RAM interna (%u bytes, sin PSRAM)", (unsigned)len);
    return p;
  }

  LOG("UPLOAD: sin memoria para el body: pedidos %u bytes, mayor bloque "
      "interno libre %u bytes", (unsigned)len,
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return nullptr;
}

/* ---------------------------------------------------------------------------
 * Cabecera del multipart: partes de texto de la telemetria + cabecera de la
 * parte "file". Todo lo que va antes de los bytes del JPEG.
 *
 *  Tamano maximo con los 10 campos: 10 x (80 de delimitador y
 *  Content-Disposition) + 105 de nombres + ~106 de valores + 130 de la
 *  cabecera de "file" = ~1,15 KB. Buffer estatico (en .bss, no en la pila de
 *  loopTask, que tambien usa HTTPClient) con margen de sobra.
 * -------------------------------------------------------------------------*/
static char s_head[1536];

/* Anade una parte de texto. false si no cabe (no deberia pasar nunca). */
static bool add_field(size_t &len, const char *name, const char *value) {
  const int n = snprintf(s_head + len, sizeof(s_head) - len,
      "--%s\r\n"
      "Content-Disposition: form-data; name=\"%s\"\r\n\r\n"
      "%s\r\n",
      kBoundary, name, value);
  if (n < 0 || (size_t)n >= sizeof(s_head) - len) return false;
  len += (size_t)n;
  return true;
}

/* Los numeros van como texto decimal: en multipart todo es texto, y el Fog
 * los declara como str a proposito (un int mal formado daria 422 y se
 * perderia la foto; asi lo descarta solo a el). */
static bool add_int(size_t &len, const char *name, long value) {
  char v[16];
  snprintf(v, sizeof(v), "%ld", value);
  return add_field(len, name, v);
}

static bool add_uint(size_t &len, const char *name, unsigned long value) {
  char v[16];
  snprintf(v, sizeof(v), "%lu", value);
  return add_field(len, name, v);
}

static size_t build_telemetry(const UploadTelemetry &t) {
  size_t len = 0;
  const bool ok =
      add_int (len, "fw_version",       t.fw_version)       &&
      add_int (len, "rssi",             t.rssi)             &&
      add_uint(len, "wifi_connect_ms",  t.wifi_connect_ms)  &&
      add_int (len, "reset_reason",     t.reset_reason)     &&
      add_uint(len, "awake_ms",         t.awake_ms)         &&
      add_uint(len, "free_heap",        t.free_heap)        &&
      add_uint(len, "wake_count",       t.wake_count)       &&
      add_uint(len, "wifi_fail_streak", t.wifi_fail_streak) &&
      add_field(len, "framesize",       t.framesize ? t.framesize : "") &&
      add_int (len, "jpeg_quality",     t.jpeg_quality);
  if (!ok) {
    /* La foto es lo critico: si la telemetria no cabe, se manda sin ella. */
    LOG("UPLOAD: la telemetria no cabe en el buffer, se envia solo la foto");
    return 0;
  }
  return len;
}

int upload_frame(const char *fog_host, uint16_t fog_port,
                 camera_fb_t *fb, const UploadTelemetry *telemetry) {
  if (!fog_host || fog_host[0] == '\0' || fog_port == 0) {
    LOG("UPLOAD: sin host/puerto del Fog, no se sube");
    return -1;
  }
  if (!fb || !fb->buf || fb->len == 0) {
    LOG("UPLOAD: frame invalido, no se sube");
    return -1;
  }

  size_t head_len = 0;
  if (telemetry) {
    head_len = build_telemetry(*telemetry);
    LOG("UPLOAD: telemetria fw=%d rssi=%d wifi_ms=%lu reset=%d awake_ms=%lu "
        "heap=%lu wake=%lu wifi_fails=%lu %s q=%d",
        telemetry->fw_version, telemetry->rssi,
        (unsigned long)telemetry->wifi_connect_ms, telemetry->reset_reason,
        (unsigned long)telemetry->awake_ms, (unsigned long)telemetry->free_heap,
        (unsigned long)telemetry->wake_count,
        (unsigned long)telemetry->wifi_fail_streak,
        telemetry->framesize ? telemetry->framesize : "?",
        telemetry->jpeg_quality);
  }

  /* Cabecera de la parte "file", detras de la telemetria. Cabe siempre: el
   * buffer se dimensiona para los dos juntos (ver arriba). */
  const int file_hdr = snprintf(s_head + head_len, sizeof(s_head) - head_len,
      "--%s\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"frame.jpg\"\r\n"
      "Content-Type: image/jpeg\r\n\r\n",
      kBoundary);
  if (file_hdr < 0 || (size_t)file_hdr >= sizeof(s_head) - head_len) {
    LOG("UPLOAD: cabecera multipart demasiado larga, no se sube");
    return -1;
  }
  head_len += (size_t)file_hdr;

  char tail[64];
  const int tail_len = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", kBoundary);

  const size_t total_len = head_len + fb->len + (size_t)tail_len;

  uint8_t *body = alloc_body(total_len);
  if (!body) return -1;

  memcpy(body, s_head, head_len);
  memcpy(body + head_len, fb->buf, fb->len);
  memcpy(body + head_len + fb->len, tail, (size_t)tail_len);

  /* Sin DEVICE_ID en la URL: el Fog identifica la camara por la credencial
   * (X-API-Key + X-API-Token), asi una camara no puede subir frames a otra. */
  const String url = String("http://") + fog_host + ":" + (unsigned)fog_port +
                     FOG_FRAMES_PATH;

  HTTPClient http;
  http.begin(url);
  http.setTimeout(WIFI_CONNECT_TIMEOUT_MS);
  http.addHeader("X-API-Key", API_KEY);
  http.addHeader("X-API-Token", API_TOKEN);
  http.addHeader("Content-Type", String("multipart/form-data; boundary=") + kBoundary);

  LOG("UPLOAD: POST %s (%u bytes)", url.c_str(), (unsigned)total_len);
  const int code = http.POST(body, total_len);
  if (code > 0) {
    LOG("UPLOAD: respuesta HTTP %d", code);
  } else {
    LOG("UPLOAD: fallo la conexion (%s)", http.errorToString(code).c_str());
  }

  http.end();
  free(body);
  return code;
}
