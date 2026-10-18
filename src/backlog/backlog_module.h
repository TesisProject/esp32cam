/* ============================================================================
 *  backlog_module.h  -  Fotos que no se pudieron subir, en la flash interna
 *
 *  LittleFS (core ESP32) sobre la particion "spiffs" de Minimal SPIFFS. Un
 *  fichero por foto: BACKLOG_DIR/<epoch UTC de captura, 10 cifras>.bin, con
 *  una cabecera fija (telemetria del momento de la captura) y el JPEG. La
 *  hora va en el nombre y no en la cabecera para poder corregirla con un
 *  rename (solo metadatos) en lugar de reescribir la foto.
 *
 *  La flash solo se toca cuando hay algo que hacer: un contador en memoria
 *  RTC dice si hay fotos pendientes, asi que un ciclo normal no monta el
 *  sistema de ficheros. Solo tras un arranque en frio se monta una vez para
 *  contarlas.
 *
 *  La particion es otra que la NVS y las ranuras de app: ni el OTA ni el
 *  rollback la tocan, y un firmware sin este modulo simplemente la ignora.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include <esp_system.h>
#include "../../config.h"
#include "../clock/clock_module.h"

/* Telemetria del momento de la captura que viaja con la foto atrasada. La
 * del ciclo de subida (rssi, wifi_connect_ms, awake_ms...) no: describiria
 * otro ciclo. */
struct BacklogMeta {
  int32_t fw_version;
  int16_t reset_reason;
  uint8_t jpeg_quality;
  bool    battery_valid;
  int32_t battery_mv;
  char    framesize[CAP_FRAMESIZE_MAX_LEN + 1];
};

/* Una foto leida de la flash. jpeg es del item: liberarlo SIEMPRE con
 * backlog_remove() (subida resuelta) o backlog_release() (se reintenta). */
struct BacklogItem {
  char        path[24];
  uint32_t    captured_at;   // epoch UTC
  BacklogMeta meta;
  uint8_t    *jpeg;
  size_t      jpeg_len;
};

/* Llamar al principio del ciclo: fuera de un despertar de deep sleep el
 * contador RTC no es fiable y se recuenta al primer uso. */
void backlog_begin(esp_reset_reason_t reason);

/* Fotos pendientes. Sin montar nada si el contador RTC es fiable. */
uint32_t backlog_pending();

/* Guarda una foto. Si no cabe, descarta las mas antiguas hasta que quepa.
 * false si no se pudo (flash llena o averiada): la foto se pierde. */
bool backlog_save(uint32_t captured_at, const uint8_t *jpeg, size_t len,
                  const BacklogMeta &meta);

/* Carga la mas antigua. Por el camino borra las ilegibles y las de mas de
 * BACKLOG_MAX_AGE_S (si hay hora). false = no queda ninguna utilizable. */
bool backlog_load_oldest(BacklogItem &out);

/* Borra la foto del item (subida resuelta, 2xx o 4xx) y libera su memoria. */
void backlog_remove(BacklogItem &item);

/* Libera la memoria del item sin borrar la foto (se reintenta otro ciclo). */
void backlog_release(BacklogItem &item);

/* Corrige con la deriva medida la hora de las fotos capturadas desde la
 * sincronizacion anterior (renombrandolas). */
void backlog_retime(const ClockSync &sync);

/* Borra todas las fotos pendientes (aprovisionamiento: {"cmd":"erase"}). */
bool backlog_erase_all();
