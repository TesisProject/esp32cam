/* ============================================================================
 *  provision_module.cpp  -  Lee, valida y guarda la linea de aprovisionamiento
 *
 *  Todo se valida ANTES de escribir nada: una linea con un solo campo malo
 *  no deja la NVS a medias. Los rangos se comprueban aqui aunque el script
 *  ya los respete, porque la linea tambien se puede pegar a mano desde un
 *  monitor serie.
 * ==========================================================================*/
#include "provision_module.h"
#include "../../config.h"
#include "../settings/settings_module.h"
#include "../backlog/backlog_module.h"

#include "cJSON.h"

/* Respuestas del protocolo: Serial directo (ver provision_module.h). */
#define PROV_REPLY(fmt, ...)  Serial.printf("PROV: " fmt "\n", ##__VA_ARGS__)

/* Estatico y no en la pila: 512 B de linea + lo que usa cJSON por debajo. */
static char s_line[PROV_LINE_MAX_LEN + 1];

/* ---------------------------------------------------------------------------
 * Validaciones
 * -------------------------------------------------------------------------*/
/* WPA2-PSK: la passphrase son 8..63 caracteres ASCII imprimibles (802.11i).
 * Una tilde o un salto de linea no los aceptaria el router de todas formas:
 * mejor fallar aqui con un motivo claro que en cada ciclo al conectar. */
static bool valid_wpa2_passphrase(const char *p, const char **why) {
  const size_t n = strlen(p);
  if (n < WIFI_PASS_MIN_LEN || n > WIFI_PASS_MAX_LEN) {
    *why = "wifi.password debe tener entre 8 y 63 caracteres (WPA2; no se aceptan redes abiertas)";
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)p[i];
    if (c < 0x20 || c > 0x7E) {
      *why = "wifi.password solo admite ASCII imprimible";
      return false;
    }
  }
  return true;
}

/* Nombre o IPv4. Se rechaza todo lo demas a proposito: el error tipico es
 * escribir "http://192.168.1.20" o "192.168.1.20:8000", y con eso la URL que
 * arma upload_module saldria mal en cada ciclo, sin forma de corregirlo
 * desde la nube. */
