/* ============================================================================
 *  wifi_module.cpp  -  WiFi solo en modo estacion
 *
 *  Un AP propio obligaria al ESP32 a emitir balizas sin parar, y con eso la
 *  radio no podria dormir nunca entre ciclos.
 * ==========================================================================*/
#include "wifi_module.h"
#include "../../config.h"

#include <WiFi.h>
#include <esp_mac.h>

/* Duracion del ultimo intento de conexion (WiFi.begin -> WL_CONNECTED). Es
 * la parte del ciclo con la radio encendida que mas varia (router lejos o
 * saturado, DHCP lento), y por eso se reporta al Fog como wifi_connect_ms. */
static uint32_t s_connect_ms = 0;

/* Si el ciclo se aborta antes de conectar (camara rota, placa sin
 * aprovisionar), el driver WiFi ni siquiera se ha iniciado: no hay nada que
 * apagar y llamar a WiFi.disconnect() solo ensuciaria el log. */
static bool s_started = false;

bool wifi_connect(const char *ssid, const char *password) {
  s_started = true;
  /* persistent(false): el driver no copia SSID/contrasena en su propia NVS.
   * La unica copia es la de settings_module, y "borrar la NVS" la elimina. */
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_HOSTNAME);

  /* Antes de conectar: si el router filtra por MAC o hay que reservarle IP,
   * hace falta verla justo en los ciclos en que la conexion falla.
   * Se lee del eFuse y no con WiFi.macAddress(): en el core 3.x esa devuelve
   * 00:00:00:00:00:00 hasta que la interfaz STA ha arrancado. */
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  LOG("WIFI: MAC=%02X:%02X:%02X:%02X:%02X:%02X",
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

#if WIFI_USE_STATIC_IP
  IPAddress ip(WIFI_STATIC_IP), gw(WIFI_STATIC_GATEWAY),
            mask(WIFI_STATIC_SUBNET), dns(WIFI_STATIC_DNS);
  if (!WiFi.config(ip, gw, mask, dns)) {
    LOG("WIFI: fallo al fijar la IP estatica, se sigue con DHCP");
  }
#endif

  /* Menos potencia de TX = menos corriente de pico y menos hundimiento de
   * tension en el pack. Si el router queda lejos, subelo en config.h. */
  WiFi.setTxPower((wifi_power_t)(WIFI_TX_POWER_DBM * 4));

  LOG("WIFI: conectando a \"%s\"...", ssid);
  WiFi.begin(ssid, password);

  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - started) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }
  /* Resolucion de 250 ms por el delay del bucle: suficiente para ver si un
   * ciclo tarda 1 s o 8 s en asociarse, que es lo que pesa en la bateria. */
  s_connect_ms = millis() - started;

  if (WiFi.status() != WL_CONNECTED) {
    /* Codigo de estado para diagnostico: 1=WL_NO_SSID_AVAIL (no ve la red,
     * problema de alcance/banda), 4=WL_CONNECT_FAILED (password/auth mal),
     * 6=WL_DISCONNECTED (generico). Ver enum wl_status_t en WiFiType.h. */
    LOG("WIFI: sin conexion tras %lu ms (status=%d), se aborta este ciclo",
        (unsigned long)WIFI_CONNECT_TIMEOUT_MS, (int)WiFi.status());
    WiFi.disconnect(true);
    return false;
  }

  LOG("WIFI: conectado en %lu ms. IP=%s  RSSI=%d dBm",
      (unsigned long)s_connect_ms, WiFi.localIP().toString().c_str(),
      (int)WiFi.RSSI());
  return true;
}

uint32_t wifi_connect_ms() { return s_connect_ms; }

int wifi_rssi() { return (int)WiFi.RSSI(); }

void wifi_off() {
  if (!s_started) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}
