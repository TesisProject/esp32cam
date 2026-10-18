/* ============================================================================
 *  fogconfig_module.h  -  GET /api/v1/camera/config del Fog
 *
 *  Por ahora solo se interpreta el bloque "firmware" (E5). "capture" sera
 *  E8. "wifi" NO se lee nunca: la WiFi, el host del Fog y la credencial solo
 *  se graban por USB (decision del 2026-10-17, ver CLAUDE.md "OTA y
 *  configuracion por USB"). Si el Fog la manda, se ignora.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "../../config.h"

/* Version de firmware que el Fog tiene asignada a esta camara. */
struct FirmwareOffer {
  uint32_t version;
  uint32_t size;                                     // bytes del .bin
  char     sha256[65];                               // hex minusculas
  char     signature[OTA_SIGNATURE_B64_MAX_LEN + 1]; // DER en Base64
  char     url[FOG_PATH_MAX_LEN + 1];                // relativa al Fog: "/api/v1/camera/firmware/4.bin"
};

struct FogConfig {
  bool          has_firmware;   // false: "firmware": null (nada asignado)
  FirmwareOffer firmware;
};

/* Pide la config. true si el Fog respondio 200 con un JSON valido; un bloque
 * "firmware" mal formado se trata como ausente (con log). */
bool fogconfig_fetch(const char *fog_host, uint16_t fog_port,
                     const char *api_key, const char *api_token, FogConfig &out);
