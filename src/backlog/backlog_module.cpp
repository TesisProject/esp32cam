/* ============================================================================
 *  backlog_module.cpp  -  Cola de fotos atrasadas sobre LittleFS
 *
 *  Por que LittleFS y no SPIFFS: LittleFS aguanta un corte de alimentacion
 *  en mitad de una escritura (copia antes de soltar lo viejo y sus metadatos
 *  son atomicos) y reparte el desgaste. SPIFFS esta obsoleto en ESP-IDF.
 *
 *  Una foto a medias no puede quedar con nombre definitivo: se escribe en
 *  BACKLOG_DIR/tmp y solo al terminar se renombra (rename es atomico en
 *  LittleFS). Un tmp que sobreviva es de una escritura cortada y se borra al
 *  montar.
 *
 *  Desgaste: solo se escribe cuando una subida falla. En el peor caso (corte
 *  permanente, una foto por ciclo) son ~72 borrados por bloque y dia; con
 *  100 000 ciclos por sector, ~3,8 anos de corte continuo.
 * ==========================================================================*/
#include "backlog_module.h"

#include <LittleFS.h>
#include <esp_heap_caps.h>

static const size_t   kBlock     = 4096;   // bloque de LittleFS = sector de la flash
static const size_t   kPathLen   = 24;     // "/q1/1760800000.bin" + '\0'
static const size_t   kScanMax   = 16;     // > BACKLOG_MAX_FILES: margen para basura
static const char    *kTmpPath   = BACKLOG_DIR "/tmp";
static const uint32_t kMagic     = 0x31515650;   // "PVQ1" en little endian
static const uint16_t kVersion   = 1;

static_assert(BACKLOG_MAX_FILES < kScanMax, "BACKLOG_MAX_FILES debe caber en el escaneo");

/* Cabecera fija delante del JPEG. Cambiar su forma = directorio nuevo
 * (BACKLOG_DIR): un firmware no tiene que saber leer formatos ajenos. */
struct BacklogHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t jpeg_len;
  int32_t  fw_version;
  int32_t  battery_mv;
  int16_t  reset_reason;
  uint8_t  jpeg_quality;
  uint8_t  battery_valid;
  char     framesize[CAP_FRAMESIZE_MAX_LEN + 1];
};
static_assert(sizeof(BacklogHeader) == 32, "BacklogHeader: formato de /q1, no cambiarlo");

/* Contador de pendientes en memoria RTC: evita montar LittleFS en cada ciclo
 * (unas decenas de ms y lecturas de flash) solo para saber que no hay nada. */
RTC_DATA_ATTR static uint32_t s_count       = 0;
RTC_DATA_ATTR static bool     s_count_valid = false;

static bool s_mounted      = false;
static bool s_mount_failed = false;   // no reintentar en el mismo ciclo

/* ---------------------------------------------------------------------------
 * Montaje, nombres y escaneo
 * -------------------------------------------------------------------------*/
static bool mount() {
  if (s_mounted) return true;
  if (s_mount_failed) return false;

  const uint32_t t0 = millis();
  /* formatOnFail: la primera vez la particion tiene restos de otros
   * firmwares (o nada) y hay que formatearla (~1,5 s, una sola vez). Si mas
   * adelante se corrompe, tambien: se pierden las fotos pendientes, que solo
   * son un buffer. */
  if (!LittleFS.begin(true, "/littlefs", 2, BACKLOG_PARTITION_LABEL)) {
    LOG("BACKLOG: no se pudo montar LittleFS en la particion '%s'", BACKLOG_PARTITION_LABEL);
    s_mount_failed = true;
    return false;
  }
  if (!LittleFS.exists(BACKLOG_DIR)) LittleFS.mkdir(BACKLOG_DIR);
  if (LittleFS.exists(kTmpPath)) {
    LOG("BACKLOG: se borra una escritura cortada (tmp)");
    LittleFS.remove(kTmpPath);
  }
  s_mounted = true;
  LOG("BACKLOG: LittleFS montado en %lu ms (%u de %u bytes usados)",
      (unsigned long)(millis() - t0), (unsigned)LittleFS.usedBytes(),
      (unsigned)LittleFS.totalBytes());
  return true;
}

static size_t free_bytes() {
  const size_t total = LittleFS.totalBytes();
  const size_t used  = LittleFS.usedBytes();
  return used < total ? total - used : 0;
}

static void make_path(uint32_t t, char *out) {
  snprintf(out, kPathLen, "%s/%010lu.bin", BACKLOG_DIR, (unsigned long)t);
}

/* "1760800000.bin" -> 1760800000. Diez cifras: el orden alfabetico es el
 * cronologico hasta el ano 2286. */
static bool parse_name(const char *name, uint32_t *t) {
  if (strlen(name) != 14 || strcmp(name + 10, ".bin") != 0) return false;
  uint32_t v = 0;
  for (int i = 0; i < 10; i++) {
    if (name[i] < '0' || name[i] > '9') return false;
    v = v * 10 + (uint32_t)(name[i] - '0');
  }
  *t = v;
  return v != 0;
}

