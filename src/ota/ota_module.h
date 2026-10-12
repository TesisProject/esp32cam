/* ============================================================================
 *  ota_module.h  -  Preparacion para OTA: version, particion activa, rollback
 *
 *  Todavia NO descarga firmware (eso es el cliente OTA, pendiente). Lo que
 *  hace ya es dejar el arranque listo para que una imagen nueva solo se de
 *  por buena despues de demostrar que puede completar un ciclo.
 *  Requiere un esquema de particiones con dos ranuras de app (min_spiffs).
 * ==========================================================================*/
#pragma once

#include <Arduino.h>

/* Log de arranque: FW_VERSION, particion activa y su estado OTA. Si el
 * bootloader acaba de hacer rollback, lo dice. */
void ota_log_boot_info();

/* Confirma la imagen en ejecucion si esta pendiente de verificar
 * (ESP_OTA_IMG_PENDING_VERIFY). Llamar SOLO tras un ciclo con WiFi conectado
 * y subida con respuesta 2xx. En una imagen flasheada por USB no hace nada. */
void ota_confirm_if_pending();
