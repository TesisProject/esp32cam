/* ============================================================================
 *  battery_monitor.ino  -  Pagina web con la bateria de la ESP32-CAM
 *
 *  Herramienta de banco, NO es el firmware de campo: no hay deep sleep. La
 *  placa crea su propia red WiFi (access point MON_AP_SSID) y sirve en
 *  http://192.168.4.1/ una pagina con la tension, el % y la curva de
 *  descarga. No necesita router ni aprovisionamiento.
 *
 *  El problema de fondo: GPIO13 es ADC2 y el ADC2 no se puede leer con el
 *  WiFi encendido. Por eso cada MON_INTERVAL_MS la placa APAGA el AP, espera
 *  MON_SETTLE_MS, mide y lo vuelve a levantar (~1 s). El movil se desconecta
 *  y se reconecta solo; la pagina sigue preguntando y se salta el hueco.
 *
 *  Calibracion: si la placa se aprovisiono con el firmware principal, lee de
 *  la NVS (solo lectura) la calibracion "batteryCal"; si no, usa los
 *  defectos. Nunca escribe la NVS.
 *
 *  Copia a mano (no se pueden incluir ficheros de fuera del sketch con el IDE)
 *  la recta y la curva de src/battery/battery_module.cpp y las claves/blobs
 *  de src/settings/settings_module.*. Si cambian alli, cambiarlas aqui.
 *
 *  Compilar con el MISMO esquema de particiones que el firmware principal,
 *  para no mover la NVS:
 *    arduino-cli compile --fqbn esp32:esp32:esp32cam:PartitionScheme=min_spiffs --build-path build .
 * ==========================================================================*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <driver/rtc_io.h>
#include <esp_system.h>

/* --- Ajustes de la herramienta -------------------------------------------- */
#define MON_AP_SSID            "ParkVision-BAT"
#define MON_AP_PASS            "parkvision"   // WPA2, >= 8 caracteres. No es un secreto del proyecto
#define MON_AP_CHANNEL         6
/* Potencia de TX baja: es una red de banco a pocos metros, y el pico de
 * corriente de la radio (~300-500 mA a 5 V con 19,5 dBm) es lo que tumba la
 * alimentacion con la celda + MT3608 + protoboard (reinicio por brownout). */
#define MON_AP_TX_POWER        WIFI_POWER_8_5dBm
#define MON_INTERVAL_MS        30000   // cada cuanto se mide (cada medida apaga el AP ~1 s)
#define MON_SETTLE_MS          500     // tras apagar el WiFi: la celda recupera la caida de la carga
#define MON_HISTORY_LEN        720     // 6 h a 30 s; ~8,6 KB de RAM
#define MON_SERIAL_BAUD        115200

/* --- Copiado de config.h del firmware principal (mantener igual) --------- */
#define BATTERY_ADC_PIN              13
#define BATTERY_ADC_SAMPLES          32
#define BATTERY_ADC_SAMPLE_GAP_MS    2
#define BATTERY_CAL_ADC_A_MV_DEFAULT 1500
#define BATTERY_CAL_BAT_A_MV_DEFAULT 3000
#define BATTERY_CAL_ADC_B_MV_DEFAULT 2100
#define BATTERY_CAL_BAT_B_MV_DEFAULT 4200
#define BATTERY_CAL_MIN_BAT_SPAN_MV  300
#define BATTERY_CAL_MIN_ADC_SPAN_MV  30
#define BATTERY_CAL_ADC_MAX_MV       3300
#define BATTERY_VALID_MIN_MV         2500
#define BATTERY_VALID_MAX_MV         4500
#define BATTERY_CUTOFF_MV            3300
static const int32_t kCurveMv[]  = { 3000, 3300, 3500, 3650, 3750, 3850, 4000, 4200 };
static const int     kCurvePct[] = {    0,    3,   12,   30,   45,   60,   80,  100 };
static const size_t  kCurveLen   = sizeof(kCurveMv) / sizeof(kCurveMv[0]);

#define CAM_PIN_PWDN           32
#define CAM_PIN_FLASH_LED       4
#define STATUS_LED_PIN         33      // LED rojo pequeno, LOW enciende

/* --- Copiado de settings_module (forma del blob en la NVS) --------------- */
#define SETTINGS_NVS_NAMESPACE  "parkvision"
#define SETTINGS_SCHEMA_VERSION 1
struct NvsBatCal { uint16_t adc_a_mv, bat_a_mv, adc_b_mv, bat_b_mv; };

