/* ============================================================================
 *  wifi_module.h  -  Conexion WiFi en modo estacion
 *  Red, timeout, potencia de TX e IP fija: ver config.h y secrets.h.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>

/* Conecta al router con timeout (WIFI_CONNECT_TIMEOUT_MS). Devuelve false si
 * no lo consigue: el llamador debe abortar el ciclo, no reintentar. */
bool wifi_connect();

/* Desconecta y apaga la radio antes del deep sleep. */
void wifi_off();

/* Telemetria del ultimo wifi_connect(): ms hasta asociarse y RSSI actual
 * (dBm; 0 si no hay conexion). */
uint32_t wifi_connect_ms();
int wifi_rssi();
