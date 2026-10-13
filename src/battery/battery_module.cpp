/* ============================================================================
 *  battery_module.cpp  -  Lectura del divisor y conversion a mV y %
 *
 *  Por que una recta de dos puntos y no "x2": el divisor es 1/2 nominal,
 *  pero el ADC de cada chip tiene su propio error de ganancia y de offset
 *  (ver la seccion de bateria de config.h). Todo es lineal en Vbat, asi que
 *  dos puntos medidos corrigen ambos errores a la vez.
 * ==========================================================================*/
#include "battery_module.h"
#include "../../config.h"

static const int32_t kCurveMv[]  = BATTERY_CURVE_MV;
static const int     kCurvePct[] = BATTERY_CURVE_PCT;
static const size_t  kCurveLen   = sizeof(kCurveMv) / sizeof(kCurveMv[0]);
static_assert(sizeof(kCurvePct) / sizeof(kCurvePct[0]) == kCurveLen,
              "BATTERY_CURVE_MV y BATTERY_CURVE_PCT deben tener los mismos puntos");

/* Interpolacion lineal entre los puntos de la curva, saturada en 0 y 100. */
static int percent_from_mv(int32_t mv) {
  if (mv <= kCurveMv[0]) return kCurvePct[0];
  for (size_t i = 1; i < kCurveLen; i++) {
    if (mv < kCurveMv[i]) {
      return kCurvePct[i - 1] + (int)((int64_t)(mv - kCurveMv[i - 1]) *
             (kCurvePct[i] - kCurvePct[i - 1]) / (kCurveMv[i] - kCurveMv[i - 1]));
    }
  }
  return kCurvePct[kCurveLen - 1];
}

void battery_read(const SettingsBatteryCal &cal, BatteryReading &out) {
  /* 12 dB (ADC_11db en la API de Arduino): fondo de escala ~3,1 V. Es ya el
   * defecto del core, pero el rango de la recta depende de ello. */
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);

  /* analogReadMilliVolts() devuelve 0 si la lectura falla (p. ej. el WiFi
   * tiene el ADC2). Con una celda conectada GPIO13 esta por encima de ~1,25 V,
   * asi que un 0 nunca es una medida util: esas muestras se descartan en vez
   * de arrastrar el promedio hacia abajo. Sin celda fallan todas: no valida. */
  uint32_t sum = 0;
  uint32_t good = 0;
  for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
    const uint32_t mv = analogReadMilliVolts(BATTERY_ADC_PIN);
    if (mv > 0) {
      sum += mv;
      good++;
    }
    delay(BATTERY_ADC_SAMPLE_GAP_MS);
  }

  out.adc_mv = good ? sum / good : 0;
  /* Recta por los dos puntos de calibracion. adc_b > adc_a lo garantiza
   * settings_battery_cal_valid(), asi que no hay division por cero. */
  out.mv = (int32_t)cal.bat_a_mv +
           ((int32_t)out.adc_mv - (int32_t)cal.adc_a_mv) *
           ((int32_t)cal.bat_b_mv - (int32_t)cal.bat_a_mv) /
           ((int32_t)cal.adc_b_mv - (int32_t)cal.adc_a_mv);
  out.percent = percent_from_mv(out.mv);
  out.valid = good >= (uint32_t)BATTERY_ADC_SAMPLES / 2 &&
              out.mv >= BATTERY_VALID_MIN_MV && out.mv <= BATTERY_VALID_MAX_MV;

  if (good < (uint32_t)BATTERY_ADC_SAMPLES) {
    LOG("BAT: %lu de %d lecturas del ADC2 fallaron",
        (unsigned long)(BATTERY_ADC_SAMPLES - good), BATTERY_ADC_SAMPLES);
  }
}
