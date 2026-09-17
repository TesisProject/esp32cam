/*
 * ESP32-CAM (AI-Thinker) - Captura de fotos a microSD + streaming por navegador
 *
 * Funciones:
 *  - Servidor web en el puerto 80 con una pagina de control.
 *  - Streaming MJPEG en vivo en el puerto 81 (se ve embebido en la pagina).
 *  - Boton "Tomar foto": captura una imagen JPEG y la guarda en la microSD.
 *  - Modo automatico opcional: guarda una foto cada N segundos.
 *
 * Placa en Arduino IDE: "AI Thinker ESP32-CAM"
 * (Herramientas -> Placa -> ESP32 Arduino -> AI Thinker ESP32-CAM)
 */

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "FS.h"
#include "SD_MMC.h"
#include <Preferences.h>

// ================== CONFIGURACION ==================
const char *WIFI_SSID = "TU_RED_WIFI";      // <-- cambiar
const char *WIFI_PASS = "TU_PASSWORD";      // <-- cambiar

// Modo automatico: guardar una foto cada N segundos (0 = desactivado al inicio)
unsigned long intervaloAutoSeg = 0;
// ===================================================

// Pines de la camara para el modulo AI-Thinker ESP32-CAM
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

#define LED_FLASH 4  // LED blanco de la placa

httpd_handle_t servidorWeb = NULL;     // puerto 80: pagina + acciones
httpd_handle_t servidorStream = NULL;  // puerto 81: video MJPEG

Preferences prefs;             // guarda el contador de fotos entre reinicios
bool sdDisponible = false;
unsigned long ultimaFotoAuto = 0;

// ---------- Inicializacion de la camara ----------
bool iniciarCamara() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;

  // Con PSRAM se puede usar mas resolucion y doble buffer
  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;   // 800x600 (subir a UXGA si se quiere mas)
    config.jpeg_quality = 12;             // 0-63, menor = mejor calidad
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size = FRAMESIZE_VGA;    // 640x480
    config.jpeg_quality = 15;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Error al iniciar camara: 0x%x\n", err);
    return false;
  }
  return true;
}

// ---------- Inicializacion de la microSD ----------
bool iniciarSD() {
  // Modo 1 bit: mas lento pero libera GPIO4 (LED flash) y GPIO12/13
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("No se pudo montar la microSD");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("No hay tarjeta microSD insertada");
    return false;
  }
  Serial.printf("microSD montada. Tamano: %llu MB\n", SD_MMC.cardSize() / (1024 * 1024));
  if (!SD_MMC.exists("/fotos")) {
    SD_MMC.mkdir("/fotos");
  }
  return true;
}

// ---------- Guardar una foto en la SD ----------
// Devuelve el nombre del archivo guardado, o cadena vacia si fallo.
String guardarFoto() {
  if (!sdDisponible) {
    Serial.println("SD no disponible, no se guarda la foto");
    return "";
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Fallo la captura de la camara");
    return "";
  }

  // Numero de foto persistente (sigue contando despues de reiniciar)
  unsigned int numFoto = prefs.getUInt("contador", 0) + 1;
  prefs.putUInt("contador", numFoto);

  String ruta = "/fotos/foto_" + String(numFoto) + ".jpg";
  File archivo = SD_MMC.open(ruta.c_str(), FILE_WRITE);
  if (!archivo) {
    Serial.println("No se pudo crear el archivo en la SD");
    esp_camera_fb_return(fb);
    return "";
  }
  archivo.write(fb->buf, fb->len);
  archivo.close();
  esp_camera_fb_return(fb);

  Serial.printf("Foto guardada: %s (%u bytes)\n", ruta.c_str(), fb->len);
  return ruta;
}

