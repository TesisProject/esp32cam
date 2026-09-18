/* ============================================================================
 *  upload_module.h  -  Sube un frame JPEG a la Raspberry Pi (fog node)
 *  Endpoint y credenciales: ver config.h (FOG_SERVER_*) y secrets.h.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "esp_camera.h"

/* Telemetria que viaja junto a la imagen, en el mismo multipart.
 *
 *  Son los campos que el Fog acepta; enviar otros no rompe nada (los ignora)
 *  pero tampoco sirve de nada. Deliberadamente NO estan aqui battery_percent
 *  ni battery_state (conclusiones derivadas de una curva que puede estar mal)
 *  ni battery_sag_mv ni frame_len (los calcula el Fog al recibir).
 *
 *  Todos son opcionales: un campo a 0 (o a false en battery_valid) se omite
 *  del cuerpo en vez de enviarse vacio. Ver config.h, seccion 3.            */
struct UploadTelemetry {
  bool     battery_valid;      // false si el ADS1115 no respondio
  uint32_t battery_mv;         // pack en reposo, al despertar
  uint32_t battery_load_mv;    // minimo durante la asociacion WiFi. 0 = sin dato
  int32_t  rssi;               // dBm
  uint32_t wifi_connect_ms;    // lo que tardo en asociarse
  uint8_t  reset_reason;       // esp_reset_reason(): 8 = deep sleep, 9 = brownout
  uint32_t awake_ms;           // tiempo despierto al lanzar el POST
  uint32_t free_heap;          // bytes libres
  uint32_t wake_count;         // ciclos desde el ultimo arranque en frio
  uint32_t wifi_fail_streak;   // fallos de WiFi seguidos antes de este ciclo
};

/* Invalida la IP del Fog cacheada en RTC memory, para que el proximo ciclo
 * vuelva a resolver FOG_SERVER_HOST por mDNS. Se llama cuando el POST falla
 * por conexion: es como el firmware absorbe un cambio de IP de la Pi sin que
 * nadie tenga que tocar config.h. */
void upload_forget_host();

/* Deja la estructura en "sin datos": todo a cero. */
void upload_telemetry_clear(UploadTelemetry *t);

/* POST multipart/form-data del frame al endpoint de esta camara.
 * Devuelve el codigo de respuesta HTTP (202 = aceptado), o un valor negativo
 * (ver HTTPClient.h) si fallo antes de recibir respuesta (timeout, DNS, ...). */
int upload_frame(camera_fb_t *fb, const UploadTelemetry &telemetry);