/* --- Estado ----------------------------------------------------------------*/
struct Sample {
  uint32_t t_s;        // segundos desde el arranque
  uint16_t adc_mv;
  int16_t  bat_mv;
  int8_t   pct;
  bool     valid;
};

static Sample    s_hist[MON_HISTORY_LEN];
static size_t    s_hist_count = 0;     // muestras guardadas (<= MON_HISTORY_LEN)
static size_t    s_hist_next  = 0;     // siguiente hueco del anillo
static Sample    s_last       = {};
static uint32_t  s_last_fails = 0;     // lecturas del ADC2 que fallaron en la ultima medida
static uint32_t  s_last_ms    = 0;

static NvsBatCal s_cal        = { BATTERY_CAL_ADC_A_MV_DEFAULT, BATTERY_CAL_BAT_A_MV_DEFAULT,
                                  BATTERY_CAL_ADC_B_MV_DEFAULT, BATTERY_CAL_BAT_B_MV_DEFAULT };
static bool      s_cal_nvs    = false;
static bool      s_ap_ok      = false;

static WebServer s_server(80);
static DNSServer s_dns;               // todo nombre -> 192.168.4.1 (el movil abre la pagina solo)

/* ========================================================================== */

/* Mismo criterio que settings_battery_cal_valid() del firmware principal. */
static bool cal_valid(const NvsBatCal &c) {
  return c.adc_a_mv <= BATTERY_CAL_ADC_MAX_MV && c.adc_b_mv <= BATTERY_CAL_ADC_MAX_MV &&
         c.bat_a_mv >= BATTERY_VALID_MIN_MV && c.bat_a_mv <= BATTERY_VALID_MAX_MV &&
         c.bat_b_mv >= BATTERY_VALID_MIN_MV && c.bat_b_mv <= BATTERY_VALID_MAX_MV &&
         (int)c.bat_b_mv - (int)c.bat_a_mv >= BATTERY_CAL_MIN_BAT_SPAN_MV &&
         (int)c.adc_b_mv - (int)c.adc_a_mv >= BATTERY_CAL_MIN_ADC_SPAN_MV;
}

/* Solo lectura: esta herramienta nunca escribe ni borra la NVS. */
static void load_cal() {
  Preferences p;
  if (!p.begin(SETTINGS_NVS_NAMESPACE, true)) {
    Serial.println("NVS: sin namespace (placa sin aprovisionar): cal. por defecto");
    return;
  }
  const uint8_t schema = p.getUChar("schema", 0);
  NvsBatCal cal;
  if (schema != SETTINGS_SCHEMA_VERSION) {
    Serial.printf("NVS: esquema %u, esta herramienta entiende el %u: cal. por defecto\n",
                  schema, SETTINGS_SCHEMA_VERSION);
  } else if (p.getBytesLength("bat_cal") == sizeof(NvsBatCal) &&
             p.getBytes("bat_cal", &cal, sizeof(NvsBatCal)) == sizeof(NvsBatCal) &&
             cal_valid(cal)) {
    s_cal = cal;
    s_cal_nvs = true;
  }
  p.end();
  Serial.printf("NVS: cal. %s: %u->%u, %u->%u\n", s_cal_nvs ? "NVS" : "defecto",
                s_cal.adc_a_mv, s_cal.bat_a_mv, s_cal.adc_b_mv, s_cal.bat_b_mv);
}

static int percent_from_mv(int32_t mv) {
  if (mv <= kCurveMv[0]) return kCurvePct[0];
  for (size_t i = 1; i < kCurveLen; i++) {
    if (mv < kCurveMv[i]) {
      return kCurvePct[i - 1] + (int)((int64_t)(mv - kCurveMv[i - 1]) *
             (kCurvePct[i] - kCurvePct[i - 1]) / (kCurveMv[i] - kCurveMv[i - 1]));
    }
  }
  return kCurvePct[kCurveLen - 1];
}

