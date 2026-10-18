/* ============================================================================
 *  ota_module.h  -  OTA (E5): version, rollback y cliente de actualizacion
 *
 *  Una imagen nueva solo se da por buena despues de demostrar que puede
 *  completar un ciclo (rollback del bootloader). El cliente descarga del
 *  Fog la version que este le asigna en la config, solo si la firma de la
 *  nube es valida para la clave publica compilada (config.h, seccion 10).
 *  Requiere un esquema de particiones con dos ranuras de app (min_spiffs).
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "../fogconfig/fogconfig_module.h"

/* Log de arranque: FW_VERSION, particion activa y su estado OTA. Si el
 * bootloader acaba de hacer rollback, lo dice. */
void ota_log_boot_info();

/* Confirma la imagen en ejecucion si esta pendiente de verificar
 * (ESP_OTA_IMG_PENDING_VERIFY). Llamar SOLO tras un ciclo con WiFi conectado
 * y subida con respuesta 2xx. En una imagen flasheada por USB no hace nada. */
void ota_confirm_if_pending();

/* Comprueba la verificacion de firmas con el vector de prueba de la nube
 * (cloud-service/src/test/resources/firmware/signature-test-vector.json):
 * la firma buena valida y la misma con otra version no. Es la forma barata
 * de saber que nube, Fog y ESP32 arman la misma cadena. ~decenas de ms:
 * llamarlo solo en arranques en frio. */
bool ota_self_test();

/* Resultado de ota_handle_offer() cuando no instala (si instala, reinicia). */
enum OtaResult {
  OTA_IDLE,    // nada que hacer o motivo que no cambia en minutos (version,
               // ota_rej, firma, bateria): siguiente comprobacion la normal
  OTA_RETRY,   // fallo de la descarga o de la escritura: merece reintentar pronto
};

/* Decide que hacer con la version que ofrece el Fog: instalarla (verifica
 * firma, descarga, comprueba tamano y SHA-256, activa la otra ranura y
 * REINICIA: no vuelve) o ignorarla con un log (no es mayor, ya se rechazo,
 * bateria baja, firma mala, fallo de descarga...). Si detecta que esa
 * version ya se probo y hubo rollback, la apunta en ota_rej (NVS). */
OtaResult ota_handle_offer(const char *fog_host, uint16_t fog_port,
                           const char *api_key, const char *api_token,
                           const FirmwareOffer &offer, bool battery_ok);
