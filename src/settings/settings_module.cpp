/* ============================================================================
 *  settings_module.cpp  -  Esquema de la NVS sobre Preferences (core ESP32)
 *
 *  Los nombres de clave son el esquema. "schema", "wifi", "fog", "bat_cal" y
 *  "cred" no se renombran NUNCA: una placa en campo dejaria de encontrarlos y solo
 *  se arregla por USB. Limite de la NVS: 15 caracteres por clave.
 *
 *  En un ciclo normal este modulo solo LEE: no se escribe la flash en cada
 *  despertar (la NVS reparte el desgaste, pero no hay por que gastarlo).
 * ==========================================================================*/
#include "settings_module.h"

#include <Preferences.h>
#include <nvs.h>

static const char *kKeySchema     = "schema";
static const char *kKeyWifi       = "wifi";
static const char *kKeyFog        = "fog";
static const char *kKeyCred       = "cred";
static const char *kKeyCapture    = "capture";
static const char *kKeyCfgRev     = "cfg_rev";
static const char *kKeyOtaRej     = "ota_rej";
static const char *kKeyBatCal     = "bat_cal";

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
 * Cambio de esquema: se borra todo MENOS lo que solo se graba por USB
 *
 * WiFi, host del Fog, calibracion de bateria y credencial solo llegan por el
 * aprovisionamiento USB (decision del 2026-10-17: ni el Fog ni la nube
 * cambian la WiFi, y el host nunca vino de la red). Si un firmware nuevo,
 * llegado por OTA, los borrase, la camara ya no encontraria la red o el Fog
 * (o el Fog la rechazaria, sin credencial), ni podria recibir otro OTA que lo
 * arreglase: habria que ir a cada placa con un cable. Lo demas (captura
 * E8...) se vuelve a pedir al Fog.
 *
 * Se borran las claves una a una (no clear() + reescribir: un corte entre las
 * dos dejaria la placa sin WiFi) y el esquema se escribe al final. Si se
 * corta a medias, el siguiente arranque ve el esquema viejo y repite; borrar
 * es idempotente. Funciona igual en un downgrade (rollback del OTA a un
 * firmware con esquema menor), siempre que ese firmware tambien tenga esta
 * funcion: el de E6 (6066aa6) aun hacia clear().
 * -------------------------------------------------------------------------*/
struct UsbKey { const char *key; size_t size; };
/* Sus structs estan CONGELADOS (static_assert en el .h). */
static const UsbKey kUsbKeys[] = {
  { kKeyWifi,   sizeof(SettingsWifi) },
  { kKeyFog,    sizeof(SettingsFog) },
  { kKeyBatCal, sizeof(SettingsBatteryCal) },
  { kKeyCred,   sizeof(SettingsCredential) },
};

static bool is_usb_key(const char *key) {
  for (const UsbKey &k : kUsbKeys) if (strcmp(key, k.key) == 0) return true;
  return false;
}

static void reset_keeping_usb_keys() {
  static const size_t kMaxKeys = 32;   // el esquema tiene ~10 claves
  char keys[kMaxKeys][NVS_KEY_NAME_MAX_SIZE];
  size_t n = 0;

  /* Primero se recogen los nombres y luego se borra: modificar la NVS con un
   * iterador abierto no esta garantizado. */
  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, SETTINGS_NVS_NAMESPACE,
                                 NVS_TYPE_ANY, &it);
  while (err == ESP_OK && n < kMaxKeys) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    if (strcmp(info.key, kKeySchema) != 0 && !is_usb_key(info.key)) {
      strlcpy(keys[n++], info.key, NVS_KEY_NAME_MAX_SIZE);
    }
    err = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);

  for (size_t i = 0; i < n; i++) s_prefs.remove(keys[i]);

  /* Un blob de otro tamano no es de este esquema: no se puede leer. No
   * deberia pasar nunca (structs congelados). */
  for (const UsbKey &k : kUsbKeys) {
    if (s_prefs.isKey(k.key) && s_prefs.getBytesLength(k.key) != k.size) {
      LOG("NVS: '%s' con %u bytes, no es de este esquema: se borra",
          k.key, (unsigned)s_prefs.getBytesLength(k.key));
      s_prefs.remove(k.key);
    }
  }
  s_prefs.putUChar(kKeySchema, SETTINGS_SCHEMA_VERSION);
}

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
      LOG("NVS: esquema %u != %u, se borra todo salvo WiFi, Fog, calibracion y credencial",
          (unsigned)schema, (unsigned)SETTINGS_SCHEMA_VERSION);
    }
    reset_keeping_usb_keys();
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

