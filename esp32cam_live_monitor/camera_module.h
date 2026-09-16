/* ============================================================================
 *  camera_module.h  -  Inicializacion y captura del sensor OV2640
 *  Toda la parametrizacion (pines, resolucion, calidad) viene de config.h
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "esp_camera.h"

/* Perfiles de captura. Cambian resolucion + calidad JPEG segun el uso. */
enum CameraProfile {
  CAM_PROFILE_SNAPSHOT,   // foto fija: mas resolucion, mejor calidad
  CAM_PROFILE_STREAM      // video MJPEG: mas ligero, mas fps
};

/* Arranca el sensor. Devuelve false si no hay camara o falla la PSRAM. */
bool camera_init();

/* true si el sensor esta inicializado y respondiendo. */
bool camera_ready();

/* Aplica el perfil (no hace nada si ya esta activo). Evita reconfigurar
 * el sensor en cada frame del stream. */
bool camera_use_profile(CameraProfile profile);

/* Captura un frame JPEG. Devuelve nullptr si falla.
 * El buffer DEBE devolverse siempre con camera_release(). */
camera_fb_t *camera_capture();

/* Devuelve el frame buffer al driver. Seguro con nullptr. */
void camera_release(camera_fb_t *fb);

/* Control del LED flash blanco (GPIO4). No hace nada si CAM_FLASH_ENABLED=0. */
void camera_flash(bool on);

/* Ahorro de energia: corta/restaura la alimentacion del sensor por PWDN.
 * camera_capture() despierta el sensor automaticamente si hiciera falta. */
void camera_power_down();
void camera_power_up();
bool camera_is_powered();

/* Instante (millis) de la ultima captura: lo usa la gestion de energia. */
uint32_t camera_last_use_ms();

/* Nombre legible de la resolucion activa, para /status. */
const char *camera_resolution_name();
