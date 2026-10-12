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

/* ---------------------------------------------------------------------------
 * Procesado de una linea. Solo retorna si hay error (con exito reinicia).
 * -------------------------------------------------------------------------*/
static void restart_now() {
  Serial.flush();
  delay(50);
  ESP.restart();
}

static void handle_line(size_t len, bool wifi_required) {
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
    if (strcmp(it->string, "wifi") != 0 && strcmp(it->string, "fogHost") != 0 &&
        strcmp(it->string, "fogPort") != 0 && strcmp(it->string, "cmd") != 0) {
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
        PROV_REPLY("OK NVS borrada, reiniciando (pedira aprovisionar)");
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
  } else if (wifi_required) {
    why = "falta \"wifi\": la placa no tiene red guardada";
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

  if (!why && !jwifi && !jhost && !jport) why = "nada que guardar";
  cJSON_Delete(root);

  if (why) {
    PROV_REPLY("ERROR %s", why);
    return;
  }

  /* --- Escritura: Fog antes que WiFi --------------------------------------
   * La WiFi es la que marca la placa como aprovisionada. Si la alimentacion
   * se corta entre las dos escrituras, la placa sigue "sin aprovisionar" y
   * vuelve a pedir la linea, en vez de quedar con WiFi nueva y Fog viejo. */
  if ((jhost || jport) && !settings_set_fog(fog)) {
    PROV_REPLY("ERROR no se pudo escribir el Fog en la NVS");
    return;
  }
  if (jwifi && !settings_set_wifi(wifi)) {
    PROV_REPLY("ERROR no se pudo escribir la WiFi en la NVS");
    return;
  }
  memset(&wifi, 0, sizeof(wifi));   // no dejar la contrasena en RAM

  SettingsWifi saved = {};
  settings_get_wifi(saved);
  /* La contrasena nunca sale por el serie. El SSID si: no es secreto y es lo
   * que hay que comprobar a ojo. */
  PROV_REPLY("OK guardado: wifi=\"%s\" fog=%s:%u. Reiniciando",
             saved.ssid, fog.host, (unsigned)fog.port);
  memset(&saved, 0, sizeof(saved));
  restart_now();
}

/* ---------------------------------------------------------------------------
 * Escucha
 * -------------------------------------------------------------------------*/
void provision_listen(uint32_t window_ms, bool wifi_required) {
  /* Lo que haya llegado antes de abrir la ventana (ruido del arranque) no es
   * una linea del script: este solo escribe despues de ver READY. */
  while (Serial.available()) Serial.read();

  PROV_REPLY("READY modo=%s ventana=%lu ms. Enviar una linea JSON",
             wifi_required ? "aprovisionar" : "reaprovisionar",
             (unsigned long)window_ms);

  size_t len = 0;
  bool overflow = false;
  const uint32_t start = millis();
  while (millis() - start < window_ms) {
    while (Serial.available()) {
      const int c = Serial.read();
      if (c == '\r') continue;
      if (c != '\n') {
        if (len < PROV_LINE_MAX_LEN) s_line[len++] = (char)c;
        else overflow = true;
        continue;
      }
      if (overflow) {
        PROV_REPLY("ERROR linea demasiado larga (max. %u bytes)", (unsigned)PROV_LINE_MAX_LEN);
      } else if (len > 0) {
        s_line[len] = '\0';
        handle_line(len, wifi_required);   // solo vuelve si hubo error
      }
      memset(s_line, 0, sizeof(s_line));
      len = 0;
      overflow = false;
    }
    delay(10);   // nada que hacer entre bytes; no tiene sentido girar a 160 MHz
  }
  memset(s_line, 0, sizeof(s_line));
}
