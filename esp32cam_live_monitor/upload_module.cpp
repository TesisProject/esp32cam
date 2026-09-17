/* ============================================================================
 *  upload_module.cpp  -  POST multipart/form-data a la Raspberry Pi
 *
 *  HTTPClient (core ESP32) no trae helper de multipart, asi que el cuerpo se
 *  arma a mano: prefijo + bytes JPEG + cierre, en un unico buffer de PSRAM.
 *  Con el Content-Length exacto ya calculado, http.POST(buffer, len) manda
 *  todo de una vez y evita la complejidad de un body en streaming/chunked.
 * ==========================================================================*/
#include "upload_module.h"
#include "config.h"
#include "secrets.h"

#include <HTTPClient.h>

static const char *kBoundary = "ESP32CAMBoundary7MA4YWxkTrZu0gW";

int upload_frame(camera_fb_t *fb) {
  if (!fb || !fb->buf || fb->len == 0) {
    LOG("UPLOAD: frame invalido, no se sube");
    return -1;
  }

  char head[192];
  const int head_len = snprintf(head, sizeof(head),
      "--%s\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"frame.jpg\"\r\n"
      "Content-Type: image/jpeg\r\n\r\n",
      kBoundary);

  char tail[64];
  const int tail_len = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", kBoundary);

  const size_t total_len = (size_t)head_len + fb->len + (size_t)tail_len;

  uint8_t *body = (uint8_t *)ps_malloc(total_len);
  if (!body) {
    LOG("UPLOAD: sin memoria PSRAM para el body (%u bytes)", (unsigned)total_len);
    return -1;
  }

  memcpy(body, head, (size_t)head_len);
  memcpy(body + head_len, fb->buf, fb->len);
  memcpy(body + head_len + fb->len, tail, (size_t)tail_len);

  const String url = String("http://") + FOG_SERVER_HOST + ":" + FOG_SERVER_PORT +
                      "/api/v1/cameras/" + DEVICE_ID + "/frames";

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