static void remove_path(const char *path) {
  if (LittleFS.remove(path) && s_count_valid && s_count > 0) s_count--;
}

/* Horas de captura de las fotos validas, de la mas antigua a la mas nueva.
 * Lo que no tenga nombre de foto se borra: es basura de un firmware
 * anterior o de una escritura a medias y nadie lo va a leer. */
static size_t scan(uint32_t *out, size_t max) {
  if (!LittleFS.exists(BACKLOG_DIR)) return 0;
  File dir = LittleFS.open(BACKLOG_DIR);
  if (!dir || !dir.isDirectory()) return 0;

  char   junk[kScanMax][kPathLen];
  size_t n = 0, nj = 0;
  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    const char *name  = e.name();
    const char *slash = strrchr(name, '/');   // por si el core da la ruta entera
    if (slash) name = slash + 1;
    uint32_t t;
    if (!e.isDirectory() && parse_name(name, &t)) {
      if (n < max) out[n++] = t;
    } else if (!e.isDirectory() && nj < kScanMax) {
      snprintf(junk[nj++], kPathLen, "%s/%s", BACKLOG_DIR, name);
    }
    e.close();
  }
  dir.close();

  for (size_t i = 0; i < nj; i++) {
    LOG("BACKLOG: se borra %s (no es una foto)", junk[i]);
    LittleFS.remove(junk[i]);
  }

  /* Insercion: son como mucho BACKLOG_MAX_FILES elementos. */
  for (size_t i = 1; i < n; i++) {
    const uint32_t v = out[i];
    size_t j = i;
    for (; j > 0 && out[j - 1] > v; j--) out[j] = out[j - 1];
    out[j] = v;
  }
  return n;
}

/* ---------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------*/
void backlog_begin(esp_reset_reason_t reason) {
  if (reason != ESP_RST_DEEPSLEEP) s_count_valid = false;
}

uint32_t backlog_pending() {
  if (!s_count_valid) {
    uint32_t ts[kScanMax];
    /* Sin LittleFS se da por vacia y no se reintenta hasta el siguiente
     * arranque en frio: montar una particion averiada en cada ciclo solo
     * gastaria bateria. */
    s_count       = mount() ? (uint32_t)scan(ts, kScanMax) : 0;
    s_count_valid = true;
    if (s_count) LOG("BACKLOG: %lu fotos pendientes", (unsigned long)s_count);
  }
  return s_count;
}

bool backlog_save(uint32_t captured_at, const uint8_t *jpeg, size_t len,
                  const BacklogMeta &meta) {
  if (!jpeg || len == 0 || captured_at == 0) return false;
  if (!mount()) return false;

  const size_t need = ((sizeof(BacklogHeader) + len + kBlock - 1) / kBlock) * kBlock +
                      BACKLOG_FREE_MARGIN_BYTES;
  if (need > LittleFS.totalBytes()) {
    LOG("BACKLOG: la foto (%u bytes) no cabe ni con la particion vacia", (unsigned)len);
    return false;
  }

  /* Se hace sitio borrando las mas antiguas: con 5-15 min de capacidad, la
   * foto mas reciente es la que mas vale. */
  uint32_t ts[kScanMax];
  const size_t n = scan(ts, kScanMax);
  size_t first = 0;
  while (first < n && (n - first >= BACKLOG_MAX_FILES || free_bytes() < need)) {
    char old[kPathLen];
    make_path(ts[first], old);
    LittleFS.remove(old);
    LOG("BACKLOG: lleno, se descarta la mas antigua (%s)", old);
    first++;
  }
  s_count       = (uint32_t)(n - first);
  s_count_valid = true;
  if (free_bytes() < need) {
    LOG("BACKLOG: sin espacio (%u libres, %u necesarios)", (unsigned)free_bytes(),
        (unsigned)need);
    return false;
  }

  BacklogHeader h = {};
  h.magic         = kMagic;
  h.version       = kVersion;
  h.header_size   = sizeof(BacklogHeader);
  h.jpeg_len      = (uint32_t)len;
  h.fw_version    = meta.fw_version;
  h.battery_mv    = meta.battery_mv;
  h.reset_reason  = meta.reset_reason;
  h.jpeg_quality  = meta.jpeg_quality;
  h.battery_valid = meta.battery_valid ? 1 : 0;
  strlcpy(h.framesize, meta.framesize, sizeof(h.framesize));

  const uint32_t t0 = millis();
  File f = LittleFS.open(kTmpPath, FILE_WRITE);
  if (!f) {
    LOG("BACKLOG: no se pudo crear %s", kTmpPath);
    return false;
  }
  const bool ok = f.write((const uint8_t *)&h, sizeof(h)) == sizeof(h) &&
                  f.write(jpeg, len) == len;
  f.close();
  if (!ok) {
    LittleFS.remove(kTmpPath);
    LOG("BACKLOG: fallo al escribir la foto (%u bytes)", (unsigned)len);
    return false;
  }

  char path[kPathLen];
  make_path(captured_at, path);
  if (LittleFS.exists(path)) LittleFS.remove(path);   // misma hora: la nueva manda
  if (!LittleFS.rename(kTmpPath, path)) {
    LittleFS.remove(kTmpPath);
    LOG("BACKLOG: fallo al renombrar a %s", path);
    return false;
  }
  s_count++;
  LOG("BACKLOG: guardada %s (%u bytes, %lu ms); pendientes %lu, %u bytes libres",
      path, (unsigned)len, (unsigned long)(millis() - t0), (unsigned long)s_count,
      (unsigned)free_bytes());
  return true;
}

