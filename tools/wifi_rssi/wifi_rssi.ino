/* ============================================================================
 *  wifi_rssi.ino  -  Nivel de senal WiFi (RSSI) de la ESP32-CAM
 *
 *  Herramienta de banco/campo, NO es el firmware de campo: no hay deep sleep
 *  ni camara. La placa se conecta a la red como estacion (igual que el
 *  firmware principal) y mide el RSSI del AP cada RSSI_SAMPLE_MS. Lo muestra:
 *    - por el monitor serie, una linea por segundo;
 *    - en http://<IP de la placa>/ (movil en la misma red), para ir moviendo
 *      la camara o la antena y ver el valor sin cable;
 *    - con el LED rojo (GPIO33): N destellos cada 2 s = N barras (0..4).
 *
 *  Pensado para comparar antena PCB vs. externa (IPEX): enviar "r" por el
 *  serie (o pulsar "Reiniciar" en la web) pone a cero min/max/media y asi
 *  cada posicion o antena tiene su propia estadistica.
 *
 *  Ojo: el RSSI es lo que la placa RECIBE del AP (enlace de bajada). No
 *  depende de la potencia de TX de la placa, pero si de la antena, porque la
 *  misma antena se usa para emitir y recibir.
 *
 *  WiFi: la lee de la NVS (solo lectura) si la placa se aprovisiono con el
 *  firmware principal. Si no, la pide por el serie y NO la guarda. No hay
 *  credenciales en el binario.
 *
 *  Copia a mano las claves/blobs de src/settings/settings_module.*. Si
 *  cambian alli, cambiarlas aqui.
 *
 *  Compilar con el MISMO esquema de particiones que el firmware principal,
 *  para no mover la NVS:
 *    arduino-cli compile --fqbn esp32:esp32:esp32cam:PartitionScheme=min_spiffs --build-path build .
 * ==========================================================================*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <driver/rtc_io.h>
#include <esp_system.h>

/* --- Ajustes de la herramienta -------------------------------------------- */
#define RSSI_SAMPLE_MS         200     // el driver actualiza el RSSI con cada baliza (~102 ms)
#define RSSI_REPORT_MS         1000    // una linea por segundo en el serie
#define RSSI_CONNECT_TIMEOUT_MS 20000
#define RSSI_SERIAL_BAUD       115200
#define RSSI_PROMPT_TIMEOUT_MS 120000  // espera de SSID/clave por serie si la NVS no tiene WiFi
/* Misma potencia que el firmware de campo (WIFI_TX_POWER_DBM de config.h):
 * asi el pico de corriente y el comportamiento del enlace son los reales. */
#define RSSI_TX_POWER_DBM      17

/* Umbrales de las barras, dBm. Referencia practica para el ESP32 a 802.11n:
 * por encima de -67 el enlace va sobrado; entre -67 y -75 funciona con
 * reintentos ocasionales; por debajo de -80 la subida de una foto de
 * ~30-60 KB empieza a fallar o a tardar varios segundos (mas tiempo
 * despierto = mas bateria). */
#define RSSI_BAR4_DBM          -60
#define RSSI_BAR3_DBM          -67
#define RSSI_BAR2_DBM          -75
#define RSSI_BAR1_DBM          -82

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

/* Estadistica desde el ultimo reinicio ("r"). Suma en int64 por si se deja
 * horas: 18 000 muestras/h * 100 no desborda, pero no cuesta nada. */
static int       s_last = 0;
static int       s_min = 0, s_max = -127;
static int64_t   s_sum = 0;
static uint32_t  s_count = 0;
static uint32_t  s_drops = 0;          // desconexiones desde el ultimo reinicio
static uint32_t  s_stats_since_ms = 0;

/* ========================================================================== */

static void led_set(bool on) { digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH); }

static int bars_for(int rssi) {
  if (rssi >= RSSI_BAR4_DBM) return 4;
  if (rssi >= RSSI_BAR3_DBM) return 3;
  if (rssi >= RSSI_BAR2_DBM) return 2;
  if (rssi >= RSSI_BAR1_DBM) return 1;
  return 0;
}

static const char *label_for(int bars) {
  static const char *kLabels[] = { "muy mala", "mala", "justa", "buena", "excelente" };
  return kLabels[bars];
}

