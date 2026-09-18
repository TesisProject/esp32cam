/* ============================================================================
 *  upload_module.cpp  -  POST multipart/form-data a la Raspberry Pi
 *
 *  HTTPClient (core ESP32) no trae helper de multipart, asi que el cuerpo se
 *  arma a mano: prefijo + bytes JPEG + campos de texto + cierre, en un unico
 *  buffer de PSRAM. Con el Content-Length exacto ya calculado, http.POST()
 *  manda todo de una vez y evita un body en streaming/chunked.
 *
 *  El contrato del Fog exige tres cosas en la parte "file", y las tres estan
 *  en el prefijo de abajo: el nombre de campo "file", un filename en el
 *  Content-Disposition (sin el, 422) y su propio Content-Type (sin el, 415).
 *
 *  Los campos de telemetria van DESPUES de la imagen. El orden de las partes
 *  es indiferente en multipart, y dejar el JPEG primero permite construirlo
 *  sin recorrer la telemetria dos veces.
 *
 *  No se manda el campo opcional "captured_at": el ESP32 despierta del deep
 *  sleep sin hora valida (no hay RTC con pila ni NTP en el ciclo), asi que
 *  solo podria enviar una marca inventada. Ademas es el UNICO campo que el
 *  Fog valida de forma estricta: mal formado devuelve 422 y se pierde la foto.
 *  Si algun dia se anade NTP, tiene que ser ISO 8601 o no enviarse.
 * ==========================================================================*/
#include "upload_module.h"
#include "config.h"
#include "secrets.h"
#include "camera_module.h"

#include <HTTPClient.h>
#if FOG_MDNS_ENABLED
  #include <ESPmDNS.h>
#endif

static const char *kBoundary = "ESP32CAMBoundary7MA4YWxkTrZu0gW";

/* Buffer de los campos de texto. Estatico y no en pila: la tarea de Arduino
 * tiene 8 KB y este modulo no es reentrante. 13 campos caben de sobra. */
static char s_fields[1536];

/* ===========================================================================
 * Resolucion del host del Fog
 *
 *  La IP resuelta por mDNS se guarda en RTC memory, que sobrevive al deep
 *  sleep: solo el primer ciclo tras un arranque en frio paga la consulta. Los
 *  siguientes van directos, asi que usar un nombre .local no cuesta radio
 *  extra en regimen normal.
 *
 *  Vacia = sin resolver. Se invalida sola cuando el POST falla por conexion
 *  (ver upload_forget_host), que es como se absorbe un cambio de IP de la Pi.
 * ==========================================================================*/
RTC_DATA_ATTR static char rtc_fog_ip[16] = {0};

void upload_forget_host() {
  if (rtc_fog_ip[0] != 0) {
    LOG("UPLOAD: se olvida la IP cacheada %s, se resolvera de nuevo", rtc_fog_ip);
    rtc_fog_ip[0] = 0;
  }
}

static String resolve_fog_host() {
  const String host(FOG_SERVER_HOST);

#if !FOG_MDNS_ENABLED
  return host;
#else
  /* Una IP o un nombre normal se usan tal cual: no hay nada que resolver. */
  if (!host.endsWith(".local")) return host;

  if (rtc_fog_ip[0] != 0) return String(rtc_fog_ip);

  /* MDNS.begin() hace falta para poder consultar, no solo para anunciarse. */
  const String name = host.substring(0, host.length() - 6);   // sin ".local"
  if (MDNS.begin(DEVICE_HOSTNAME)) {
    const IPAddress ip = MDNS.queryHost(name.c_str(), FOG_MDNS_TIMEOUT_MS);
    if ((uint32_t)ip != 0) {
      const String txt = ip.toString();
      strncpy(rtc_fog_ip, txt.c_str(), sizeof(rtc_fog_ip) - 1);
      rtc_fog_ip[sizeof(rtc_fog_ip) - 1] = 0;
      LOG("UPLOAD: mDNS %s -> %s (cacheado para los proximos ciclos)",
          FOG_SERVER_HOST, rtc_fog_ip);
      return txt;
    }
  }

  LOG("UPLOAD: mDNS no resolvio %s, se usa la IP de reserva %s",
      FOG_SERVER_HOST, FOG_SERVER_FALLBACK_IP);
  return String(FOG_SERVER_FALLBACK_IP);
#endif
}

void upload_telemetry_clear(UploadTelemetry *t) {
  if (!t) return;
  memset(t, 0, sizeof(*t));
}

/* Anade una parte de texto. El salto de linea inicial cierra el cuerpo de la
 * parte anterior, sea el JPEG o el campo previo. */
static size_t append_field(char *dst, size_t cap, size_t used,
                           const char *name, const char *value) {
  if (used >= cap) return used;
  const int n = snprintf(dst + used, cap - used,
      "\r\n--%s\r\n"
      "Content-Disposition: form-data; name=\"%s\"\r\n\r\n"
      "%s",
      kBoundary, name, value);
  if (n < 0 || (size_t)n >= cap - used) {
    dst[used] = 0;          // deshace lo truncado: el campo se omite entero
    return used;
  }
  return used + (size_t)n;
}

