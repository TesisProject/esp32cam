/* ============================================================================
 *  ota_test.ino  -  Banco de pruebas del OTA y del rollback de la ESP32-CAM
 *
 *  Herramienta de banco, NO es el firmware de campo y NO es el cliente OTA
 *  (E5): sirve para comprobar, antes de que exista E5, que la parte del OTA
 *  que ya esta en el firmware funciona en la placa real:
 *    - que el .bin cabe en una ranura de Minimal SPIFFS y arranca desde la
 *      otra (app0 <-> app1);
 *    - que el firmware principal solo confirma la imagen tras una subida 2xx
 *      (ota_confirm_if_pending) y que, si no, el bootloader vuelve solo a la
 *      imagen anterior al despertar del deep sleep;
 *    - que la NVS (WiFi, Fog, credencial) sobrevive al cambio de imagen.
 *
 *  Como: se conecta a la red como estacion (WiFi de la NVS, solo lectura) y
 *  sirve en http://<IP>/ un formulario para subir el .bin; tambien acepta
 *  curl -F "firmware=@build/esp32cam.ino.bin" http://<IP>/update
 *  Escribe la imagen con Update (core ESP32) en la ranura libre, que valida
 *  el byte magico y el SHA-256 de la imagen al terminar, y reinicia.
 *
 *  Esta herramienta es la "version anterior": si el firmware nuevo no llega a
 *  confirmarse, al despertar el bootloader vuelve aqui y la pagina (y el
 *  serie) dicen que hubo rollback.
 *
 *  SIN autenticacion: cualquiera en la LAN puede flashear la placa mientras
 *  esto corre. Solo en el banco, nunca en una camara desatendida.
 *
 *  No escribe la NVS. Copia a mano las claves/blobs de
 *  src/settings/settings_module.*: si cambian alli, cambiarlas aqui.
 *
 *  Compilar con el MISMO esquema de particiones que el firmware principal
 *  (con otro, la tabla de particiones no tendria app1 y moveria la NVS):
 *    arduino-cli compile --fqbn esp32:esp32:esp32cam:PartitionScheme=min_spiffs --build-path build .
 * ==========================================================================*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <driver/rtc_io.h>
#include <esp_ota_ops.h>
#include <esp_app_desc.h>
#include <esp_system.h>

/* --- Ajustes de la herramienta -------------------------------------------- */
#define OTA_SERIAL_BAUD          115200
#define OTA_CONNECT_TIMEOUT_MS   20000
#define OTA_PROMPT_TIMEOUT_MS    120000   // espera de SSID/clave por serie si la NVS no tiene WiFi
#define OTA_PROGRESS_STEP        (64 * 1024)
#define OTA_RESTART_DELAY_MS     1500     // deja salir la respuesta HTTP antes de reiniciar

/* --- Copiado de config.h del firmware principal (mantener igual) --------- */
#define WIFI_SSID_MAX_LEN      32
#define WIFI_PASS_MAX_LEN      63
#define CAM_PIN_PWDN           32
#define CAM_PIN_FLASH_LED       4
#define STATUS_LED_PIN         33      // LED rojo pequeno, LOW enciende

/* --- Copiado de settings_module (forma del blob en la NVS) --------------- */
#define SETTINGS_NVS_NAMESPACE  "parkvision"
#define SETTINGS_SCHEMA_VERSION 1
struct NvsWifi {
  char ssid[WIFI_SSID_MAX_LEN + 1];
  char password[WIFI_PASS_MAX_LEN + 1];
};

/* --- Estado ----------------------------------------------------------------*/
static NvsWifi   s_wifi;
static WebServer s_server(80);
static size_t    s_written    = 0;
static size_t    s_next_log   = 0;
static uint32_t  s_upload_t0  = 0;
static bool      s_upload_ok  = false;
static String    s_upload_err;

/* ========================================================================== */

static void led_set(bool on) { digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH); }

static const char *state_name(esp_ota_img_states_t s) {
  switch (s) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED (flasheo USB)";
    default:                         return "?";
  }
}

/* Una linea por ranura: estado OTA y que imagen tiene (nombre del sketch y
 * fecha de compilacion, del esp_app_desc_t que lleva cada .bin). */
static String slot_info(const esp_partition_t *p) {
  if (!p) return "?";
  String s = String(p->label) + ": ";
  esp_ota_img_states_t st;
  s += esp_ota_get_state_partition(p, &st) == ESP_OK ? state_name(st) : "sin otadata";
  esp_app_desc_t d;
  if (esp_ota_get_partition_description(p, &d) == ESP_OK) {
    s += String(" | ") + d.project_name + " compilado " + d.date + " " + d.time;
  } else {
    s += " | sin imagen valida";
  }
  return s;
}