static bool valid_host(const char *h, const char **why) {
  const size_t n = strlen(h);
  if (n == 0) { *why = "fogHost esta vacio"; return false; }
  if (n > FOG_HOST_MAX_LEN) { *why = "fogHost demasiado largo (max. 63)"; return false; }
  for (size_t i = 0; i < n; i++) {
    const char c = h[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-';
    if (!ok) {
      *why = "fogHost solo admite letras, digitos, '.' y '-' (sin http:// ni :puerto)";
      return false;
    }
  }
  return true;
}

/* Identificador de la camara: solo para el log, pero se limita a lo que
 * tiene un "CAM-001" para que un campo mal pegado se vea al aprovisionar. */
static bool valid_camera_id(const char *s, const char **why) {
  const size_t n = strlen(s);
  if (n == 0 || n > CAM_CODE_MAX_LEN) {
    *why = "camera.id debe tener entre 1 y 15 caracteres (p. ej. CAM-001)";
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    const char c = s[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) { *why = "camera.id solo admite letras, digitos, '-' y '_'"; return false; }
  }
  return true;
}

/* apiKey/apiToken van tal cual en una cabecera HTTP: nada de espacios ni de
 * caracteres de control. Un espacio o un salto de linea pegado al copiar la
 * credencial daria un 401 en cada ciclo, sin pista en el log de por que. */
static bool valid_api_secret(const char *s, const char *field, const char **why) {
  static char msg[96];
  const size_t n = strlen(s);
  bool ok = n >= 1 && n <= API_SECRET_MAX_LEN;
  for (size_t i = 0; ok && i < n; i++) {
    const unsigned char c = (unsigned char)s[i];
    ok = c >= 0x21 && c <= 0x7E;
  }
  if (!ok) {
    snprintf(msg, sizeof(msg),
             "camera.%s debe tener entre 1 y 63 caracteres ASCII imprimibles, sin espacios",
             field);
    *why = msg;
  }
  return ok;
}

/* ---------------------------------------------------------------------------
 * Procesado de una linea. Solo retorna si hay error (con exito reinicia).
 * -------------------------------------------------------------------------*/
static void restart_now() {
  Serial.flush();
  delay(50);
  ESP.restart();
}

static void handle_line(size_t len) {
  cJSON *root = cJSON_ParseWithLength(s_line, len);
  if (!cJSON_IsObject(root)) {
    PROV_REPLY("ERROR JSON invalido (se espera un objeto en una sola linea)");
    cJSON_Delete(root);
    return;
  }

  const char *why = nullptr;

  /* Campos desconocidos = error: un "fog_host" mal escrito se ignoraria en
   * silencio y la placa seguiria con el host por defecto. */
  for (const cJSON *it = root->child; it; it = it->next) {
    if (strcmp(it->string, "wifi") != 0 && strcmp(it->string, "camera") != 0 &&
        strcmp(it->string, "fogHost") != 0 &&
        strcmp(it->string, "fogPort") != 0 && strcmp(it->string, "batteryCal") != 0 &&
        strcmp(it->string, "cmd") != 0) {
      PROV_REPLY("ERROR campo desconocido: \"%s\"", it->string);
      cJSON_Delete(root);
      return;
    }
  }

  /* --- Comandos -----------------------------------------------------------*/
  const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
  if (cmd) {
    if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "erase") == 0 &&
        cJSON_GetArraySize(root) == 1) {
      cJSON_Delete(root);
      if (settings_erase_all()) {
        /* Las fotos pendientes son de la placa de antes del borrado (otra
         * credencial, quiza otra camara): no deben subirse con la nueva. Que
         * falle no impide el borrado: la NVS es lo que importa. */
        const bool photos = backlog_erase_all();
        PROV_REPLY("OK NVS borrada%s, reiniciando (pedira aprovisionar)",
                   photos ? " y fotos pendientes descartadas" : " (fotos pendientes NO)");
        restart_now();
      }
      PROV_REPLY("ERROR no se pudo borrar la NVS");
      return;
    }
    PROV_REPLY("ERROR comando desconocido (el unico es {\"cmd\":\"erase\"}, solo)");
    cJSON_Delete(root);
    return;
  }

  /* --- WiFi ---------------------------------------------------------------*/
  SettingsWifi wifi = {};
  const cJSON *jwifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
  if (jwifi) {
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(jwifi, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(jwifi, "password");
    if (!cJSON_IsObject(jwifi) || !cJSON_IsString(ssid) || !cJSON_IsString(pass)) {
      why = "wifi debe ser {\"ssid\":\"...\",\"password\":\"...\"}";
    } else if (strlen(ssid->valuestring) == 0 ||
               strlen(ssid->valuestring) > WIFI_SSID_MAX_LEN) {
      why = "wifi.ssid debe tener entre 1 y 32 bytes (UTF-8 cuenta bytes, no letras)";
    } else if (valid_wpa2_passphrase(pass->valuestring, &why)) {
      strlcpy(wifi.ssid, ssid->valuestring, sizeof(wifi.ssid));
      strlcpy(wifi.password, pass->valuestring, sizeof(wifi.password));
    }
  } else if (!settings_has_wifi()) {
    why = "falta \"wifi\": la placa no tiene red guardada";
  }

  /* --- Credencial de la camara ----------------------------------------------
   * Los tres campos juntos: van en un solo blob, y una key de una camara con
   * el token de otra daria 401 sin forma de saber cual esta mal. */
  SettingsCredential cred = {};
  const cJSON *jcam = cJSON_GetObjectItemCaseSensitive(root, "camera");
  if (!why && jcam) {
    const cJSON *id  = cJSON_GetObjectItemCaseSensitive(jcam, "id");
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(jcam, "apiKey");
    const cJSON *tok = cJSON_GetObjectItemCaseSensitive(jcam, "apiToken");
    if (!cJSON_IsObject(jcam) || cJSON_GetArraySize(jcam) != 3 || !cJSON_IsString(id) ||
        !cJSON_IsString(key) || !cJSON_IsString(tok)) {
      why = "camera debe ser {\"id\":\"CAM-001\",\"apiKey\":\"...\",\"apiToken\":\"...\"}";
    } else if (valid_camera_id(id->valuestring, &why) &&
               valid_api_secret(key->valuestring, "apiKey", &why) &&
               valid_api_secret(tok->valuestring, "apiToken", &why)) {
      strlcpy(cred.camera_code, id->valuestring, sizeof(cred.camera_code));
      strlcpy(cred.api_key, key->valuestring, sizeof(cred.api_key));
      strlcpy(cred.api_token, tok->valuestring, sizeof(cred.api_token));
    }
  } else if (!why) {
    SettingsCredential current;
    if (!settings_get_credential(current)) {
      why = "falta \"camera\": la placa no tiene credencial guardada";
    }
    memset(&current, 0, sizeof(current));
  }

  /* --- Fog ----------------------------------------------------------------*/
  SettingsFog fog;
  settings_get_fog(fog);   // lo actual (o los defectos): se cambia lo que llegue
  const cJSON *jhost = cJSON_GetObjectItemCaseSensitive(root, "fogHost");
  const cJSON *jport = cJSON_GetObjectItemCaseSensitive(root, "fogPort");
  if (!why && jhost) {
    if (!cJSON_IsString(jhost)) {
      why = "fogHost debe ser un texto";
    } else if (valid_host(jhost->valuestring, &why)) {
      strlcpy(fog.host, jhost->valuestring, sizeof(fog.host));
    }
  }
  if (!why && jport) {
    const double p = cJSON_IsNumber(jport) ? jport->valuedouble : -1;
    if (p < 1 || p > 65535 || p != (double)(long)p) {
      why = "fogPort debe ser un entero entre 1 y 65535";
    } else {
      fog.port = (uint16_t)p;
    }
  }

  /* --- Calibracion de la bateria ----------------------------------------
   * Opcional y propia de cada placa: dos puntos medidos con el monitor serie
   * (linea "BAT:") y un multimetro en la celda. */
  SettingsBatteryCal cal = {};
  const cJSON *jcal = cJSON_GetObjectItemCaseSensitive(root, "batteryCal");
  if (!why && jcal) {
    static const char *kCalKeys[] = {"adcA", "batA", "adcB", "batB"};
    uint16_t *dst[] = {&cal.adc_a_mv, &cal.bat_a_mv, &cal.adc_b_mv, &cal.bat_b_mv};
    if (!cJSON_IsObject(jcal) || cJSON_GetArraySize(jcal) != 4) {
      why = "batteryCal debe ser {\"adcA\":mV,\"batA\":mV,\"adcB\":mV,\"batB\":mV}";
    }
    for (size_t i = 0; !why && i < 4; i++) {
      const cJSON *v = cJSON_GetObjectItemCaseSensitive(jcal, kCalKeys[i]);
      const double d = cJSON_IsNumber(v) ? v->valuedouble : -1;
      if (d < 0 || d > 65535 || d != (double)(long)d) {
        why = "batteryCal: adcA, batA, adcB y batB deben ser enteros en mV";
      } else {
        *dst[i] = (uint16_t)d;
      }
    }
    if (!why) settings_battery_cal_valid(cal, &why);
  }

  if (!why && !jwifi && !jcam && !jhost && !jport && !jcal) why = "nada que guardar";
  cJSON_Delete(root);

  if (why) {
    memset(&wifi, 0, sizeof(wifi));
    memset(&cred, 0, sizeof(cred));
    PROV_REPLY("ERROR %s", why);
    return;
  }

  /* --- Escritura: la WiFi la ultima -----------------------------------------
   * La WiFi es la que marca una placa nueva como aprovisionada. Si la
   * alimentacion se corta entre escrituras, la placa sigue "sin aprovisionar"
   * y vuelve a pedir la linea, en vez de quedar con WiFi nueva y Fog o
   * credencial viejos. */
  const char *write_err = nullptr;
  if ((jhost || jport) && !settings_set_fog(fog)) {
    write_err = "el Fog";
  } else if (jcal && !settings_set_battery_cal(cal)) {
    write_err = "la calibracion de bateria";
  }
  if (write_err) {
    memset(&wifi, 0, sizeof(wifi));
    memset(&cred, 0, sizeof(cred));
    PROV_REPLY("ERROR no se pudo escribir %s en la NVS", write_err);
    return;
  }
  const bool cred_ok = !jcam || settings_set_credential(cred);
  memset(&cred, 0, sizeof(cred));   // no dejar el token en RAM
  if (!cred_ok) {
    memset(&wifi, 0, sizeof(wifi));
    PROV_REPLY("ERROR no se pudo escribir la credencial en la NVS");
    return;
  }
  const bool wifi_ok = !jwifi || settings_set_wifi(wifi);
  memset(&wifi, 0, sizeof(wifi));   // no dejar la contrasena en RAM
  if (!wifi_ok) {
    PROV_REPLY("ERROR no se pudo escribir la WiFi en la NVS");
    return;
  }

  SettingsWifi saved = {};
  settings_get_wifi(saved);
  SettingsCredential saved_cred = {};
  settings_get_credential(saved_cred);
  SettingsBatteryCal saved_cal;
  const bool cal_from_nvs = settings_get_battery_cal(saved_cal);
  /* La contrasena, la key y el token nunca salen por el serie. El SSID y el
   * id de camara si: no son secretos y es lo que hay que comprobar a ojo. */
  PROV_REPLY("OK guardado: camera=%s wifi=\"%s\" fog=%s:%u bat=%u->%u,%u->%u (%s). Reiniciando",
             saved_cred.camera_code, saved.ssid, fog.host, (unsigned)fog.port,
             (unsigned)saved_cal.adc_a_mv, (unsigned)saved_cal.bat_a_mv,
             (unsigned)saved_cal.adc_b_mv, (unsigned)saved_cal.bat_b_mv,
             cal_from_nvs ? "NVS" : "defecto");
  memset(&saved, 0, sizeof(saved));
  memset(&saved_cred, 0, sizeof(saved_cred));
  restart_now();
}