// ================== PAGINA WEB ==================
static const char PAGINA_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32-CAM</title>
<style>
  body { font-family: Arial, sans-serif; text-align: center; background: #1e1e1e; color: #eee; margin: 0; padding: 16px; }
  h1 { font-size: 1.3em; }
  img { max-width: 100%; border-radius: 8px; border: 2px solid #444; }
  button { font-size: 1em; padding: 10px 22px; margin: 8px; border: none; border-radius: 6px; cursor: pointer; }
  .foto { background: #2e7d32; color: white; }
  .auto { background: #1565c0; color: white; }
  #estado { margin-top: 10px; color: #9e9e9e; min-height: 1.2em; }
  input { width: 60px; font-size: 1em; padding: 6px; text-align: center; }
</style>
</head>
<body>
<h1>ESP32-CAM &mdash; Monitoreo</h1>
<img id="stream" src="">
<div>
  <button class="foto" onclick="tomarFoto()">Tomar foto</button>
</div>
<div>
  Cada <input id="segundos" type="number" min="1" value="10"> seg
  <button class="auto" id="btnAuto" onclick="toggleAuto()">Activar auto</button>
</div>
<p id="estado"></p>
<script>
  // El stream corre en el puerto 81 del mismo equipo
  document.getElementById('stream').src = 'http://' + location.hostname + ':81/stream';
  let autoActivo = false;

  function tomarFoto() {
    document.getElementById('estado').textContent = 'Capturando...';
    fetch('/foto').then(r => r.text()).then(t => {
      document.getElementById('estado').textContent = t;
    }).catch(() => {
      document.getElementById('estado').textContent = 'Error al tomar la foto';
    });
  }

  function toggleAuto() {
    const seg = document.getElementById('segundos').value;
    const url = autoActivo ? '/auto?seg=0' : '/auto?seg=' + seg;
    fetch(url).then(r => r.text()).then(t => {
      autoActivo = !autoActivo;
      document.getElementById('btnAuto').textContent = autoActivo ? 'Detener auto' : 'Activar auto';
      document.getElementById('estado').textContent = t;
    });
  }
</script>
</body>
</html>
)HTML";

// ================== HANDLERS HTTP ==================

// GET /  -> pagina principal
static esp_err_t handlerIndex(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, PAGINA_HTML, HTTPD_RESP_USE_STRLEN);
}

// GET /foto -> captura y guarda en SD
static esp_err_t handlerFoto(httpd_req_t *req) {
  String ruta = guardarFoto();
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  if (ruta.length() > 0) {
    String msg = "Foto guardada en SD: " + ruta;
    return httpd_resp_send(req, msg.c_str(), HTTPD_RESP_USE_STRLEN);
  }
  httpd_resp_set_status(req, "500 Internal Server Error");
  return httpd_resp_send(req, "Error: no se pudo guardar (verificar SD)", HTTPD_RESP_USE_STRLEN);
}

// GET /auto?seg=N -> activa/desactiva captura automatica cada N segundos (0 = apagar)
static esp_err_t handlerAuto(httpd_req_t *req) {
  char query[32] = {0};
  char valor[8] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "seg", valor, sizeof(valor)) == ESP_OK) {
    intervaloAutoSeg = atol(valor);
    ultimaFotoAuto = millis();
  }
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  String msg = (intervaloAutoSeg > 0)
                 ? "Modo automatico: 1 foto cada " + String(intervaloAutoSeg) + " seg"
                 : String("Modo automatico desactivado");
  return httpd_resp_send(req, msg.c_str(), HTTPD_RESP_USE_STRLEN);
}

// GET :81/stream -> video MJPEG continuo
static esp_err_t handlerStream(httpd_req_t *req) {
  static const char *BOUNDARY = "--frame";
  char cabecera[64];

  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Fallo captura para stream");
      return ESP_FAIL;
    }

    size_t lenCab = snprintf(cabecera, sizeof(cabecera),
                             "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                             fb->len);

    esp_err_t res = httpd_resp_send_chunk(req, cabecera, lenCab);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;  // el navegador cerro la conexion
  }
  return ESP_OK;
}

// ---------- Arranque de los servidores ----------
void iniciarServidores() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t uriIndex = { .uri = "/",     .method = HTTP_GET, .handler = handlerIndex, .user_ctx = NULL };
  httpd_uri_t uriFoto  = { .uri = "/foto", .method = HTTP_GET, .handler = handlerFoto,  .user_ctx = NULL };
  httpd_uri_t uriAuto  = { .uri = "/auto", .method = HTTP_GET, .handler = handlerAuto,  .user_ctx = NULL };

  if (httpd_start(&servidorWeb, &config) == ESP_OK) {
    httpd_register_uri_handler(servidorWeb, &uriIndex);
    httpd_register_uri_handler(servidorWeb, &uriFoto);
    httpd_register_uri_handler(servidorWeb, &uriAuto);
  }

  // Segundo servidor solo para el video, asi el stream no bloquea los botones
  httpd_config_t configStream = HTTPD_DEFAULT_CONFIG();
  configStream.server_port = 81;
  configStream.ctrl_port = 32769;

  httpd_uri_t uriStream = { .uri = "/stream", .method = HTTP_GET, .handler = handlerStream, .user_ctx = NULL };

  if (httpd_start(&servidorStream, &configStream) == ESP_OK) {
    httpd_register_uri_handler(servidorStream, &uriStream);
  }
}

// ================== SETUP / LOOP ==================
void setup() {
  Serial.begin(115200);
  Serial.println();

  pinMode(LED_FLASH, OUTPUT);
  digitalWrite(LED_FLASH, LOW);

  prefs.begin("esp32cam", false);

  if (!iniciarCamara()) {
    Serial.println("Camara no disponible. Reiniciando en 5 seg...");
    delay(5000);
    ESP.restart();
  }

  sdDisponible = iniciarSD();

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);  // mejora la fluidez del stream
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  iniciarServidores();

  Serial.println("==========================================");
  Serial.print("Listo. Abrir en el navegador: http://");
  Serial.println(WiFi.localIP());
  Serial.println("==========================================");
}

void loop() {
  // Captura automatica cada N segundos si esta activada
  if (intervaloAutoSeg > 0 && millis() - ultimaFotoAuto >= intervaloAutoSeg * 1000UL) {
    ultimaFotoAuto = millis();
    guardarFoto();
  }
  delay(50);
}
