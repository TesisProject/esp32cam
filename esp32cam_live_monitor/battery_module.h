/* ============================================================================
 *  battery_module.h  -  Lectura del pack de bateria via ADC externo ADS1115
 *  Pines, divisor, umbrales y ritmo de muestreo: todo en config.h
 * ==========================================================================*/
#pragma once

#include <Arduino.h>

enum BatteryState {
  BATT_STATE_UNKNOWN = 0,   // sin ADC o sin lectura valida todavia
  BATT_STATE_OK,
  BATT_STATE_LOW,           // por debajo de BATTERY_LOW_PERCENT
  BATT_STATE_CRITICAL       // por debajo de BATTERY_CRITICAL_PERCENT
};

struct BatteryReading {
  bool         valid;        // false si el ADS1115 no responde
  float        voltage;      // voltios en bornes del pack (ya filtrados)
  float        raw_voltage;  // ultima medida sin filtro EMA
  uint8_t      percent;      // 0..100 segun la curva de descarga Li-ion
  BatteryState state;
  uint32_t     age_ms;       // tiempo desde la ultima medida
};

/* Arranca el bus I2C y el ADS1115. false si el chip no responde. */
bool battery_init();

/* Llamar desde loop(). Solo mide cuando toca (BATTERY_READ_INTERVAL_MS).
 * force=true mide ya mismo. Devuelve true si ha hecho una medida nueva. */
bool battery_update(bool force = false);

/* Minimo de tension del pack (en mV) durante una ventana de window_ms,
 * muestreando lo mas rapido que da el ADS1115. Pensada para llamarse mientras
 * el WiFi transmite: la caida respecto a la medida en reposo es proporcional
 * a la resistencia interna, que es el predictor real de fin de vida.
 *
 * Bloquea window_ms. Devuelve 0 si no hubo ninguna lectura valida. */
uint32_t battery_sample_min_mv(uint32_t window_ms);

/* Ultimo estado conocido (no bloquea, no toca el I2C). */
BatteryReading battery_get();

/* Etiqueta textual del estado, para JSON y logs. */
const char *battery_state_name(BatteryState state);

/* true si el pack lleva BATTERY_CRITICAL_CONFIRMATIONS lecturas seguidas
 * por debajo del umbral critico: hora de apagar para no danar las celdas. */
bool battery_critical_confirmed();