static size_t append_u32(char *dst, size_t cap, size_t used,
                         const char *name, uint32_t value) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)value);
  return append_field(dst, cap, used, name, buf);
}

static size_t append_i32(char *dst, size_t cap, size_t used,
                         const char *name, int32_t value) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%ld", (long)value);
  return append_field(dst, cap, used, name, buf);
}

/* Construye las partes de telemetria. Un valor a cero significa "sin dato" y
 * se omite: el contrato dice opcionales, y un campo vacio seria un dato falso.
 * Excepcion: battery_valid se manda siempre, porque su "false" es informacion
 * (el ADS1115 no respondio), no ausencia de dato. */
static size_t build_fields(const UploadTelemetry &t) {
#if !UPLOAD_TELEMETRY_ENABLED
  (void)t;
  s_fields[0] = 0;
  return 0;
#else
  size_t used = 0;
  s_fields[0] = 0;

  used = append_field(s_fields, sizeof(s_fields), used, "battery_valid",
                      t.battery_valid ? "true" : "false");

  if (t.battery_mv)      used = append_u32(s_fields, sizeof(s_fields), used, "battery_mv", t.battery_mv);
  if (t.battery_load_mv) used = append_u32(s_fields, sizeof(s_fields), used, "battery_load_mv", t.battery_load_mv);

  /* RSSI es negativo siempre; 0 dBm no es una lectura real, es "sin dato". */
  if (t.rssi != 0)       used = append_i32(s_fields, sizeof(s_fields), used, "rssi", t.rssi);
  if (t.wifi_connect_ms) used = append_u32(s_fields, sizeof(s_fields), used, "wifi_connect_ms", t.wifi_connect_ms);

  used = append_u32(s_fields, sizeof(s_fields), used, "reset_reason", (uint32_t)t.reset_reason);
  used = append_u32(s_fields, sizeof(s_fields), used, "awake_ms", t.awake_ms);
  used = append_u32(s_fields, sizeof(s_fields), used, "free_heap", t.free_heap);
  used = append_field(s_fields, sizeof(s_fields), used, "fw_version", FW_VERSION);
  used = append_u32(s_fields, sizeof(s_fields), used, "wake_count", t.wake_count);
  used = append_u32(s_fields, sizeof(s_fields), used, "wifi_fail_streak", t.wifi_fail_streak);

  used = append_field(s_fields, sizeof(s_fields), used, "framesize", camera_framesize_name());
  used = append_u32(s_fields, sizeof(s_fields), used, "jpeg_quality", (uint32_t)CAM_JPEG_QUALITY_SNAPSHOT);

  return used;
#endif
}

int upload_frame(camera_fb_t *fb, const UploadTelemetry &telemetry) {
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

  const size_t fields_len = build_fields(telemetry);

  char tail[64];
  const int tail_len = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", kBoundary);

  const size_t total_len =
      (size_t)head_len + fb->len + fields_len + (size_t)tail_len;

  uint8_t *body = (uint8_t *)ps_malloc(total_len);
  if (!body) {
    LOG("UPLOAD: sin memoria PSRAM para el body (%u bytes)", (unsigned)total_len);
    return -1;
  }

  size_t off = 0;
  memcpy(body + off, head, (size_t)head_len);   off += (size_t)head_len;
  memcpy(body + off, fb->buf, fb->len);         off += fb->len;
  memcpy(body + off, s_fields, fields_len);     off += fields_len;
  memcpy(body + off, tail, (size_t)tail_len);

  /* La URL no lleva el id de la camara: la identifica la credencial. */
  const String url = String("http://") + resolve_fog_host() + ":" +
                     FOG_SERVER_PORT + FOG_FRAMES_PATH;

  HTTPClient http;
  http.begin(url);
  http.setTimeout(WIFI_CONNECT_TIMEOUT_MS);
  http.addHeader("X-API-Key", API_KEY);
  http.addHeader("X-API-Token", API_TOKEN);
  http.addHeader("Content-Type", String("multipart/form-data; boundary=") + kBoundary);

  LOG("UPLOAD: POST %s (%u bytes img + %u bytes telemetria)",
      url.c_str(), (unsigned)fb->len, (unsigned)fields_len);
  const int code = http.POST(body, total_len);

  if (code > 0) {
    LOG("UPLOAD: respuesta HTTP %d", code);
    /* El cuerpo son ~60 bytes de JSON y trae el camera_id que el Fog asocia a
     * esta credencial: la unica forma de comprobar desde el dispositivo que se
     * grabo el secrets.h correcto (CAM-001 flasheada con la clave de CAM-002
     * subiria fotos a la zona equivocada sin dar ningun error). */
    const String payload = http.getString();
    if (payload.length() > 0) {
      LOG("UPLOAD: cuerpo %s", payload.c_str());
    }
  } else {
    LOG("UPLOAD: fallo la conexion (%s)", http.errorToString(code).c_str());
  }

  http.end();
  free(body);
  return code;
}