/* ---------------------------------------------------------------------------
 * Escucha
 * -------------------------------------------------------------------------*/
void provision_listen(uint32_t window_ms) {
  /* Lo que haya llegado antes de abrir la ventana (ruido del arranque) no es
   * una linea del script: este solo escribe despues de ver READY. */
  while (Serial.available()) Serial.read();

  PROV_REPLY("READY modo=%s ventana=%lu ms. Enviar una linea JSON",
             settings_is_provisioned() ? "reaprovisionar" : "aprovisionar",
             (unsigned long)window_ms);

  size_t len = 0;
  bool overflow = false;
  const uint32_t start = millis();
  while (millis() - start < window_ms) {
    while (Serial.available()) {
      const int c = Serial.read();
      /* Fin de linea: CR, LF o CRLF. El script manda LF, pero el monitor
       * serie del IDE manda solo CR por defecto. El segundo caracter de un
       * CRLF deja una linea vacia, que se ignora (len == 0). */
      if (c != '\r' && c != '\n') {
        if (len < PROV_LINE_MAX_LEN) s_line[len++] = (char)c;
        else overflow = true;
        continue;
      }
      if (overflow) {
        PROV_REPLY("ERROR linea demasiado larga (max. %u bytes)", (unsigned)PROV_LINE_MAX_LEN);
      } else if (len > 0) {
        s_line[len] = '\0';
        handle_line(len);   // solo vuelve si hubo error
      }
      memset(s_line, 0, sizeof(s_line));
      len = 0;
      overflow = false;
    }
    delay(10);   // nada que hacer entre bytes; no tiene sentido girar a 160 MHz
  }
  memset(s_line, 0, sizeof(s_line));
}
