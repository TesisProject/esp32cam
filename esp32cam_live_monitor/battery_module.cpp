/* ============================================================================
 *  battery_module.cpp  -  Medida del pack de bateria con ADS1115 (I2C)
 *
 *  Cadena de medida:
 *      VBAT -> divisor R_TOP/R_BOTTOM -> A0 del ADS1115 -> I2C -> ESP32
 *
 *  El driver del ADS1115 va escrito aqui mismo sobre Wire (son ~60 lineas):
 *  asi el sketch no necesita instalar NINGUNA libreria desde el Gestor de
 *  Librerias del Arduino IDE. Solo el core ESP32 de Espressif.
 *
 *  Nota sobre los buses I2C: la camara usa SCCB en el puerto I2C 1 (con sus
 *  propios pines 26/27), asi que el objeto Wire de Arduino (puerto 0) queda
 *  libre para el ADS1115 en GPIO14/15. No hay conflicto entre ambos.
 * ==========================================================================*/
#include "battery_module.h"
#include "config.h"

#include <Wire.h>

/* ===========================================================================
 * Driver minimo del ADS1115
 *
 *  Solo hacen falta dos registros:
 *    0x00  conversion  (16 bits con signo, el resultado)
 *    0x01  config      (16 bits, que medir y como)
 *
 *  Mapa del registro de configuracion:
 *    bit 15     OS        1 = iniciar una conversion
 *    bits 14-12 MUX       100+n = AINn contra GND (medida simple)
 *    bits 11-9  PGA       ganancia -> fondo de escala (ver config.h)
 *    bit 8      MODE      1 = disparo unico (el chip se duerme entre medidas)
 *    bits 7-5   DR        velocidad de conversion
 *    bits 4-2   comparador (sin usar)
 *    bits 1-0   COMP_QUE  11 = comparador desactivado
 * ==========================================================================*/
static const uint8_t ADS_REG_CONVERSION = 0x00;
static const uint8_t ADS_REG_CONFIG     = 0x01;

/* Muestras por segundo segun BATTERY_ADS_DATA_RATE (0..7). */
static const uint16_t kDataRateSps[8] = {8, 16, 32, 64, 128, 250, 475, 860};

/* Tiempo de conversion + margen, en ms. */
static const uint32_t kConversionMs =
    (1000UL / kDataRateSps[BATTERY_ADS_DATA_RATE & 0x7]) + 2UL;

static bool ads_write_reg(uint8_t reg, uint16_t value) {
  Wire.beginTransmission((uint8_t)BATTERY_ADS_ADDRESS);
  Wire.write(reg);
  Wire.write((uint8_t)(value >> 8));
  Wire.write((uint8_t)(value & 0xFF));
  return Wire.endTransmission() == 0;
}

