/* ============================================================================
 *  settings_module.h  -  Configuracion propia de la placa en la NVS
 *
 *  UNICA puerta a la NVS del firmware (sobre Preferences, del core). Ningun
 *  otro modulo abre la NVS: asi el esquema esta en un solo sitio y se puede
 *  versionar (SETTINGS_SCHEMA_VERSION en config.h).
 *
 *  Esquema completo desde E6, aunque E6 solo use WiFi y Fog:
 *
 *    clave       tipo   contenido                                  usa
 *    ---------   ----   ----------------------------------------   ---
 *    schema      u8     version del esquema                        E6
 *    wifi        blob   WiFi (SettingsWifi), CONGELADO             E6
 *    fog         blob   host y puerto del Fog (SettingsFog), CONG.  E6
 *    cred        blob   codigo de camara + API key/token           E7
 *    capture     blob   intervalo, framesize, calidad              E8
 *    cfg_rev     u32    ultima revision de config aplicada         E8
 *    ota_rej     u32    version OTA rechazada por rollback         E5
 *    bat_cal     blob   calibracion del divisor de bateria, CONG.  bateria
 *
 *  wifi, fog y bat_cal (CONG. = congelados) solo se graban por USB y ningun
 *  cambio de esquema los borra (ver reset_keeping_usb_keys() en el .cpp): ni
 *  el Fog ni la nube los cambian (decision del 2026-10-17). Las claves wifi_pend,
 *  wifi_tries y wifi_rej del plan E8 se retiraron sin llegar a escribirse;
 *  no reutilizar esos nombres para otra cosa.
 *
 *  Dato nuevo = clave NUEVA. No cambiar la forma de un blob existente: eso
 *  obliga a subir SETTINGS_SCHEMA_VERSION, y el resto (credencial, captura...)
 *  se pierde en todas las placas a la vez al llegar el firmware por OTA.
 *
 *  bat_cal se anadio despues de E6 como clave NUEVA, sin tocar la forma de
 *  ningun blob: una placa ya aprovisionada no la tiene y usa los defectos,
 *  asi que no hizo falta subir SETTINGS_SCHEMA_VERSION.
 *
 *  Cada grupo que se escribe junto va en UN blob: la NVS escribe una entrada
 *  de forma atomica, asi que un corte de alimentacion a mitad de grabar no
 *  deja, por ejemplo, un SSID nuevo con la contrasena vieja. Un blob con un
 *  tamano distinto del esperado (firmware con otro esquema) se trata como
 *  ausente.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "../../config.h"

/* wifi, fog y bat_cal: CONGELADOS. Si cambiase su tamano, la NVS de las
 * placas ya no encajaria y reset_keeping_usb_keys() los descartaria: habria
 * que reaprovisionar cada placa por USB. 32 + 63 son los maximos de WPA2 y
 * 63 los de un nombre de host razonable: no hay motivo para cambiarlos. */
struct SettingsWifi {
  char ssid[WIFI_SSID_MAX_LEN + 1];
  char password[WIFI_PASS_MAX_LEN + 1];
};
static_assert(sizeof(SettingsWifi) == 97, "SettingsWifi esta congelado: ver el comentario");

struct SettingsFog {
  char     host[FOG_HOST_MAX_LEN + 1];
  uint16_t port;
};
static_assert(sizeof(SettingsFog) == 66, "SettingsFog esta congelado: ver SettingsWifi");

struct SettingsCredential {
  char camera_code[CAM_CODE_MAX_LEN + 1];
  char api_key[API_SECRET_MAX_LEN + 1];
  char api_token[API_SECRET_MAX_LEN + 1];
};

/* Dos puntos (tension en GPIO13, tension real del pack), medidos a la vez
 * con el monitor serie y un multimetro. Ver la seccion de bateria de config.h. */
struct SettingsBatteryCal {
  uint16_t adc_a_mv;
  uint16_t bat_a_mv;
  uint16_t adc_b_mv;
  uint16_t bat_b_mv;
};
static_assert(sizeof(SettingsBatteryCal) == 8, "SettingsBatteryCal esta congelado: ver SettingsWifi");

struct SettingsCapture {
  uint32_t interval_s;
  char     framesize[CAP_FRAMESIZE_MAX_LEN + 1];
  uint8_t  jpeg_quality;
};

/* Abre la NVS (lectura/escritura) para todo el ciclo. Si la version del
 * esquema guardada no coincide, borra el namespace: es preferible volver a
 * aprovisionar que interpretar blobs de otra forma. false = NVS inusable. */
bool settings_begin();

/* --- WiFi (E6). Solo la escribe el aprovisionamiento por USB --------------*/
bool settings_has_wifi();
bool settings_get_wifi(SettingsWifi &out);
bool settings_set_wifi(const SettingsWifi &wifi);

/* --- Fog (E6). Sin valor en la NVS, devuelve los defectos de config.h ------
 * Devuelve true si el valor viene de la NVS y false si son los defectos. */
bool settings_get_fog(SettingsFog &out);
bool settings_set_fog(const SettingsFog &fog);

/* --- Credencial de la camara (E7) ------------------------------------------*/
bool settings_get_credential(SettingsCredential &out);
bool settings_set_credential(const SettingsCredential &cred);
bool settings_clear_credential();

/* --- Calibracion de la bateria. Sin valor valido en la NVS, devuelve los
 * defectos de config.h. true si viene de la NVS. -----------------------------*/
bool settings_get_battery_cal(SettingsBatteryCal &out);
bool settings_set_battery_cal(const SettingsBatteryCal &cal);
/* Reglas de una calibracion aceptable (las usa tambien el aprovisionamiento).
 * why: motivo legible si no lo es. */
bool settings_battery_cal_valid(const SettingsBatteryCal &cal, const char **why);

/* --- Captura y revision de la config del Fog (E8) --------------------------*/
bool     settings_get_capture(SettingsCapture &out);
bool     settings_set_capture(const SettingsCapture &cap);
uint32_t settings_config_revision();       // 0 = nunca aplicada
bool     settings_set_config_revision(uint32_t revision);

/* --- OTA (E5) ---------------------------------------------------------------*/
uint32_t settings_rejected_ota_version();  // 0 = ninguna
bool     settings_set_rejected_ota_version(uint32_t version);

/* --- Mantenimiento ----------------------------------------------------------*/
/* Borra TODO el namespace del proyecto (no toca la NVS interna del WiFi ni de
 * la calibracion de radio). Tras esto la placa vuelve a pedir aprovisionar. */
bool settings_erase_all();
