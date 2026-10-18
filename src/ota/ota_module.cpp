/* ============================================================================
 *  ota_module.cpp  -  Version, particion activa y rollback de la imagen
 *
 *  Como funciona el rollback (bootloader del core 3.3.12, compilado con
 *  CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y):
 *    - Una imagen recien escrita por OTA arranca en estado NEW y el
 *      bootloader la pasa a PENDING_VERIFY en ese primer arranque.
 *    - Si la app no la marca como valida, en el SIGUIENTE arranque el
 *      bootloader la ve aun en PENDING_VERIFY, la marca ABORTED y arranca la
 *      otra particion (la version anterior).
 *    - Despertar del deep sleep ES un arranque completo (pasa por el
 *      bootloader). Asi que una imagen que se duerme sin validarse vuelve
 *      sola a la anterior al despertar. Esa es justo la red de seguridad que
 *      se quiere: una version que no sabe subir fotos se descarta sola.
 *
 *  Por defecto, initArduino() la marca valida en cuanto arranca (antes de
 *  setup()), lo que anula la proteccion: una version que arranca pero no
 *  conecta o no sube se quedaria para siempre. De ahi el override de
 *  verifyRollbackLater() y la validacion manual tras un ciclo bueno.
 *
 *  Imagen flasheada por USB: boot_app0.bin deja otadata con la ranura app0
 *  en estado UNDEFINED (0xFFFFFFFF), no PENDING_VERIFY. El bootloader nunca
 *  hace rollback de UNDEFINED y ota_confirm_if_pending() no la toca (ni
 *  escribe en flash), asi que el flujo normal de desarrollo no cambia.
 * ==========================================================================*/
#include "ota_module.h"
#include "../../config.h"
#include "../settings/settings_module.h"

#include <esp_ota_ops.h>
#include <HTTPClient.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>

/* El core declara este hook como weak en esp32-hal-misc.c, un fichero C y
 * sin prototipo en ningun header. Sin extern "C", esta definicion C++
 * saldria con nombre mangleado (_Z19verifyRollbackLaterv), no sustituiria a
 * la weak y el core seguiria validando la imagen al arrancar, sin ningun
 * error ni aviso. true = "no verifiques en initArduino(), lo hago yo". */
extern "C" bool verifyRollbackLater() { return true; }

static const char *state_name(esp_ota_img_states_t s) {
  switch (s) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED (flasheo USB)";
    default:                         return "?";
  }
}

void ota_log_boot_info() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  const bool has_state =
      running && esp_ota_get_state_partition(running, &state) == ESP_OK;

  LOG("OTA: FW_VERSION=%d  particion activa=%s  estado=%s", FW_VERSION,
      running ? running->label : "?",
      has_state ? state_name(state) : "sin otadata");

  /* Si hubo rollback, la ranura descartada queda marcada como invalida (hasta
   * que un OTA la sobrescriba): es la forma de verlo desde el log, porque la
   * version que corre es la anterior y FW_VERSION no lo delata por si solo. */
  const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
  if (invalid) {
    LOG("OTA: la imagen de %s esta descartada (hubo rollback)", invalid->label);
  }
}

void ota_confirm_if_pending() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) return;

  /* Solo PENDING_VERIFY. Llamarlo en otro estado no rompe nada, pero con
   * UNDEFINED (flasheo USB) reescribiria otadata en flash sin necesidad. */
  if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    LOG("OTA: imagen de %s confirmada (ciclo completo con subida ok)",
        running->label);
  } else {
    /* Si falla, la imagen sigue pendiente y el proximo arranque volvera a la
     * anterior: preferible a quedarse con una version sin confirmar. */
    LOG("OTA: no se pudo confirmar la imagen (0x%x)", (unsigned)err);
  }
}

/* ===========================================================================
 * Cliente OTA (E5)
 *
 *  Orden de las comprobaciones, de la mas barata a la mas cara: version
 *  (gratis) -> ota_rej (lectura de NVS) -> rollback ya ocurrido con esta
 *  version (hash de la ranura descartada, solo si la hay) -> bateria ->
 *  firma (~decenas de ms, sin radio) -> descarga (~10-20 s de radio). Una
 *  firma mala no cuesta ninguna descarga.
 *
 *  Como el sha256 va dentro de lo firmado, verificada la firma basta con
 *  que el fichero descargado tenga ese hash. La cabecera x-SHA256 de la
 *  descarga no se usa: en la LAN se puede falsificar.
 * ==========================================================================*/