bool settings_is_provisioned() {
  SettingsCredential c;
  const bool ok = settings_has_wifi() && settings_get_credential(c);
  memset(&c, 0, sizeof(c));   // no dejar el token en la pila
  return ok;
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
 * Credencial
 * -------------------------------------------------------------------------*/
bool settings_get_credential(SettingsCredential &out) {
  if (!get_blob(kKeyCred, out)) return false;
  terminate(out.camera_code);
  terminate(out.api_key);
  terminate(out.api_token);
  return out.camera_code[0] != '\0' && out.api_key[0] != '\0' &&
         out.api_token[0] != '\0';
}

bool settings_set_credential(const SettingsCredential &cred) {
  return put_blob(kKeyCred, cred);
}

bool settings_clear_credential() { return remove_key(kKeyCred); }

/* ---------------------------------------------------------------------------
 * Calibracion de la bateria
 * -------------------------------------------------------------------------*/
bool settings_battery_cal_valid(const SettingsBatteryCal &cal, const char **why) {
  const char *dummy;
  if (!why) why = &dummy;
  if (cal.adc_a_mv > BATTERY_CAL_ADC_MAX_MV || cal.adc_b_mv > BATTERY_CAL_ADC_MAX_MV) {
    *why = "batteryCal: adcA/adcB deben estar entre 0 y 3300 mV";
    return false;
  }
  if (cal.bat_a_mv < BATTERY_VALID_MIN_MV || cal.bat_a_mv > BATTERY_VALID_MAX_MV ||
      cal.bat_b_mv < BATTERY_VALID_MIN_MV || cal.bat_b_mv > BATTERY_VALID_MAX_MV) {
    *why = "batteryCal: batA/batB deben estar entre 2500 y 4500 mV (Li-ion 1S)";
    return false;
  }
  /* B por encima de A en los dos ejes: la recta del divisor siempre sube
   * (mas bateria = mas tension en GPIO13). Al reves es un error de anotacion. */
  if ((int)cal.bat_b_mv - (int)cal.bat_a_mv < BATTERY_CAL_MIN_BAT_SPAN_MV) {
    *why = "batteryCal: batB debe superar a batA en 300 mV o mas (p. ej. celda llena y a media carga)";
    return false;
  }
  if ((int)cal.adc_b_mv - (int)cal.adc_a_mv < BATTERY_CAL_MIN_ADC_SPAN_MV) {
    *why = "batteryCal: adcB debe superar a adcA en 30 mV o mas";
    return false;
  }
  return true;
}

bool settings_get_battery_cal(SettingsBatteryCal &out) {
  /* Se revalida al leer: una recta absurda daria tensiones absurdas y un
   * corte por bateria baja que no existe. Mejor los defectos. */
  if (get_blob(kKeyBatCal, out) && settings_battery_cal_valid(out, nullptr)) return true;
  out.adc_a_mv = BATTERY_CAL_ADC_A_MV_DEFAULT;
  out.bat_a_mv = BATTERY_CAL_BAT_A_MV_DEFAULT;
  out.adc_b_mv = BATTERY_CAL_ADC_B_MV_DEFAULT;
  out.bat_b_mv = BATTERY_CAL_BAT_B_MV_DEFAULT;
  return false;
}

bool settings_set_battery_cal(const SettingsBatteryCal &cal) {
  return put_blob(kKeyBatCal, cal);
}

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