static uint8_t *alloc_jpeg(size_t len) {
  uint8_t *p = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
}

/* Lee y valida una foto. false si esta danada (se borrara). */
static bool read_file(const char *path, uint32_t captured_at, BacklogItem &out) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;

  BacklogHeader h;
  const size_t size = f.size();
  bool ok = size > sizeof(h) &&
            f.read((uint8_t *)&h, sizeof(h)) == sizeof(h) &&
            h.magic == kMagic && h.version == kVersion &&
            h.header_size == sizeof(h) && h.jpeg_len == size - sizeof(h);
  uint8_t *jpeg = nullptr;
  if (ok) {
    jpeg = alloc_jpeg(h.jpeg_len);
    if (!jpeg) {
      /* No es culpa del fichero: se deja para otro ciclo. */
      LOG("BACKLOG: sin memoria para leer %s (%lu bytes)", path, (unsigned long)h.jpeg_len);
      f.close();
      return true;   // ni foto ni borrado: out.jpeg queda a nullptr
    }
    /* Un JPEG empieza siempre por FF D8 (SOI): atrapa un fichero truncado
     * o con basura sin gastar una subida en el. */
    ok = f.read(jpeg, h.jpeg_len) == h.jpeg_len && jpeg[0] == 0xFF && jpeg[1] == 0xD8;
  }
  f.close();
  if (!ok) {
    free(jpeg);
    return false;
  }

  strlcpy(out.path, path, sizeof(out.path));
  out.captured_at        = captured_at;
  out.jpeg               = jpeg;
  out.jpeg_len           = h.jpeg_len;
  out.meta.fw_version    = h.fw_version;
  out.meta.reset_reason  = h.reset_reason;
  out.meta.jpeg_quality  = h.jpeg_quality;
  out.meta.battery_valid = h.battery_valid != 0;
  out.meta.battery_mv    = h.battery_mv;
  memcpy(out.meta.framesize, h.framesize, sizeof(out.meta.framesize));
  out.meta.framesize[sizeof(out.meta.framesize) - 1] = '\0';
  return true;
}

bool backlog_load_oldest(BacklogItem &out) {
  memset(&out, 0, sizeof(out));
  if (backlog_pending() == 0 || !mount()) return false;

  uint32_t ts[kScanMax];
  const size_t n = scan(ts, kScanMax);
  s_count       = (uint32_t)n;
  s_count_valid = true;

  const uint32_t now = clock_now();
  for (size_t i = 0; i < n; i++) {
    char path[kPathLen];
    make_path(ts[i], path);
    if (now && ts[i] + BACKLOG_MAX_AGE_S < now) {
      LOG("BACKLOG: %s tiene mas de %lu s, se descarta", path,
          (unsigned long)BACKLOG_MAX_AGE_S);
      remove_path(path);
      continue;
    }
    if (read_file(path, ts[i], out)) return out.jpeg != nullptr;
    LOG("BACKLOG: %s esta danada, se descarta", path);
    remove_path(path);
  }
  return false;
}

void backlog_release(BacklogItem &item) {
  free(item.jpeg);
  item.jpeg     = nullptr;
  item.jpeg_len = 0;
}

void backlog_remove(BacklogItem &item) {
  if (item.path[0] && mount()) remove_path(item.path);
  backlog_release(item);
}

void backlog_retime(const ClockSync &sync) {
  if (!sync.continuous || backlog_pending() == 0 || !mount()) return;

  uint32_t ts[kScanMax];
  const size_t n = scan(ts, kScanMax);
  for (size_t i = 0; i < n; i++) {
    const uint32_t fixed = clock_correct(sync, ts[i]);
    if (fixed == ts[i]) continue;
    char from[kPathLen], to[kPathLen];
    make_path(ts[i], from);
    make_path(fixed, to);
    if (LittleFS.exists(to)) continue;   // otra foto con esa hora: mejor no tocar
    if (LittleFS.rename(from, to)) {
      LOG("BACKLOG: hora corregida %s -> %s (%+ld s)", from, to,
          (long)((int64_t)fixed - (int64_t)ts[i]));
    }
  }
}

bool backlog_erase_all() {
  if (!mount()) return false;
  uint32_t ts[kScanMax];
  const size_t n = scan(ts, kScanMax);   // de paso borra tmp y la basura
  bool ok = true;
  for (size_t i = 0; i < n; i++) {
    char path[kPathLen];
    make_path(ts[i], path);
    ok = LittleFS.remove(path) && ok;
  }
  s_count       = 0;
  s_count_valid = true;
  return ok;
}