/* Igual que battery_read() del firmware principal. Llamar con el WiFi APAGADO. */
static void take_measurement() {
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  uint32_t sum = 0, good = 0;
  for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
    const uint32_t mv = analogReadMilliVolts(BATTERY_ADC_PIN);
    if (mv > 0) { sum += mv; good++; }   // 0 = lectura fallida (ADC2 ocupado)
    delay(BATTERY_ADC_SAMPLE_GAP_MS);
  }
  const int32_t adc = good ? (int32_t)(sum / good) : 0;
  const int32_t bat = (int32_t)s_cal.bat_a_mv +
                      (adc - (int32_t)s_cal.adc_a_mv) *
                      ((int32_t)s_cal.bat_b_mv - (int32_t)s_cal.bat_a_mv) /
                      ((int32_t)s_cal.adc_b_mv - (int32_t)s_cal.adc_a_mv);

  s_last.t_s    = millis() / 1000;
  s_last.adc_mv = (uint16_t)adc;
  s_last.bat_mv = (int16_t)constrain(bat, -32000, 32000);
  s_last.pct    = (int8_t)percent_from_mv(bat);
  s_last.valid  = good >= BATTERY_ADC_SAMPLES / 2 &&
                  bat >= BATTERY_VALID_MIN_MV && bat <= BATTERY_VALID_MAX_MV;
  s_last_fails  = BATTERY_ADC_SAMPLES - good;
  s_last_ms     = millis();

  s_hist[s_hist_next] = s_last;
  s_hist_next = (s_hist_next + 1) % MON_HISTORY_LEN;
  if (s_hist_count < MON_HISTORY_LEN) s_hist_count++;

  /* Mismo formato que el log del firmware principal: sirve para calibrar. */
  Serial.printf("BAT: ADC IO13: %u mV | Bateria: %.2f V | %d %% %s(cal. %s)%s\n",
                s_last.adc_mv, s_last.bat_mv / 1000.0f, s_last.pct,
                s_last.valid ? "" : "[NO VALIDA] ", s_cal_nvs ? "NVS" : "defecto",
                s_last_fails ? " (fallaron lecturas del ADC2)" : "");
}

/* --- LED de estado (GPIO33) ----------------------------------------------
 *  Fijo encendido     : midiendo (red apagada, ~0,6 s)
 *  1 destello / 2 s   : red levantada, nadie conectado
 *  2 destellos / 2 s  : red levantada, hay al menos un movil conectado
 *  Parpadeo rapido    : no se pudo levantar la red
 *  3 destellos largos al arrancar: el arranque anterior acabo en BROWNOUT
 *                      (la alimentacion cayo); si se repite en bucle, la
 *                      fuente no aguanta el pico del WiFi
 *  El flash blanco (GPIO4) no se usa: ~150 mA falsearian la lectura. */
static void led_set(bool on) { digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH); }

static void led_update() {
  const uint32_t t = millis();
  if (!s_ap_ok) { led_set((t / 100) % 2); return; }
  const uint32_t ph = t % 2000;
  const bool two = WiFi.softAPgetStationNum() > 0;
  led_set(ph < 60 || (two && ph >= 200 && ph < 260));
}

/* --- WiFi (access point propio) ------------------------------------------*/
static void ap_up() {
  WiFi.persistent(false);              // no tocar la NVS del driver
  WiFi.mode(WIFI_AP);
  s_ap_ok = WiFi.softAP(MON_AP_SSID, MON_AP_PASS, MON_AP_CHANNEL);
  if (!s_ap_ok) {
    Serial.println("WIFI: no se pudo levantar el AP");
    return;
  }
  WiFi.setTxPower(MON_AP_TX_POWER);
  s_dns.start(53, "*", WiFi.softAPIP());
  s_server.begin();
}

/* El driver WiFi tiene que soltar el ADC2: no basta con desconectar. */
static void ap_down() {
  s_server.stop();
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}

