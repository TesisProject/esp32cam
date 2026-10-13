/* ============================================================================
 *  battery_module.h  -  Tension del pack Li-ion por GPIO13 (ADC2)
 *
 *  Divisor 100k/100k desde VIN+ del MT3608 (tension de la celda) hasta
 *  GPIO13, con 100 nF a GND. GPIO13 es ADC2: battery_read() tiene que
 *  llamarse ANTES de wifi_connect(). Con el WiFi encendido el ADC2 es suyo y
 *  las lecturas fallan.
 *
 *  La recta de calibracion la pasa el llamador (NVS o defectos, via
 *  settings_module): este modulo no abre la NVS.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "../settings/settings_module.h"

struct BatteryReading {
  bool     valid;     // dentro de BATTERY_VALID_MIN_MV..MAX_MV y con muestras
  uint32_t adc_mv;    // promedio en GPIO13, mV (analogReadMilliVolts)
  int32_t  mv;        // tension estimada del pack, mV (con valid=false, orientativa)
  int      percent;   // 0..100 segun BATTERY_CURVE_*: solo para el log
};

/* Promedia BATTERY_ADC_SAMPLES lecturas (~70 ms) y aplica la recta. */
void battery_read(const SettingsBatteryCal &cal, BatteryReading &out);
