/* ============================================================================
 *  fogconfig_module.cpp  -  Lee la config de la camara que sirve el Fog
 *
 *  Fuente de verdad del formato: fog-application/src/vision/schemas.py
 *  (CameraConfigResponse, FirmwareConfig) y src/firmware/service.py
 *  (firmware_block). Todo lo que llega se valida aqui: el Fog esta en la
 *  LAN, por HTTP plano, y lo que de verdad protege es la firma (ota_module),
 *  pero un campo absurdo no debe llegar ni a intentarlo.
 * ==========================================================================*/
#include "fogconfig_module.h"

#include <HTTPClient.h>
#include "cJSON.h"

/* La respuesta ocupa ~400 B. Algo mucho mayor no es esta config. */
static const int kMaxBody = 4096;

static bool get_u32(const cJSON *obj, const char *name, uint32_t min, uint32_t max,
                    uint32_t *out) {
  const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, name);
  if (!cJSON_IsNumber(it)) return false;
  const double v = it->valuedouble;
  if (v < min || v > max || v != (double)(uint32_t)v) return false;   // entero y en rango
  *out = (uint32_t)v;
  return true;
}

static bool get_str(const cJSON *obj, const char *name, char *out, size_t cap) {
  const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, name);
  if (!cJSON_IsString(it) || it->valuestring[0] == '\0' || strlen(it->valuestring) >= cap) {
    return false;
  }
  strlcpy(out, it->valuestring, cap);
  return true;
}

static bool parse_firmware(const cJSON *fw, FirmwareOffer &o) {
  if (!get_u32(fw, "version", 1, 1000000, &o.version) ||
      !get_u32(fw, "size", 1, 4 * 1024 * 1024, &o.size) ||
      !get_str(fw, "sha256", o.sha256, sizeof(o.sha256)) ||
      !get_str(fw, "signature", o.signature, sizeof(o.signature)) ||
      !get_str(fw, "url", o.url, sizeof(o.url))) {
    return false;
  }
  /* 64 hex; se pasa a minusculas porque asi entra en la cadena firmada. */
  if (strlen(o.sha256) != 64) return false;
  for (char *c = o.sha256; *c; c++) {
    if (!isxdigit((unsigned char)*c)) return false;
    *c = (char)tolower((unsigned char)*c);
  }
  /* Ruta del mismo Fog, nunca una URL completa: la camara solo habla con el
   * host aprovisionado por USB. */
  if (o.url[0] != '/' || strstr(o.url, "://") || strchr(o.url, ' ')) return false;
  return true;
}

bool fogconfig_fetch(const char *fog_host, uint16_t fog_port,
                     const char *api_key, const char *api_token, FogConfig &out) {
  memset(&out, 0, sizeof(out));

  const String url = String("http://") + fog_host + ":" + (unsigned)fog_port + FOG_CONFIG_PATH;
  HTTPClient http;
  http.begin(url);
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);
  http.addHeader("X-API-Key", api_key);
  http.addHeader("X-API-Token", api_token);

  const int code = http.GET();
  if (code != 200) {
    LOG("OTA: GET %s -> %d", FOG_CONFIG_PATH, code);
    http.end();
    return false;
  }
  const int len = http.getSize();
  if (len > kMaxBody) {
    LOG("OTA: config demasiado grande (%d bytes)", len);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  cJSON *root = cJSON_Parse(body.c_str());
  /* La respuesta puede traer la contrasena WiFi (el Fog aun manda "wifi"):
   * no se deja en el heap mas de lo necesario. */
  memset((void *)body.c_str(), 0, body.length());
  if (!cJSON_IsObject(root)) {
    LOG("OTA: la config no es un JSON valido");
    cJSON_Delete(root);
    return false;
  }

  const cJSON *fw = cJSON_GetObjectItemCaseSensitive(root, "firmware");
  if (fw && !cJSON_IsNull(fw)) {
    out.has_firmware = cJSON_IsObject(fw) && parse_firmware(fw, out.firmware);
    if (!out.has_firmware) LOG("OTA: bloque firmware mal formado, se ignora");
  }
  cJSON_Delete(root);
  return true;
}
