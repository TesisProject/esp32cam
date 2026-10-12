/* ============================================================================
 *  wifi_module.h  -  Conexion WiFi en modo estacion
 *  Timeout, potencia de TX e IP fija: config.h. La red: NVS (settings_module).
 * ==========================================================================*/
#pragma once

#include <Arduino.h>

/* Conecta al router con timeout (WIFI_CONNECT_TIMEOUT_MS). Devuelve false si
 * no lo consigue: el llamador debe abortar el ciclo, no reintentar.
 * La red la decide el llamador (la de la NVS, o en E8 la pendiente): este
 * modulo no sabe nada de donde sale. */
bool wifi_connect(const char *ssid, const char *password);

/* Desconecta y apaga la radio antes del deep sleep. */
void wifi_off();

/* Telemetria del ultimo wifi_connect(): ms hasta asociarse y RSSI actual
 * (dBm; 0 si no hay conexion). */
uint32_t wifi_connect_ms();
int wifi_rssi();
