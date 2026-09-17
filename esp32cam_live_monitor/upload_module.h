/* ============================================================================
 *  upload_module.h  -  Sube un frame JPEG a la Raspberry Pi (fog node)
 *  Endpoint y credenciales: ver config.h (FOG_SERVER_*) y secrets.h.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "esp_camera.h"

/* POST multipart/form-data del frame al endpoint de esta camara.
 * Devuelve el codigo de respuesta HTTP (200-299 = ok), o un valor negativo
 * (ver HTTPClient.h) si fallo antes de recibir respuesta (timeout, DNS, ...). */
int upload_frame(camera_fb_t *fb);
