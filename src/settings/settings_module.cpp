/* ============================================================================
 *  settings_module.cpp  -  Esquema de la NVS sobre Preferences (core ESP32)
 *
 *  Los nombres de clave son el esquema: no renombrarlos sin subir
 *  SETTINGS_SCHEMA_VERSION (config.h), o una placa ya aprovisionada dejaria
 *  de encontrar su WiFi. Limite de la NVS: 15 caracteres por clave.
 *
 *  En un ciclo normal este modulo solo LEE: no se escribe la flash en cada
 *  despertar (la NVS reparte el desgaste, pero no hay por que gastarlo).
 * ==========================================================================*/
#include "settings_module.h"

#include <Preferences.h>

static const char *kKeySchema     = "schema";
static const char *kKeyWifi       = "wifi";
static const char *kKeyWifiPend   = "wifi_pend";
static const char *kKeyWifiTries  = "wifi_tries";
static const char *kKeyWifiRej    = "wifi_rej";
static const char *kKeyFog        = "fog";
static const char *kKeyCred       = "cred";
static const char *kKeyCapture    = "capture";
static const char *kKeyCfgRev     = "cfg_rev";
static const char *kKeyOtaRej     = "ota_rej";

static Preferences s_prefs;
static bool        s_open = false;

/* ---------------------------------------------------------------------------
 * Blobs: lectura con comprobacion de tamano y escritura atomica
 * -------------------------------------------------------------------------*/
template <typename T>
static bool get_blob(const char *key, T &out) {
  if (!s_open || !s_prefs.isKey(key)) return false;
  const size_t len = s_prefs.getBytesLength(key);
  if (len != sizeof(T)) {
    /* Otro firmware con otra forma del struct. Leerlo daria basura. */
    LOG("NVS: '%s' tiene %u bytes y se esperaban %u, se ignora",
        key, (unsigned)len, (unsigned)sizeof(T));
    return false;
  }
  return s_prefs.getBytes(key, &out, sizeof(T)) == sizeof(T);
}

template <typename T>
static bool put_blob(const char *key, const T &value) {
  if (!s_open) return false;
  const bool ok = s_prefs.putBytes(key, &value, sizeof(T)) == sizeof(T);
  if (!ok) LOG("NVS: fallo al escribir '%s'", key);
  return ok;
}

static bool remove_key(const char *key) {
  if (!s_open) return false;
  if (!s_prefs.isKey(key)) return true;
  return s_prefs.remove(key);
}

/* Los arrays de texto se guardan con su tamano fijo: se fuerza el '\0'
 * final al leer por si el blob viniera corrupto. */
template <size_t N>
static void terminate(char (&s)[N]) { s[N - 1] = '\0'; }

/* ---------------------------------------------------------------------------
 * Apertura
 * -------------------------------------------------------------------------*/
bool settings_begin() {
  if (s_open) return true;
  /* Lectura/escritura siempre: en modo solo lectura, la primera vez (el
   * namespace aun no existe) begin() falla. */
  s_open = s_prefs.begin(SETTINGS_NVS_NAMESPACE, false);
  if (!s_open) {
    LOG("NVS: no se pudo abrir el namespace '%s'", SETTINGS_NVS_NAMESPACE);
    return false;
  }

  const uint8_t schema = s_prefs.getUChar(kKeySchema, 0);
  if (schema != SETTINGS_SCHEMA_VERSION) {
    if (schema != 0) {
      LOG("NVS: esquema %u != %u, se borra y habra que aprovisionar de nuevo",
          (unsigned)schema, (unsigned)SETTINGS_SCHEMA_VERSION);
    }
    s_prefs.clear();
    s_prefs.putUChar(kKeySchema, SETTINGS_SCHEMA_VERSION);
  }
  return true;
}

/* ---------------------------------------------------------------------------
 * WiFi activa
 * -------------------------------------------------------------------------*/
bool settings_get_wifi(SettingsWifi &out) {
  if (!get_blob(kKeyWifi, out)) return false;
  terminate(out.ssid);
  terminate(out.password);
  return out.ssid[0] != '\0';
}

bool settings_has_wifi() {
  SettingsWifi w;
  return settings_get_wifi(w);
}

bool settings_set_wifi(const SettingsWifi &wifi) { return put_blob(kKeyWifi, wifi); }

/* ---------------------------------------------------------------------------
 * WiFi pendiente (E8)
 * -------------------------------------------------------------------------*/
