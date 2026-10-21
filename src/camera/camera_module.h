/* ============================================================================
 *  camera_module.h  -  Inicializacion y captura del sensor OV2640
 *  Toda la parametrizacion (pines, resolucion, calidad) viene de config.h
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "esp_camera.h"

/* Arranca el sensor. Devuelve false si no hay camara o falla la PSRAM. */
bool camera_init();

/* true si el sensor esta inicializado y respondiendo. */
bool camera_ready();

/* Captura un frame JPEG. Devuelve nullptr si falla.
 * El buffer DEBE devolverse siempre con camera_release(). */
camera_fb_t *camera_capture();

/* Devuelve el frame buffer al driver. Seguro con nullptr. */
void camera_release(camera_fb_t *fb);

/* Control del LED flash blanco (GPIO4). No hace nada si CAM_FLASH_ENABLED=0. */
void camera_flash(bool on);

/* Flash en LOW y retenido (rtc_gpio_hold_en) para el deep sleep: sin esto,
 * el pull-up R11 lo deja encendido a medias. Siempre, haya flash o no. */
void camera_flash_hold_off();

/* Ahorro de energia: corta/restaura la alimentacion del sensor por PWDN.
 * camera_capture() despierta el sensor automaticamente si hiciera falta. */
void camera_power_down();
void camera_power_up();
bool camera_is_powered();

/* Instante (millis) de la ultima captura: lo usa la gestion de energia. */
uint32_t camera_last_use_ms();

/* Nombre legible de la resolucion activa, para el log por serie. */
const char *camera_resolution_name();

/* Resolucion (nombre del enum: "SVGA", "QVGA"...) y calidad JPEG aplicadas
 * de verdad, ya degradadas si no hay PSRAM. Para la telemetria del POST. */
const char *camera_framesize_name();
int camera_jpeg_quality();