static void stats_reset() {
  s_min = 0; s_max = -127; s_sum = 0; s_count = 0; s_drops = 0;
  s_stats_since_ms = millis();
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

/* Lee una linea del serie (sin eco de la contrasena: el monitor ya la
 * muestra al escribirla, no hace falta repetirla en el log). */
static bool read_line(char *out, size_t cap, uint32_t timeout_ms) {
  size_t n = 0;
  const uint32_t t0 = millis();
  while (millis() - t0 < timeout_ms) {
    while (Serial.available()) {
      const char c = (char)Serial.read();
      /* Fin de linea: CR, LF o CRLF, segun como este el monitor (el del IDE
       * por defecto manda solo CR). El segundo caracter de un CRLF llega
       * como linea vacia y se salta. */
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
  if (!read_line(s_wifi.ssid, sizeof(s_wifi.ssid), RSSI_PROMPT_TIMEOUT_MS)) return false;
  Serial.println("WIFI: ahora la contrasena y Enter");
  if (!read_line(s_wifi.password, sizeof(s_wifi.password), RSSI_PROMPT_TIMEOUT_MS)) return false;
  return true;
}

/* Lista las redes con ese SSID: si hay varios AP (repetidor, malla), dice a
 * cual se va a conectar y con que senal llega cada uno. */
static void scan_target() {
  Serial.println("SCAN: buscando redes...");
  const int n = WiFi.scanNetworks(false, true);
  int found = 0;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != s_wifi.ssid) continue;
    Serial.printf("SCAN: \"%s\" BSSID %s canal %d RSSI %d dBm\n",
                  s_wifi.ssid, WiFi.BSSIDstr(i).c_str(), (int)WiFi.channel(i),
                  (int)WiFi.RSSI(i));
    found++;
  }
  if (found == 0) {
    Serial.printf("SCAN: \"%s\" no aparece (%d redes visibles). Si es de 5 GHz, el ESP32 no la ve: solo 2,4 GHz\n",
                  s_wifi.ssid, n);
  }
  WiFi.scanDelete();
}

static bool connect_wifi() {
  WiFi.begin(s_wifi.ssid, s_wifi.password);
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < RSSI_CONNECT_TIMEOUT_MS) {
    led_set((millis() / 100) % 2);      // parpadeo rapido mientras conecta
    delay(50);
  }
  led_set(false);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("WIFI: sin conexion tras %lu ms (status %d)\n",
                  (unsigned long)RSSI_CONNECT_TIMEOUT_MS, (int)WiFi.status());
    return false;
  }
  Serial.printf("WIFI: conectado en %lu ms, IP %s, BSSID %s, canal %d\n",
                (unsigned long)(millis() - t0), WiFi.localIP().toString().c_str(),
                WiFi.BSSIDstr().c_str(), (int)WiFi.channel());
  Serial.printf("WEB: abre http://%s/ desde un movil en la misma red\n",
                WiFi.localIP().toString().c_str());
  return true;
}

/* --- Web -------------------------------------------------------------------*/
static const char kPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RSSI ESP32-CAM</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee;text-align:center}
#v{font-size:5rem;font-weight:700;margin:.2em 0}
#l{font-size:1.4rem}
.bars{display:flex;gap:6px;justify-content:center;align-items:flex-end;height:60px;margin:16px 0}
.bars i{width:22px;background:#333;border-radius:3px}
table{margin:16px auto;border-collapse:collapse}td{padding:4px 10px;text-align:left}
button{font-size:1.1rem;padding:10px 20px;margin-top:8px}
</style></head><body>
<div id="v">--</div><div id="l">conectando...</div>
<div class="bars"><i style="height:25%"></i><i style="height:50%"></i><i style="height:75%"></i><i style="height:100%"></i></div>
<table>
<tr><td>Media</td><td id="avg"></td></tr><tr><td>Min / Max</td><td id="mm"></td></tr>
<tr><td>Muestras</td><td id="n"></td></tr><tr><td>Desconexiones</td><td id="d"></td></tr>
<tr><td>Canal / BSSID</td><td id="ch"></td></tr>
</table>
<button onclick="fetch('/reset',{method:'POST'})">Reiniciar estadistica</button>
<script>
const C=['#d33','#e80','#db0','#7c3','#2b5'];
async function tick(){
 try{
  const r=await (await fetch('/rssi',{cache:'no-store'})).json();
  v.textContent=r.rssi+' dBm';l.textContent=r.label;
  document.querySelectorAll('.bars i').forEach((e,i)=>e.style.background=i<r.bars?C[r.bars]:'#333');
  avg.textContent=r.avg.toFixed(1)+' dBm';mm.textContent=r.min+' / '+r.max+' dBm';
  n.textContent=r.n+' ('+r.secs+' s)';d.textContent=r.drops;ch.textContent=r.ch+' / '+r.bssid;
 }catch(e){l.textContent='sin respuesta';}
}
setInterval(tick,1000);tick();
</script></body></html>)HTML";

static void handle_root() { s_server.send_P(200, "text/html", kPage); }