static bool ads_read_reg(uint8_t reg, uint16_t *out) {
  Wire.beginTransmission((uint8_t)BATTERY_ADS_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;

  if (Wire.requestFrom((uint8_t)BATTERY_ADS_ADDRESS, (uint8_t)2) != 2) {
    return false;
  }
  const uint8_t hi = Wire.read();
  const uint8_t lo = Wire.read();
  *out = ((uint16_t)hi << 8) | (uint16_t)lo;
  return true;
}

/* Una medida simple (AINn contra GND) en modo disparo unico. */
static bool ads_read_channel(uint8_t channel, int16_t *counts) {
  const uint16_t cfg =
      (uint16_t)(1u << 15) |                                   // OS: arrancar
      (uint16_t)(((uint16_t)(0x4 + (channel & 0x3))) << 12) |  // MUX
      (uint16_t)(((uint16_t)BATTERY_ADS_PGA & 0x7) << 9) |     // PGA
      (uint16_t)(1u << 8) |                                    // disparo unico
      (uint16_t)(((uint16_t)BATTERY_ADS_DATA_RATE & 0x7) << 5) |
      (uint16_t)0x0003;                                        // sin comparador

  if (!ads_write_reg(ADS_REG_CONFIG, cfg)) return false;

  delay(kConversionMs);

  /* El bit OS vuelve a 1 cuando la conversion ha terminado. Se sondea por si
   * el chip va mas lento de lo previsto (p. ej. bus a baja velocidad). */
  for (int i = 0; i < 10; i++) {
    uint16_t status = 0;
    if (!ads_read_reg(ADS_REG_CONFIG, &status)) return false;
    if (status & 0x8000) break;
    delay(1);
  }

  uint16_t raw = 0;
  if (!ads_read_reg(ADS_REG_CONVERSION, &raw)) return false;

  *counts = (int16_t)raw;   // el resultado viene en complemento a dos
  return true;
}

/* ===========================================================================
 * Estado del modulo
 * ==========================================================================*/
static bool         s_present       = false;   // ADS1115 detectado
static bool         s_have_reading  = false;
static float        s_voltage_ema   = 0.0f;
static float        s_voltage_raw   = 0.0f;
static uint8_t      s_percent       = 0;
static BatteryState s_state         = BATT_STATE_UNKNOWN;
static uint32_t     s_last_read_ms  = 0;
static uint8_t      s_critical_hits = 0;

/* Factor del divisor: cuanto hay que multiplicar la tension del pin para
 * recuperar la del pack.  (R_TOP + R_BOTTOM) / R_BOTTOM                    */
static constexpr float kDividerFactor =
    (BATTERY_R_TOP_OHM + BATTERY_R_BOTTOM_OHM) / BATTERY_R_BOTTOM_OHM;

/* ---------------------------------------------------------------------------
 * Curvas de descarga: tension POR CELDA -> % de carga restante.
 *
 * Una regla de tres entre "vacia" y "llena" miente en las tres quimicas, y
 * cada una miente de forma distinta. Por eso hay una tabla por quimica y se
 * interpola por tramos. La curva la elige BATTERY_CHEMISTRY en config.h.
 * -------------------------------------------------------------------------*/
struct CurvePoint { float volts; uint8_t percent; };

#if BATTERY_CHEMISTRY == BATTERY_CHEM_ALKALINE
/* Pila alcalina AA/AAA bajo carga media (~200 mA).
 * Es la quimica mas facil de medir: la tension cae de forma casi lineal de
 * 1.5 V a 0.9 V, asi que el porcentaje es bastante fiable.
 * Ojo: bajo los picos de TX del WiFi estas pilas se hunden bastante mas de
 * lo que sugiere la tabla; de ahi el promediado de BATTERY_SAMPLES. */
static const CurvePoint kCurve[] = {
  {1.60f, 100}, {1.55f, 97}, {1.50f, 92}, {1.45f, 85}, {1.40f, 77},
  {1.35f, 69}, {1.30f, 60}, {1.25f, 51}, {1.20f, 42}, {1.15f, 33},
  {1.10f, 24}, {1.05f, 16}, {1.00f, 9}, {0.95f, 4}, {0.90f, 0}
};
static const char *kChemName = "alcalina";

#elif BATTERY_CHEMISTRY == BATTERY_CHEM_NIMH
/* NiMH recargable AA/AAA. Curva MUY plana: entre el 70 % y el 20 % de carga
 * la tension solo se mueve ~30 mV por celda. El porcentaje aqui es
 * orientativo por naturaleza, no por falta de precision del ADC: la fisica
 * de la celda no da para mas. El desplome final es abrupto. */
static const CurvePoint kCurve[] = {
  {1.45f, 100}, {1.40f, 95}, {1.35f, 88}, {1.32f, 80}, {1.30f, 71},
  {1.28f, 62}, {1.27f, 53}, {1.26f, 45}, {1.25f, 37}, {1.23f, 28},
  {1.21f, 20}, {1.18f, 13}, {1.15f, 8}, {1.10f, 4}, {1.00f, 0}
};
static const char *kChemName = "NiMH";

#else   /* BATTERY_CHEM_LIION */
/* Li-ion / LiPo. Pasa casi la mitad de su capacidad entre 3.7 y 3.9 V. */
static const CurvePoint kCurve[] = {
  {4.20f, 100}, {4.15f, 95}, {4.10f, 90}, {4.05f, 85}, {4.00f, 80},
  {3.95f, 74}, {3.90f, 67}, {3.85f, 59}, {3.82f, 52}, {3.79f, 45},
  {3.77f, 38}, {3.74f, 31}, {3.70f, 25}, {3.66f, 19}, {3.62f, 14},
  {3.55f, 10}, {3.48f,  6}, {3.40f,  3}, {3.30f,  0}
};
static const char *kChemName = "Li-ion";
#endif

static const size_t kCurveLen = sizeof(kCurve) / sizeof(kCurve[0]);

static uint8_t voltage_to_percent(float pack_volts) {
  /* La tabla es por celda: hay que repartir la tension del pack entre las
   * celdas en serie antes de buscar en ella. */
  const float v = pack_volts / (float)BATTERY_CELLS_SERIES;

  if (v >= kCurve[0].volts) return 100;
  if (v <= kCurve[kCurveLen - 1].volts) return 0;

  for (size_t i = 1; i < kCurveLen; i++) {
    if (v >= kCurve[i].volts) {
      const CurvePoint &hi = kCurve[i - 1];
      const CurvePoint &lo = kCurve[i];
      const float span = hi.volts - lo.volts;
      if (span <= 0.0f) return lo.percent;
      const float t = (v - lo.volts) / span;
      return (uint8_t)lroundf(lo.percent + t * (hi.percent - lo.percent));
    }
  }
  return 0;
}

static BatteryState percent_to_state(uint8_t percent) {
  if (percent <= BATTERY_CRITICAL_PERCENT) return BATT_STATE_CRITICAL;
  if (percent <= BATTERY_LOW_PERCENT)      return BATT_STATE_LOW;
  return BATT_STATE_OK;
}

/* ---------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------*/
bool battery_init() {
#if !BATTERY_ENABLED
  LOG("BAT: medida desactivada en config.h");
  return false;
#else
  Wire.begin(BATTERY_I2C_SDA, BATTERY_I2C_SCL, BATTERY_I2C_FREQ_HZ);

  /* Sonda de presencia: si el chip esta, su registro de configuracion
   * responde. Un bus sin nada devuelve error o 0xFFFF. */
  uint16_t probe = 0;
  if (!ads_read_reg(ADS_REG_CONFIG, &probe) || probe == 0xFFFF) {
    LOG("BAT: ADS1115 no responde en 0x%02X (SDA=%d SCL=%d)",
        BATTERY_ADS_ADDRESS, BATTERY_I2C_SDA, BATTERY_I2C_SCL);
    s_present = false;
    s_state   = BATT_STATE_UNKNOWN;
    return false;
  }

  s_present = true;
  LOG("BAT: ADS1115 ok (config=0x%04X). Divisor x%.3f", probe, kDividerFactor);
  LOG("BAT: pack %s, %d celda(s) en serie, %d mAh. Rango %.2f - %.2f V",
      kChemName, (int)BATTERY_CELLS_SERIES, (int)BATTERY_CAPACITY_MAH,
      (double)BATTERY_VOLT_EMPTY, (double)BATTERY_VOLT_FULL);

  battery_update(true);   // primera medida inmediata
  return true;
#endif
}

/* ---------------------------------------------------------------------------
 * Medida
 * -------------------------------------------------------------------------*/
bool battery_update(bool force) {
#if !BATTERY_ENABLED
  (void)force;
  return false;
#else
  if (!s_present) return false;

  const uint32_t now = millis();
  if (!force && s_have_reading &&
      (now - s_last_read_ms) < BATTERY_READ_INTERVAL_MS) {
    return false;
  }

  /* Promedio de N muestras: filtra el ruido de los picos de TX del WiFi,
   * que hunden momentaneamente la tension del pack. */
  double accum = 0.0;
  int    taken = 0;
  for (int i = 0; i < BATTERY_SAMPLES; i++) {
    int16_t counts = 0;
    if (!ads_read_channel(BATTERY_ADS_CHANNEL, &counts)) continue;
    if (counts < 0) counts = 0;            // ruido por debajo de 0 V
    accum += (double)counts * BATTERY_ADS_LSB_MV;   // -> milivoltios en el pin
    taken++;
    if (BATTERY_SAMPLE_DELAY_MS > 0) delay(BATTERY_SAMPLE_DELAY_MS);
  }

  if (taken == 0) {
    LOG("BAT: sin muestras validas del ADS1115");
    return false;
  }

  const float pin_volts  = (float)(accum / taken) / 1000.0f;
  const float pack_volts = pin_volts * kDividerFactor * BATTERY_CALIBRATION;

  s_voltage_raw = pack_volts;

  /* Media exponencial: la tension de un pack Li-ion bajo carga variable
   * oscila decimas de voltio; sin filtro el porcentaje bailaria. */
  if (!s_have_reading) {
    s_voltage_ema = pack_volts;
  } else {
    s_voltage_ema = BATTERY_EMA_ALPHA * pack_volts +
                    (1.0f - BATTERY_EMA_ALPHA) * s_voltage_ema;
  }

  s_percent      = voltage_to_percent(s_voltage_ema);
  s_state        = percent_to_state(s_percent);
  s_last_read_ms = now;
  s_have_reading = true;

  if (s_state == BATT_STATE_CRITICAL) {
    if (s_critical_hits < 255) s_critical_hits++;
  } else {
    s_critical_hits = 0;
  }

  LOG("BAT: %.3f V (raw %.3f) -> %u%% [%s]",
      s_voltage_ema, s_voltage_raw, (unsigned)s_percent,
      battery_state_name(s_state));
  return true;
#endif
}

/* ---------------------------------------------------------------------------
 * Consulta
 * -------------------------------------------------------------------------*/
BatteryReading battery_get() {
  BatteryReading r;
  r.valid       = s_present && s_have_reading;
  r.voltage     = s_voltage_ema;
  r.raw_voltage = s_voltage_raw;
  r.percent     = s_percent;
  r.state       = s_state;
  r.age_ms      = s_have_reading ? (millis() - s_last_read_ms) : 0;
  return r;
}

const char *battery_state_name(BatteryState state) {
  switch (state) {
    case BATT_STATE_OK:       return "ok";
    case BATT_STATE_LOW:      return "low";
    case BATT_STATE_CRITICAL: return "critical";
    default:                  return "unknown";
  }
}

bool battery_critical_confirmed() {
  return s_present && s_have_reading &&
         s_critical_hits >= BATTERY_CRITICAL_CONFIRMATIONS;
}
