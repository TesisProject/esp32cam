/* ============================================================================
 *  clock_module.h  -  Hora real a partir de la cabecera Date del Fog
 *
 *  El ESP32 no tiene RTC con pila ni sale a Internet (NTP): la unica hora
 *  fiable que ve es la del Fog. Cada respuesta 2xx trae "Date" (uvicorn la
 *  envia por defecto) y con ella se pone en hora el reloj del sistema, que
 *  sigue contando durante el deep sleep con el oscilador RC del RTC.
 *
 *  Solo se usa para fechar las fotos que no se pudieron subir (captured_at).
 *  La foto actual no lleva hora: el Fog usa la suya al recibirla.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include <esp_system.h>

/* Lo que se sabia en el momento de una sincronizacion: con esto se mide la
 * deriva del reloj desde la anterior y se corrigen las horas guardadas. */
struct ClockSync {
  bool     continuous;   // habia una sincronizacion anterior en este arranque
  uint32_t prev_sync;    // epoch de esa sincronizacion anterior
  uint32_t local_now;    // hora local justo antes de corregirla
  uint32_t server_now;   // hora del Fog
};

/* Llamar al principio del ciclo. Fuera de un despertar de deep sleep la hora
 * se da por perdida (la memoria RTC no siempre se borra, pero tampoco se
 * puede fiar de ella tras un brownout o un encendido). */
void clock_begin(esp_reset_reason_t reason);

/* true si hay hora real: sincronizada desde el ultimo arranque en frio. */
bool clock_valid();

/* Epoch UTC actual; 0 si !clock_valid(). */
uint32_t clock_now();

/* "Sun, 18 Oct 2026 14:05:12 GMT" (RFC 7231) -> epoch UTC. false si no se
 * entiende o es anterior a CLOCK_MIN_VALID_EPOCH. */
bool clock_parse_http_date(const char *s, uint32_t *epoch);

/* Pone el reloj en hora. out (opcional) recibe lo necesario para corregir
 * las horas de lo capturado desde la sincronizacion anterior. */
bool clock_sync(uint32_t server_epoch, ClockSync *out);

/* Hora local t_local corregida con la deriva medida en sync. Devuelve
 * t_local sin tocar si no se puede medir (primera sincronizacion del
 * arranque, intervalo corto, deriva no creible) o si t_local es anterior a
 * la sincronizacion previa (ya se corrigio entonces). */
uint32_t clock_correct(const ClockSync &sync, uint32_t t_local);

/* epoch -> "2026-10-18T14:05:12Z" (out de 21 B como minimo). */
bool clock_format_iso(uint32_t epoch, char *out, size_t len);
