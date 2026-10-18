/* ============================================================================
 *  provision_module.h  -  Aprovisionamiento por USB (puerto serie)
 *
 *  Protocolo (contrato C), una linea JSON terminada en '\n':
 *    {"wifi":{"ssid":"...","password":"..."},
 *     "camera":{"id":"CAM-001","apiKey":"...","apiToken":"..."},
 *     "fogHost":"...","fogPort":8000}
 *    {"cmd":"erase"}                       -> borra la NVS del proyecto
 *  wifi y camera son obligatorios solo si la NVS aun no los tiene: al
 *  reaprovisionar basta con lo que cambie. Opcional:
 *    "batteryCal":{"adcA":2610,"batA":3700,"adcB":2720,"batB":4150}
 *  (mV en GPIO13 y mV reales del pack, dos puntos; ver config.h). Sin ella
 *  se usan los defectos de config.h.
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
 * Lo que la linea tiene que traer (wifi, camera) lo decide segun lo que ya
 * haya en la NVS: tras guardar, la placa debe quedar aprovisionada. */
void provision_listen(uint32_t window_ms);