static const char *kSchemePrefix = "parkvision-fw/1";

/* Buffer de lectura/escritura: en .bss y no en la pila de loopTask (8 KB). */
static uint8_t s_buf[OTA_BUFFER_SIZE];

static void to_hex(const uint8_t *in, size_t n, char *out) {
  static const char *kHex = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i]     = kHex[in[i] >> 4];
    out[2 * i + 1] = kHex[in[i] & 0x0F];
  }
  out[2 * n] = '\0';
}

/* ECDSA P-256 sobre el SHA-256 de la cadena canonica. pem es la clave
 * publica (SubjectPublicKeyInfo); la firma, DER en Base64. */
static bool verify_signature(const char *pem, const char *hardware, uint32_t version,
                             uint32_t size, const char *sha256_hex, const char *sig_b64) {
  char msg[192];
  const int n = snprintf(msg, sizeof(msg), "%s|%s|%lu|%lu|%s", kSchemePrefix, hardware,
                         (unsigned long)version, (unsigned long)size, sha256_hex);
  if (n <= 0 || (size_t)n >= sizeof(msg)) return false;

  uint8_t hash[32];
  if (mbedtls_sha256((const unsigned char *)msg, (size_t)n, hash, 0) != 0) return false;

  uint8_t sig[80];
  size_t  sig_len = 0;
  if (mbedtls_base64_decode(sig, sizeof(sig), &sig_len, (const unsigned char *)sig_b64,
                            strlen(sig_b64)) != 0) {
    return false;
  }

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  /* El parser PEM de mbedTLS exige contar el '\0' final en la longitud. */
  int rc = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)pem, strlen(pem) + 1);
  if (rc == 0 && !mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECDSA)) rc = -1;
  if (rc == 0) rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sig_len);
  mbedtls_pk_free(&pk);
  return rc == 0;
}

bool ota_self_test() {
  /* Copia literal del vector de la nube. Su clave privada se borro al
   * generarlo: este par no firma nada en ningun entorno. */
  static const char *kPem =
      "-----BEGIN PUBLIC KEY-----\n"
      "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAExGjFJSR0jKPg9ZbDusjwiqsLnuLI\n"
      "N3+5Bym5sbiqgIdo6NfQEIGFzJEIKgnwJqcWubRj3TUzBxiVuBz6MaSlJw==\n"
      "-----END PUBLIC KEY-----\n";
  static const char *kSha =
      "3a9dac3fc8791fd3f17d067d8ba2215cf7640673b64269b7c098d453455fff81";
  static const char *kSig =
      "MEYCIQDx9uSpnfcHVjYiO+VEbrvQ1Tvw9VPtd1JJg+DiBrcTVAIhANy4t5qVGBQKfJ4aQQASDxJZ09yaRoNmS1j1xDWWwKXL";

  const uint32_t t0       = millis();
  const bool     good     = verify_signature(kPem, "esp32cam-aithinker", 4, 1132096, kSha, kSig);
  /* La misma firma presentada como version 5 tiene que fallar: es la
   * garantia de firmar la cadena y no el binario. */
  const bool     tampered = verify_signature(kPem, "esp32cam-aithinker", 5, 1132096, kSha, kSig);
  const bool     ok       = good && !tampered;
  LOG("OTA: autotest de firma %s (vector v4 %s, v5 %s, %lu ms)", ok ? "OK" : "FALLIDO",
      good ? "valida" : "NO valida", tampered ? "VALIDA (mal)" : "rechazada",
      (unsigned long)(millis() - t0));
  return ok;
}

/* true si los primeros `size` bytes de la ranura tienen ese SHA-256: el
 * OTA escribe el .bin tal cual, asi que es el hash del fichero. */