bool settings_get_pending_wifi(SettingsPendingWifi &out) {
  if (!get_blob(kKeyWifiPend, out)) return false;
  terminate(out.wifi.ssid);
  terminate(out.wifi.password);
  return out.wifi.ssid[0] != '\0';
}

bool settings_set_pending_wifi(const SettingsPendingWifi &pending) {
  /* Una pendiente nueva empieza sin intentos fallidos. */
  return put_blob(kKeyWifiPend, pending) && settings_set_pending_wifi_tries(0);
}

uint8_t settings_pending_wifi_tries() {
  return s_open ? s_prefs.getUChar(kKeyWifiTries, 0) : 0;
}

bool settings_set_pending_wifi_tries(uint8_t tries) {
  return s_open && s_prefs.putUChar(kKeyWifiTries, tries) == sizeof(uint8_t);
}

bool settings_promote_pending_wifi() {
  SettingsPendingWifi p;
  if (!settings_get_pending_wifi(p)) return false;
  /* Primero la activa: si se corta aqui, en el peor caso queda la pendiente
   * repetida, nunca la placa sin WiFi. */
  if (!settings_set_wifi(p.wifi)) return false;
  return remove_key(kKeyWifiPend) && remove_key(kKeyWifiTries);
}

bool settings_discard_pending_wifi() {
  SettingsPendingWifi p;
  if (settings_get_pending_wifi(p)) {
    s_prefs.putUInt(kKeyWifiRej, p.revision);
  }
  return remove_key(kKeyWifiPend) && remove_key(kKeyWifiTries);
}

uint32_t settings_rejected_wifi_revision() {
  return s_open ? s_prefs.getUInt(kKeyWifiRej, 0) : 0;
}

/* ---------------------------------------------------------------------------
 * Fog
 * -------------------------------------------------------------------------*/
bool settings_get_fog(SettingsFog &out) {
  if (get_blob(kKeyFog, out)) {
    terminate(out.host);
    if (out.host[0] != '\0' && out.port != 0) return true;
  }
  /* Sin valor valido en la NVS: los defectos de config.h. */
  strlcpy(out.host, FOG_SERVER_HOST_DEFAULT, sizeof(out.host));
  out.port = FOG_SERVER_PORT_DEFAULT;
  return false;
}

bool settings_set_fog(const SettingsFog &fog) { return put_blob(kKeyFog, fog); }

/* ---------------------------------------------------------------------------
 * Credencial (E7)
 * -------------------------------------------------------------------------*/
bool settings_get_credential(SettingsCredential &out) {
  if (!get_blob(kKeyCred, out)) return false;
  terminate(out.camera_code);
  terminate(out.api_key);
  terminate(out.api_token);
  return out.api_key[0] != '\0' && out.api_token[0] != '\0';
}

bool settings_set_credential(const SettingsCredential &cred) {
  return put_blob(kKeyCred, cred);
}

bool settings_clear_credential() { return remove_key(kKeyCred); }

/* ---------------------------------------------------------------------------
 * Captura y revision (E8)
 * -------------------------------------------------------------------------*/
bool settings_get_capture(SettingsCapture &out) {
  if (!get_blob(kKeyCapture, out)) return false;
  terminate(out.framesize);
  return true;
}

bool settings_set_capture(const SettingsCapture &cap) { return put_blob(kKeyCapture, cap); }

uint32_t settings_config_revision() { return s_open ? s_prefs.getUInt(kKeyCfgRev, 0) : 0; }

bool settings_set_config_revision(uint32_t revision) {
  return s_open && s_prefs.putUInt(kKeyCfgRev, revision) == sizeof(uint32_t);
}

/* ---------------------------------------------------------------------------
 * OTA (E5)
 * -------------------------------------------------------------------------*/
uint32_t settings_rejected_ota_version() { return s_open ? s_prefs.getUInt(kKeyOtaRej, 0) : 0; }

bool settings_set_rejected_ota_version(uint32_t version) {
  return s_open && s_prefs.putUInt(kKeyOtaRej, version) == sizeof(uint32_t);
}

/* ---------------------------------------------------------------------------
 * Mantenimiento
 * -------------------------------------------------------------------------*/
bool settings_erase_all() {
  if (!s_open) return false;
  /* clear() del namespace, no nvs_flash_erase(): esta ultima borraria tambien
   * la NVS del driver WiFi y la calibracion de radio (PHY), que no son
   * configuracion del proyecto y cuesta tiempo regenerar. */
  const bool ok = s_prefs.clear();
  s_prefs.putUChar(kKeySchema, SETTINGS_SCHEMA_VERSION);
  return ok;
}