static void log_partitions() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next    = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
  Serial.printf("OTA: corriendo en %s\n", running ? running->label : "?");
  Serial.printf("OTA:   %s\n", slot_info(running).c_str());
  Serial.printf("OTA:   %s  <- aqui se escribira el .bin\n", slot_info(next).c_str());
  if (invalid) {
    Serial.printf("OTA: ROLLBACK: la imagen de %s se descarto (no se confirmo) y se volvio a esta\n",
                  invalid->label);
  }
}

/* Solo lectura: esta herramienta nunca escribe ni borra la NVS. */
static bool load_wifi_from_nvs() {
  Preferences p;
  if (!p.begin(SETTINGS_NVS_NAMESPACE, true)) {
    Serial.println("NVS: sin namespace (placa sin aprovisionar)");
    return false;
  }
  const uint8_t schema = p.getUChar("schema", 0);
  bool ok = false;
  if (schema != SETTINGS_SCHEMA_VERSION) {
    Serial.printf("NVS: esquema %u, esta herramienta entiende el %u\n",
                  schema, SETTINGS_SCHEMA_VERSION);
  } else if (p.getBytesLength("wifi") == sizeof(NvsWifi) &&
             p.getBytes("wifi", &s_wifi, sizeof(NvsWifi)) == sizeof(NvsWifi)) {
    s_wifi.ssid[WIFI_SSID_MAX_LEN] = '\0';
    s_wifi.password[WIFI_PASS_MAX_LEN] = '\0';
    ok = s_wifi.ssid[0] != '\0';
  }
  p.end();
  return ok;
}

static bool read_line(char *out, size_t cap, uint32_t timeout_ms) {
  size_t n = 0;
  const uint32_t t0 = millis();
  while (millis() - t0 < timeout_ms) {
    while (Serial.available()) {
      const char c = (char)Serial.read();
      if (c == '\r' || c == '\n') {
        if (n == 0) continue;
        out[n] = '\0';
        return true;
      }
      if (n < cap - 1) out[n++] = c;
    }
    delay(10);
  }
  return false;
}

static bool prompt_wifi() {
  Serial.println("WIFI: escribe el SSID y pulsa Enter (no se guarda en la placa)");
  if (!read_line(s_wifi.ssid, sizeof(s_wifi.ssid), OTA_PROMPT_TIMEOUT_MS)) return false;
  Serial.println("WIFI: ahora la contrasena y Enter");
  if (!read_line(s_wifi.password, sizeof(s_wifi.password), OTA_PROMPT_TIMEOUT_MS)) return false;
  return true;
}

static bool connect_wifi() {
  WiFi.begin(s_wifi.ssid, s_wifi.password);
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < OTA_CONNECT_TIMEOUT_MS) {
    led_set((millis() / 100) % 2);
    delay(50);
  }
  led_set(false);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("WIFI: sin conexion tras %lu ms (status %d)\n",
                  (unsigned long)OTA_CONNECT_TIMEOUT_MS, (int)WiFi.status());
    return false;
  }
  Serial.printf("WIFI: conectado, IP %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("WEB: abre http://%s/ o usa:\n", WiFi.localIP().toString().c_str());
  Serial.printf("     curl -F \"firmware=@build/esp32cam.ino.bin\" http://%s/update\n",
                WiFi.localIP().toString().c_str());
  return true;
}

/* --- Web -------------------------------------------------------------------*/
static const char kPageHead[] PROGMEM = R"HTML(<!doctype html>
<html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OTA ESP32-CAM</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee;max-width:720px}
pre{background:#222;padding:12px;border-radius:6px;white-space:pre-wrap;word-break:break-word}
.warn{color:#fb4}.bad{color:#f66}button,input{font-size:1rem;margin:8px 0}
</style></head><body>
<h1>Banco de OTA</h1>
<p class="warn">Sin autenticacion: solo para el banco.</p>
)HTML";

static const char kPageForm[] PROGMEM = R"HTML(
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="firmware" accept=".bin"><br>
<button type="submit">Subir y reiniciar</button>
</form>
<p>Usa <code>build/esp32cam.ino.bin</code>, no el <code>.merged.bin</code>
(ese lleva bootloader y particiones y no cabe en una ranura).</p>
</body></html>)HTML";

static void handle_root() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next    = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();

  String page = FPSTR(kPageHead);
  page += "<pre>Corriendo en " + String(running ? running->label : "?") + "\n";
  page += slot_info(running) + "\n" + slot_info(next) + "  &lt;- destino del .bin\n";
  page += "Ranura de " + String(next ? next->size : 0) + " bytes</pre>";
  if (invalid) {
    page += "<p class=\"bad\">ROLLBACK: la imagen de " + String(invalid->label) +
            " no se confirmo y el bootloader volvio a esta.</p>";
  }
  page += FPSTR(kPageForm);
  s_server.send(200, "text/html", page);
}

