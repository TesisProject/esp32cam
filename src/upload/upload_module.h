/* ============================================================================
 *  upload_module.h  -  Sube un frame JPEG a la Raspberry Pi (fog node)
 *  Ruta: config.h (FOG_FRAMES_PATH). Host/puerto y credencial: NVS (los pasa
 *  el llamador).
 * ==========================================================================*/
#pragma once

#include <Arduino.h>

/* Telemetria que viaja en el MISMO multipart que la foto, como partes de
 * texto ademas de "file". No cuesta radio extra: la peticion se hace igual.
 * Nombres y semantica: los de fog-application/src/vision/camera_router.py,
 * que acepta todos estos campos como opcionales y descarta sin fallar los
 * que no sepa leer (la foto nunca se pierde por la telemetria).
 *
 * La rellena el .ino, que es quien orquesta el ciclo y conoce a todos los
 * modulos; este modulo solo la serializa.
 *
 * Bateria: battery_mv (en reposo, antes de encender camara y WiFi) y
 * battery_valid. NO se envia el porcentaje: el Fog guarda medidas y deriva
 * el % al leer (no lo acepta como campo). Tampoco battery_load_mv: GPIO13 es
 * ADC2 y no se puede leer con el WiFi transmitiendo.
 *
 * Foto atrasada (captured_at != nullptr): se envia captured_at y solo los
 * campos del momento de la captura (fw_version, reset_reason, framesize,
 * jpeg_quality, bateria). Los del ciclo (rssi, wifi_connect_ms, awake_ms,
 * free_heap, wake_count, wifi_fail_streak) no viajan: describirian el ciclo
 * de la subida, no el de la foto, y ensuciarian las series del Fog. */
struct UploadTelemetry {
  int         fw_version;        // FW_VERSION de config.h
  int         rssi;              // dBm, WiFi.RSSI()
  uint32_t    wifi_connect_ms;   // WiFi.begin -> WL_CONNECTED
  int         reset_reason;      // esp_reset_reason(): 8 = deep sleep, 9 = brownout
  uint32_t    awake_ms;          // millis() justo antes del POST
  uint32_t    free_heap;         // ESP.getFreeHeap(): solo RAM interna
  uint32_t    wake_count;        // despertares desde el ultimo arranque en frio
  uint32_t    wifi_fail_streak;  // ciclos seguidos sin WiFi antes de este
  const char *framesize;         // "SVGA", "QVGA"... la aplicada de verdad
  int         jpeg_quality;      // 10..63, la aplicada de verdad
  bool        battery_valid;     // false: battery_mv no se envia
  int32_t     battery_mv;        // tension del pack en reposo, mV
  const char *captured_at;       // ISO 8601 UTC; nullptr = foto actual (el Fog
                                 // usa su hora de recepcion)
};

/* POST multipart/form-data de un JPEG al Fog (la camara la resuelve el Fog
 * por la credencial, no por la URL). Host, puerto y credencial los pasa el
 * llamador (NVS o defectos de config.h): este modulo no lee la NVS.
 * api_key/api_token van en X-API-Key/X-API-Token.
 * jpeg: el frame de la camara (fb->buf) o una foto atrasada leida de flash.
 * telemetry puede ser nullptr: entonces solo se manda "file".
 * date_out (opcional): la cabecera Date de la respuesta, o "" si no vino.
 * Con ella se pone en hora el reloj (clock_module).
 * Devuelve el codigo de respuesta HTTP (200-299 = ok), o un valor negativo
 * (ver HTTPClient.h) si fallo antes de recibir respuesta (timeout, DNS, ...). */
int upload_frame(const char *fog_host, uint16_t fog_port,
                 const char *api_key, const char *api_token,
                 const uint8_t *jpeg, size_t jpeg_len,
                 const UploadTelemetry *telemetry,
                 char *date_out = nullptr, size_t date_out_len = 0);