static bool partition_holds(const esp_partition_t *p, uint32_t size, const char *sha256_hex) {
  if (!p || size > p->size) return false;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  bool ok = mbedtls_sha256_starts(&ctx, 0) == 0;
  for (uint32_t off = 0; ok && off < size; off += sizeof(s_buf)) {
    const size_t n = min((uint32_t)sizeof(s_buf), size - off);
    ok = esp_partition_read(p, off, s_buf, n) == ESP_OK &&
         mbedtls_sha256_update(&ctx, s_buf, n) == 0;
  }
  uint8_t hash[32];
  ok = ok && mbedtls_sha256_finish(&ctx, hash) == 0;
  mbedtls_sha256_free(&ctx);
  if (!ok) return false;
  char hex[65];
  to_hex(hash, sizeof(hash), hex);
  return strcmp(hex, sha256_hex) == 0;
}

/* Descarga a `dst` por streaming, sin guardar el .bin en RAM. Comprueba
 * Content-Length == size y el SHA-256 de lo recibido; esp_ota_end valida
 * ademas la imagen (byte magico, checksum, digest anexado). */
static bool download_to(const char *fog_host, uint16_t fog_port, const char *api_key,
                        const char *api_token, const FirmwareOffer &o,
                        const esp_partition_t *dst) {
  const String url = String("http://") + fog_host + ":" + (unsigned)fog_port + o.url;
  HTTPClient http;
  http.begin(url);
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);
  http.addHeader("X-API-Key", api_key);
  http.addHeader("X-API-Token", api_token);

  LOG("OTA: GET %s", o.url);
  const uint32_t t_req = millis();
  const int      code  = http.GET();
  if (code != 200) {
    /* 403: no asignada a esta camara; 404/503: el Fog no tiene el binario
     * (o lo tiene corrupto y lo borra). Negativo: no llego respuesta (-5 =
     * el otro lado cerro la conexion sin responder, -11 = timeout). */
    LOG("OTA: descarga -> %d (%s) tras %lu ms", code,
        code < 0 ? http.errorToString(code).c_str() : "HTTP", (unsigned long)(millis() - t_req));
    http.end();
    return false;
  }
  const int len = http.getSize();   // -1 si no hay Content-Length (chunked)
  if (len != (int)o.size) {
    LOG("OTA: Content-Length %d != size %lu", len, (unsigned long)o.size);
    http.end();
    return false;
  }

  esp_ota_handle_t h = 0;
  /* Borrado sector a sector al escribir, no la ranura entera al empezar:
   * borrar 1,2 MB de golpe son varios segundos sin leer del socket. */
  esp_err_t err = esp_ota_begin(dst, OTA_WITH_SEQUENTIAL_WRITES, &h);
  if (err != ESP_OK) {
    LOG("OTA: esp_ota_begin fallo (0x%x)", (unsigned)err);
    http.end();
    return false;
  }

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);

  auto          *stream   = http.getStreamPtr();   // NetworkClient en el core 3.x
  const uint32_t t0       = millis();
  uint32_t       last_rx  = t0;
  uint32_t       got      = 0;
  uint32_t       next_log = 256 * 1024;
  bool           ok       = true;
  while (got < o.size) {
    if (millis() - t0 > OTA_DOWNLOAD_TIMEOUT_MS || millis() - last_rx > OTA_HTTP_TIMEOUT_MS) {
      LOG("OTA: descarga parada en %lu de %lu bytes", (unsigned long)got, (unsigned long)o.size);
      ok = false;
      break;
    }
    const size_t avail = stream->available();
    if (avail == 0) {
      delay(1);
      continue;
    }
    const size_t want = min(min(avail, sizeof(s_buf)), (size_t)(o.size - got));
    const int    n    = stream->readBytes(s_buf, want);
    if (n <= 0) continue;
    mbedtls_sha256_update(&ctx, s_buf, (size_t)n);
    err = esp_ota_write(h, s_buf, (size_t)n);
    if (err != ESP_OK) {
      /* El primer bloque falla aqui si no es una imagen de app (0xE9). */
      LOG("OTA: esp_ota_write fallo (0x%x) en el byte %lu", (unsigned)err, (unsigned long)got);
      ok = false;
      break;
    }
    got    += (uint32_t)n;
    last_rx = millis();
    if (got >= next_log) {
      LOG("OTA: %lu KB", (unsigned long)(got / 1024));
      next_log += 256 * 1024;
    }
  }
  http.end();

  uint8_t hash[32];
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  if (ok) {
    char hex[65];
    to_hex(hash, sizeof(hash), hex);
    if (strcmp(hex, o.sha256) != 0) {
      LOG("OTA: SHA-256 no coincide (recibido %.16s..., firmado %.16s...)", hex, o.sha256);
      ok = false;
    }
  }
  if (!ok) {
    esp_ota_abort(h);
    return false;
  }
  err = esp_ota_end(h);
  if (err != ESP_OK) {
    LOG("OTA: la imagen no es valida (esp_ota_end 0x%x)", (unsigned)err);
    return false;
  }
  LOG("OTA: %lu bytes descargados y verificados en %lu ms", (unsigned long)got,
      (unsigned long)(millis() - t0));
  return true;
}