/* Fin de la peticion POST /update: el cuerpo ya se proceso en handle_upload. */
static void handle_update_done() {
  if (!s_upload_ok) {
    s_server.send(500, "text/plain", "ERROR: " + s_upload_err + "\n");
    return;
  }
  s_server.send(200, "text/plain",
                "OK: " + String((unsigned)s_written) + " bytes escritos, reiniciando en la imagen nueva\n");
  Serial.println("OTA: reiniciando en la imagen nueva (estado NEW -> PENDING_VERIFY)");
  Serial.flush();
  delay(OTA_RESTART_DELAY_MS);
  ESP.restart();
}

/* Trozos del multipart a medida que llegan: se escriben directamente en la
 * ranura libre, sin guardar el .bin entero en RAM. */
static void handle_upload() {
  HTTPUpload &up = s_server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      s_written = 0; s_next_log = OTA_PROGRESS_STEP; s_upload_t0 = millis();
      s_upload_ok = false; s_upload_err = "";
      Serial.printf("OTA: recibiendo %s\n", up.filename.c_str());
      /* UPDATE_SIZE_UNKNOWN: el limite es la ranura entera. Update escribe
       * en esp_ota_get_next_update_partition(), nunca en la que corre. */
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) s_upload_err = Update.errorString();
      break;

    case UPLOAD_FILE_WRITE:
      if (!s_upload_err.isEmpty()) break;
      led_set((s_written / 4096) % 2);
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        /* El primer trozo falla aqui si no es una imagen de app (byte
         * magico 0xE9 distinto, p. ej. el .merged.bin). */
        s_upload_err = Update.errorString();
        Update.abort();
        break;
      }
      s_written += up.currentSize;
      if (s_written >= s_next_log) {
        Serial.printf("OTA: %u KB\n", (unsigned)(s_written / 1024));
        s_next_log += OTA_PROGRESS_STEP;
      }
      break;

    case UPLOAD_FILE_END:
      led_set(false);
      if (!s_upload_err.isEmpty()) break;
      /* end(true): comprueba el SHA-256 de la imagen y la marca como la que
       * arrancara en el proximo reinicio. */
      if (Update.end(true)) {
        s_upload_ok = true;
        Serial.printf("OTA: %u bytes escritos y verificados en %lu ms\n",
                      (unsigned)s_written, (unsigned long)(millis() - s_upload_t0));
      } else {
        s_upload_err = Update.errorString();
      }
      break;

    case UPLOAD_FILE_ABORTED:
      led_set(false);
      Update.abort();
      s_upload_err = "subida cortada";
      break;
  }
  if (!s_upload_err.isEmpty() && up.status != UPLOAD_FILE_WRITE) {
    Serial.printf("OTA: ERROR %s\n", s_upload_err.c_str());
  }
}

/* ========================================================================== */

void setup() {
  Serial.begin(OTA_SERIAL_BAUD);
  delay(200);
  Serial.println("\n=== ota_test (banco de OTA, sin autenticacion) ===");

  /* El firmware principal duerme con PWDN en alto y retenido: se suelta y se
   * deja en alto para que el OV2640 no consuma mientras esto espera. */
  rtc_gpio_hold_dis((gpio_num_t)CAM_PIN_PWDN);
  pinMode(CAM_PIN_PWDN, OUTPUT);
  digitalWrite(CAM_PIN_PWDN, HIGH);
  pinMode(CAM_PIN_FLASH_LED, OUTPUT);
  digitalWrite(CAM_PIN_FLASH_LED, LOW);
  pinMode(STATUS_LED_PIN, OUTPUT);
  led_set(false);

  Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());
  log_partitions();

  if (load_wifi_from_nvs()) {
    Serial.printf("NVS: WiFi \"%s\" (de la NVS, solo lectura)\n", s_wifi.ssid);
  } else if (!prompt_wifi()) {
    Serial.println("WIFI: no se recibio la red. Reinicia la placa para volver a intentarlo");
    while (true) { led_set((millis() / 100) % 2); delay(50); }
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // sin modem sleep: la subida del .bin va mas rapida
  while (!connect_wifi()) delay(2000);

  s_server.on("/", HTTP_GET, handle_root);
  s_server.on("/update", HTTP_POST, handle_update_done, handle_upload);
  s_server.begin();
}

void loop() {
  s_server.handleClient();
  if (WiFi.status() != WL_CONNECTED) {
    static uint32_t last = 0;
    if (millis() - last > OTA_CONNECT_TIMEOUT_MS) {
      last = millis();
      Serial.println("WIFI: desconectada, reintentando...");
      WiFi.reconnect();
    }
  }
}
