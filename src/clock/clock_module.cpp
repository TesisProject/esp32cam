/* ============================================================================
 *  clock_module.cpp  -  Reloj del sistema puesto en hora por el Fog
 *
 *  settimeofday() fija la hora y ESP-IDF la conserva durante el deep sleep
 *  (contador del RTC). El RTC lento va con el oscilador RC interno de
 *  150 kHz, que deriva hasta ~5 % con la temperatura: por eso, en cada
 *  sincronizacion se mide la deriva real desde la anterior y se ofrece para
 *  corregir lo que se fecho entre medias (clock_correct).
 * ==========================================================================*/
#include "clock_module.h"
#include "../../config.h"

#include <sys/time.h>
#include <time.h>
#include <math.h>

/* En memoria RTC: sobreviven al deep sleep. Viven aqui y no en el .ino
 * porque son estado del reloj, no del ciclo. */
RTC_DATA_ATTR static bool     s_synced    = false;
RTC_DATA_ATTR static uint32_t s_last_sync = 0;

void clock_begin(esp_reset_reason_t reason) {
  if (reason != ESP_RST_DEEPSLEEP) {
    s_synced    = false;
    s_last_sync = 0;
  }
  if (clock_valid()) {
    char iso[24];
    clock_format_iso(clock_now(), iso, sizeof(iso));
    LOG("CLOCK: %s (sincronizada hace %lu s)", iso,
        (unsigned long)(clock_now() - s_last_sync));
  } else {
    LOG("CLOCK: sin hora hasta la primera subida correcta");
  }
}

bool clock_valid() {
  return s_synced && (uint32_t)time(nullptr) >= CLOCK_MIN_VALID_EPOCH;
}

uint32_t clock_now() {
  return clock_valid() ? (uint32_t)time(nullptr) : 0;
}

/* ---------------------------------------------------------------------------
 * Fecha HTTP -> epoch, sin depender de timegm()/TZ de newlib
 * -------------------------------------------------------------------------*/
/* Dias desde 1970-01-01 de una fecha del calendario gregoriano (algoritmo
 * days_from_civil de H. Hinnant: exacto, sin tablas ni bucles). */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int      era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

bool clock_parse_http_date(const char *s, uint32_t *epoch) {
  if (!s || !epoch) return false;
  static const char *kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  int  day, year, hh, mm, ss;
  char mon[4] = "";
  /* IMF-fixdate, el unico formato que un servidor actual debe generar. */
  if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) {
    return false;
  }
  int month = 0;
  for (int i = 0; i < 12; i++) {
    if (strcmp(mon, kMonths[i]) == 0) { month = i + 1; break; }
  }
  if (month == 0 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60) return false;

  const int64_t t = days_from_civil(year, (unsigned)month, (unsigned)day) * 86400 +
                    hh * 3600 + mm * 60 + ss;
  if (t < (int64_t)CLOCK_MIN_VALID_EPOCH || t > (int64_t)UINT32_MAX) return false;
  *epoch = (uint32_t)t;
  return true;
}

/* ---------------------------------------------------------------------------
 * Sincronizacion y correccion de la deriva
 * -------------------------------------------------------------------------*/
bool clock_sync(uint32_t server_epoch, ClockSync *out) {
  if (server_epoch < CLOCK_MIN_VALID_EPOCH) return false;

  const bool     was_valid = clock_valid();
  const uint32_t local_now = (uint32_t)time(nullptr);
  if (out) {
    out->continuous = was_valid;
    out->prev_sync  = s_last_sync;
    out->local_now  = local_now;
    out->server_now = server_epoch;
  }

  struct timeval tv = { (time_t)server_epoch, 0 };
  settimeofday(&tv, nullptr);
  s_synced    = true;
  s_last_sync = server_epoch;

  if (was_valid) {
    LOG("CLOCK: en hora con el Fog (el reloj local iba %+ld s)",
        (long)((int64_t)local_now - (int64_t)server_epoch));
  } else {
    LOG("CLOCK: primera hora del arranque, del Fog");
  }
  return true;
}

uint32_t clock_correct(const ClockSync &sync, uint32_t t_local) {
  if (!sync.continuous || t_local <= sync.prev_sync) return t_local;
  if (sync.local_now < sync.prev_sync + CLOCK_MIN_SPAN_S) return t_local;

  /* El reloj local conto (local_now - prev) mientras pasaban de verdad
   * (server_now - prev). Lo fechado en medio se reparte con esa razon. */
  const double k = (double)((int64_t)sync.server_now - sync.prev_sync) /
                   (double)((int64_t)sync.local_now - sync.prev_sync);
  if (fabs(k - 1.0) * 100.0 > CLOCK_MAX_DRIFT_PCT) return t_local;

  const int64_t t = (int64_t)sync.prev_sync +
                    llround((double)((int64_t)t_local - sync.prev_sync) * k);
  return t > 0 ? (uint32_t)t : t_local;
}

bool clock_format_iso(uint32_t epoch, char *out, size_t len) {
  const time_t t = (time_t)epoch;
  struct tm tm;
  if (!gmtime_r(&t, &tm)) return false;
  return strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tm) > 0;
}