OtaResult ota_handle_offer(const char *fog_host, uint16_t fog_port,
                           const char *api_key, const char *api_token,
                           const FirmwareOffer &offer, bool battery_ok) {
  if (offer.version <= (uint32_t)FW_VERSION) {
    LOG("OTA: asignada v%lu, esta es la v%d: nada que hacer", (unsigned long)offer.version,
        FW_VERSION);
    return OTA_IDLE;
  }
  if (offer.version == settings_rejected_ota_version()) {
    LOG("OTA: v%lu ya se probo y hubo rollback (ota_rej): no se reinstala",
        (unsigned long)offer.version);
    return OTA_IDLE;
  }

  /* Rollback ya ocurrido con ESTA version: el bootloader dejo la ranura
   * descartada tal cual. Si contiene exactamente el .bin ofrecido, esa
   * version no llego a confirmarse: se apunta para no entrar en un bucle de
   * descarga y rollback cada hora. */
  const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
  if (invalid && partition_holds(invalid, offer.size, offer.sha256)) {
    LOG("OTA: la v%lu esta en %s, descartada por rollback: se apunta en ota_rej",
        (unsigned long)offer.version, invalid->label);
    settings_set_rejected_ota_version(offer.version);
    return OTA_IDLE;
  }

  if (OTA_SIGNING_PUBLIC_KEY_PEM[0] == '\0') {
    LOG("OTA: hay v%lu asignada, pero el firmware no tiene clave publica compilada "
        "(OTA_SIGNING_PUBLIC_KEY_PEM): OTA desactivado", (unsigned long)offer.version);
    return OTA_IDLE;
  }
  if (!battery_ok) {
    LOG("OTA: v%lu asignada, pero la bateria esta por debajo de %d mV: se espera",
        (unsigned long)offer.version, OTA_MIN_BATTERY_MV);
    return OTA_IDLE;
  }
  if (!verify_signature(OTA_SIGNING_PUBLIC_KEY_PEM, OTA_HARDWARE_ID, offer.version,
                        offer.size, offer.sha256, offer.signature)) {
    /* No se apunta en ota_rej: puede ser un error de configuracion del Fog o
     * de la clave, y corregirlo no debe obligar a publicar otra version. */
    LOG("OTA: firma NO valida para v%lu: no se descarga", (unsigned long)offer.version);
    return OTA_IDLE;
  }

  const esp_partition_t *dst = esp_ota_get_next_update_partition(nullptr);
  if (!dst || offer.size > dst->size) {
    LOG("OTA: v%lu (%lu bytes) no cabe en la ranura libre", (unsigned long)offer.version,
        (unsigned long)offer.size);
    return OTA_IDLE;
  }
  LOG("OTA: firma valida, instalando v%lu (%lu bytes) en %s", (unsigned long)offer.version,
      (unsigned long)offer.size, dst->label);
  if (!download_to(fog_host, fog_port, api_key, api_token, offer, dst)) return OTA_RETRY;

  const esp_err_t err = esp_ota_set_boot_partition(dst);
  if (err != ESP_OK) {
    LOG("OTA: esp_ota_set_boot_partition fallo (0x%x)", (unsigned)err);
    return OTA_RETRY;
  }
  /* La imagen nueva arranca en PENDING_VERIFY: si no consigue una subida
   * 2xx antes de su siguiente arranque, el bootloader vuelve a esta. */
  LOG("OTA: v%lu activada en %s, reiniciando", (unsigned long)offer.version, dst->label);
  Serial.flush();
  esp_restart();
  return OTA_IDLE;   // no se llega: esp_restart() no vuelve
}