/* --- Web -------------------------------------------------------------------*/
static const char kPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bateria ESP32-CAM</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee;max-width:760px;margin:auto}
h1{font-size:1.1rem;font-weight:600;color:#aaa}
.big{font-size:4rem;font-weight:700;line-height:1}
.bar{height:22px;background:#333;border-radius:6px;overflow:hidden;margin:12px 0}
.fill{height:100%;transition:width .5s}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px}
.card{background:#1d1d1d;border-radius:8px;padding:10px}
.card b{display:block;font-size:1.3rem}
.muted{color:#888;font-size:.85rem}
svg{width:100%;height:220px;background:#1d1d1d;border-radius:8px;margin-top:12px}
a{color:#6af}
</style></head><body>
<h1>Bater&iacute;a ESP32-CAM (GPIO13)</h1>
<div class="big"><span id="pct">--</span> %</div>
<div class="bar"><div class="fill" id="fill" style="width:0"></div></div>
<div class="grid">
 <div class="card"><span class="muted">Tensi&oacute;n de la celda</span><b id="v">--</b></div>
 <div class="card"><span class="muted">ADC en GPIO13</span><b id="adc">--</b></div>
 <div class="card"><span class="muted">Lectura</span><b id="ok">--</b></div>
 <div class="card"><span class="muted">&Uacute;ltima medida</span><b id="age">--</b></div>
</div>
<svg id="chart" viewBox="0 0 600 220" preserveAspectRatio="none"></svg>
<p class="muted" id="info"></p>
<p class="muted">Cada medida apaga la red un segundo (GPIO13 es ADC2): el m&oacute;vil se
desconecta y vuelve solo. <a href="/csv">Descargar CSV</a></p>
<script>
const $=id=>document.getElementById(id);let last=null;
function color(p){return p<15?'#e44':p<40?'#eb4':'#4c6'}
async function poll(){
 try{const r=await fetch('/api',{cache:'no-store'});last=await r.json();last.at=Date.now();render();$('info').style.color=''}
 catch(e){$('info').textContent='Sin respuesta (probablemente midiendo)...';$('info').style.color='#eb4'}
}
function render(){
 if(!last)return;const d=last;
 $('pct').textContent=d.valid?d.pct:'--';
 $('fill').style.width=(d.valid?d.pct:0)+'%';$('fill').style.background=color(d.pct);
 $('v').textContent=(d.mv/1000).toFixed(2)+' V';
 $('adc').textContent=d.adc_mv+' mV';
 $('ok').textContent=d.valid?'válida':'NO válida';$('ok').style.color=d.valid?'':'#e44';
 $('info').textContent='Calibración: '+d.cal+' ('+d.cal_pts.join(', ')+') · medida cada '+d.interval_s+
  ' s · encendida hace '+fmt(d.uptime_s)+(d.fails?' · '+d.fails+' lecturas ADC fallidas':'');
}
function fmt(s){s=Math.round(s);return s<120?s+' s':s<7200?Math.round(s/60)+' min':(s/3600).toFixed(1)+' h'}
setInterval(()=>{if(last)$('age').textContent='hace '+fmt(last.age_s+(Date.now()-last.at)/1000)},1000);
async function chart(){
 try{const t=await (await fetch('/csv',{cache:'no-store'})).text();
  const rows=t.trim().split('\n').slice(1).map(l=>l.split(',').map(Number)).filter(r=>r[4]);
  const W=600,H=220,lo=2900,hi=4300,svg=$('chart');
  const y=v=>H-(v-lo)/(hi-lo)*H;let s='';
  for(const v of [3000,3500,4000])s+=`<line x1=0 x2=${W} y1=${y(v)} y2=${y(v)} stroke=#333 /><text x=4 y=${y(v)-3} fill=#666 font-size=11>${v/1000} V</text>`;
  s+=`<line x1=0 x2=${W} y1=${y(3300)} y2=${y(3300)} stroke=#e44 stroke-dasharray=4 /><text x=${W-70} y=${y(3300)-3} fill=#e44 font-size=11>corte 3,3 V</text>`;
  if(rows.length>1){const t0=rows[0][0],t1=rows[rows.length-1][0]||1;
   s+='<polyline fill=none stroke=#6af stroke-width=2 points="'+rows.map(r=>((r[0]-t0)/Math.max(1,t1-t0)*W).toFixed(1)+','+y(r[2]).toFixed(1)).join(' ')+'"/>';}
  svg.innerHTML=s;}catch(e){}
}
poll();chart();setInterval(poll,5000);setInterval(chart,30000);
</script></body></html>)HTML";

static void handle_root() { s_server.send_P(200, "text/html; charset=utf-8", kPage); }

static void handle_api() {
  char buf[320];
  snprintf(buf, sizeof(buf),
           "{\"adc_mv\":%u,\"mv\":%d,\"pct\":%d,\"valid\":%s,\"fails\":%lu,"
           "\"age_s\":%lu,\"uptime_s\":%lu,\"interval_s\":%u,\"clients\":%u,"
           "\"cal\":\"%s\",\"cal_pts\":[%u,%u,%u,%u],\"cutoff_mv\":%d,\"samples\":%u}",
           s_last.adc_mv, s_last.bat_mv, s_last.pct, s_last.valid ? "true" : "false",
           (unsigned long)s_last_fails, (unsigned long)((millis() - s_last_ms) / 1000),
           (unsigned long)(millis() / 1000), MON_INTERVAL_MS / 1000,
           (unsigned)WiFi.softAPgetStationNum(),
           s_cal_nvs ? "NVS" : "defecto", s_cal.adc_a_mv, s_cal.bat_a_mv, s_cal.adc_b_mv,
           s_cal.bat_b_mv, BATTERY_CUTOFF_MV, (unsigned)s_hist_count);
  s_server.sendHeader("Cache-Control", "no-store");
  s_server.send(200, "application/json", buf);
}

/* Historial completo, del mas antiguo al mas reciente. Se manda por trozos
 * para no montar ~20 KB de String en el heap. */
static void handle_csv() {
  s_server.sendHeader("Cache-Control", "no-store");
  s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_server.send(200, "text/csv", "t_s,adc_mv,bat_mv,pct,valid\n");
  const size_t start = (s_hist_next + MON_HISTORY_LEN - s_hist_count) % MON_HISTORY_LEN;
  char chunk[1024];
  size_t used = 0;
  for (size_t i = 0; i < s_hist_count; i++) {
    const Sample &h = s_hist[(start + i) % MON_HISTORY_LEN];
    used += snprintf(chunk + used, sizeof(chunk) - used, "%lu,%u,%d,%d,%d\n",
                     (unsigned long)h.t_s, h.adc_mv, h.bat_mv, h.pct, h.valid ? 1 : 0);
    if (sizeof(chunk) - used < 48) { s_server.sendContent(chunk, used); used = 0; }
  }
  if (used) s_server.sendContent(chunk, used);
  s_server.sendContent("");   // fin del envio por trozos
}

/* ========================================================================== */

void setup() {
  Serial.begin(MON_SERIAL_BAUD);
  delay(200);
  Serial.println("\n=== battery_monitor (herramienta de banco) ===");

  /* El firmware principal duerme con PWDN en alto y retenido: se suelta y se
   * deja en alto para que el OV2640 no consuma (~20-40 mA) ni falsee la carga. */
  rtc_gpio_hold_dis((gpio_num_t)CAM_PIN_PWDN);
  pinMode(CAM_PIN_PWDN, OUTPUT);
  digitalWrite(CAM_PIN_PWDN, HIGH);
  pinMode(CAM_PIN_FLASH_LED, OUTPUT);
  digitalWrite(CAM_PIN_FLASH_LED, LOW);
  pinMode(STATUS_LED_PIN, OUTPUT);
  /* Sin monitor serie (placa a bateria), el LED dice por que se reinicio. */
  const esp_reset_reason_t why = esp_reset_reason();
  Serial.printf("Reset reason: %d%s\n", (int)why,
                why == ESP_RST_BROWNOUT ? " (BROWNOUT: la alimentacion cayo)" : "");
  if (why == ESP_RST_BROWNOUT) {
    for (int i = 0; i < 3; i++) { led_set(true); delay(400); led_set(false); delay(300); }
    delay(500);
  }
  led_set(true);                 // fijo durante la primera medida

  load_cal();
  take_measurement();            // primera medida con el WiFi aun sin arrancar

  s_server.on("/", handle_root);
  s_server.on("/api", handle_api);
  s_server.on("/csv", handle_csv);
  /* Portal cautivo: cualquier otra URL (incluidas las de deteccion de
   * Android/iOS) redirige a la pagina, asi el movil la abre al conectarse. */
  s_server.onNotFound([]() {
    s_server.sendHeader("Location", "http://192.168.4.1/", true);
    s_server.send(302, "text/plain", "");
  });

  ap_up();
  Serial.printf("WIFI: AP \"%s\" (clave \"%s\") -> http://%s/\n",
                MON_AP_SSID, MON_AP_PASS, WiFi.softAPIP().toString().c_str());
}

void loop() {
  s_dns.processNextRequest();
  s_server.handleClient();

  if (millis() - s_last_ms >= MON_INTERVAL_MS) {
    ap_down();
    led_set(true);
    delay(MON_SETTLE_MS);
    take_measurement();
    ap_up();
  }
  led_update();
  delay(2);
}
