/* ============================================================================
 *  provision_module.h  -  Aprovisionamiento por USB (puerto serie)
 *
 *  Protocolo (contrato C), una linea JSON terminada en '\n':
 *    {"wifi":{"ssid":"...","password":"..."},"fogHost":"...","fogPort":8000}
 *    {"cmd":"erase"}                       -> borra la NVS del proyecto
 *  Respuestas, siempre una linea que empieza por "PROV: ":
 *    PROV: READY ...    la placa escucha desde ahora (el script espera esto)
 *    PROV: OK ...       guardado; la placa reinicia a continuacion
 *    PROV: ERROR ...    motivo; se sigue escuchando hasta agotar la ventana
 *  Se imprimen con Serial directamente, no con LOG(): son protocolo, tienen
 *  que salir aunque DEBUG_ENABLED sea 0.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>

/* Escucha el puerto serie durante window_ms. Con una linea valida guarda en
 * la NVS (via settings_module) y reinicia: en ese caso NO retorna. Retorna
 * al agotarse la ventana sin configuracion valida.
 * wifi_required: true si la NVS no tiene WiFi (una linea sin "wifi" no basta
 * para dejar la placa operativa). */
void provision_listen(uint32_t window_ms, bool wifi_required);