static void handle_rssi() {
  char buf[256];
  const int bars = bars_for(s_last);
  snprintf(buf, sizeof(buf),
           "{\"rssi\":%d,\"bars\":%d,\"label\":\"%s\",\"avg\":%.2f,\"min\":%d,\"max\":%d,"
           "\"n\":%lu,\"secs\":%lu,\"drops\":%lu,\"ch\":%d,\"bssid\":\"%s\"}",
           s_last, bars, label_for(bars),
           s_count ? (double)s_sum / s_count : 0.0, s_count ? s_min : 0, s_count ? s_max : 0,
           (unsigned long)s_count, (unsigned long)((millis() - s_stats_since_ms) / 1000),
           (unsigned long)s_drops, (int)WiFi.channel(), WiFi.BSSIDstr().c_str());
  s_server.send(200, "application/json", buf);
}

static void handle_reset() {
  stats_reset();
  Serial.println("STATS: reiniciada (web)");
  s_server.send(204);
}

/* --- LED: N destellos cada 2 s = N barras --------------------------------- */
static void led_update() {
  const uint32_t phase = millis() % 2000;
  const int bars = WiFi.status() == WL_CONNECTED ? bars_for(s_last) : 0;
  /* Cada destello: 120 ms encendido + 180 ms apagado. */
  const uint32_t slot = phase / 300;
  led_set(slot < (uint32_t)bars && (phase % 300) < 120);
}

/* ========================================================================== */

void setup() {
  Serial.begin(RSSI_SERIAL_BAUD);
  delay(200);
  Serial.println("\n=== wifi_rssi (herramienta de medida) ===");

  /* El firmware principal duerme con PWDN en alto y retenido: se suelta y se
   * deja en alto para que el OV2640 no consuma ni meta ruido en el rail. */
  rtc_gpio_hold_dis((gpio_num_t)CAM_PIN_PWDN);
  pinMode(CAM_PIN_PWDN, OUTPUT);
  digitalWrite(CAM_PIN_PWDN, HIGH);
  pinMode(CAM_PIN_FLASH_LED, OUTPUT);
  digitalWrite(CAM_PIN_FLASH_LED, LOW);
  pinMode(STATUS_LED_PIN, OUTPUT);
  led_set(false);

  const esp_reset_reason_t why = esp_reset_reason();
  Serial.printf("Reset reason: %d%s\n", (int)why,
                why == ESP_RST_BROWNOUT ? " (BROWNOUT: la alimentacion cayo)" : "");

  if (load_wifi_from_nvs()) {
    Serial.printf("NVS: WiFi \"%s\" (de la NVS, solo lectura)\n", s_wifi.ssid);
  } else if (!prompt_wifi()) {
    Serial.println("WIFI: no se recibio la red. Reinicia la placa para volver a intentarlo");
    while (true) { led_set((millis() / 100) % 2); delay(50); }
  }

  WiFi.persistent(false);              // el driver no guarda copia de la clave
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                // sin modem sleep: RSSI de cada baliza
  WiFi.setTxPower((wifi_power_t)(RSSI_TX_POWER_DBM * 4));
  scan_target();

  while (!connect_wifi()) delay(2000);

  s_server.on("/", HTTP_GET, handle_root);
  s_server.on("/rssi", HTTP_GET, handle_rssi);
  s_server.on("/reset", HTTP_POST, handle_reset);
  s_server.begin();

  stats_reset();
  Serial.println("Envia \"r\" para reiniciar min/max/media (p. ej. al cambiar de antena)");
}

void loop() {
  static uint32_t last_sample = 0, last_report = 0;
  static bool was_connected = true;
  const uint32_t now = millis();

  s_server.handleClient();
  led_update();

  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == 'r' || c == 'R') { stats_reset(); Serial.println("STATS: reiniciada"); }
  }

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (!connected) {
    if (was_connected) {
      s_drops++;
      Serial.println("WIFI: desconectada, reintentando...");
    }
    was_connected = false;
    if (now - last_report >= RSSI_CONNECT_TIMEOUT_MS) {
      last_report = now;
      WiFi.reconnect();
    }
    return;
  }
  if (!was_connected) {
    Serial.printf("WIFI: reconectada, IP %s\n", WiFi.localIP().toString().c_str());
    was_connected = true;
  }

  if (now - last_sample >= RSSI_SAMPLE_MS) {
    last_sample = now;
    const int rssi = (int)WiFi.RSSI();
    if (rssi < 0) {                    // 0 = el driver aun no tiene dato
      s_last = rssi;
      if (rssi < s_min || s_count == 0) s_min = rssi;
      if (rssi > s_max || s_count == 0) s_max = rssi;
      s_sum += rssi;
      s_count++;
    }
  }

  if (now - last_report >= RSSI_REPORT_MS && s_count > 0) {
    last_report = now;
    const int bars = bars_for(s_last);
    char bar[5] = "    ";
    for (int i = 0; i < bars; i++) bar[i] = '#';
    Serial.printf("RSSI: %4d dBm [%s] %-9s | media %6.1f  min %4d  max %4d  n=%lu  desc=%lu\n",
                  s_last, bar, label_for(bars), (double)s_sum / s_count, s_min, s_max,
                  (unsigned long)s_count, (unsigned long)s_drops);
  }
}
