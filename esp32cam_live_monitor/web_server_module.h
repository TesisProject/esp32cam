/* ============================================================================
 *  web_server_module.h  -  Servidor HTTP embebido del ESP32-CAM
 *
 *  Rutas (nombres definidos en config.h):
 *    GET /         dashboard (dashboard_html.h, compilado en el firmware)
 *    GET /config   JSON con la configuracion que necesita el dashboard
 *    GET /battery  JSON con tension, porcentaje y estado
 *    GET /capture  una foto JPEG
 *    GET /stream   MJPEG multipart (en HTTP_STREAM_PORT)
 *    GET /status   JSON de diagnostico (uptime, heap, RSSI, camara)
 * ==========================================================================*/
#pragma once

#include <Arduino.h>

/* Arranca los dos servidores (control y stream). */
bool webserver_start();

/* Detiene ambos servidores. */
void webserver_stop();

/* true mientras haya al menos un cliente consumiendo /stream.
 * La gestion de energia de main.cpp lo consulta para no bajar la CPU
 * ni dormir el sensor en mitad de una emision. */
bool webserver_stream_active();

/* Numero de clientes MJPEG conectados ahora mismo. */
int webserver_stream_clients();

/* millis() de la ultima peticion HTTP atendida. */
uint32_t webserver_last_request_ms();
