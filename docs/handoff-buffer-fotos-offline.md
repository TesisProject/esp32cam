# Handoff — Fotos atrasadas: buffer local en la ESP32-CAM

**Fecha:** 2026-10-18 · **Origen:** `esp32cam` · **Destinatarios:** `cloud-service` (occupancy) y `fog-application` (vision)

**Orden de implementación:** primero la nube (§2), después el Fog (§3) y por último el firmware (§4).
El firmware no empieza a enviar fotos atrasadas hasta que §2 esté desplegado. Sin §2, una foto
atrasada **sobrescribe el estado actual** de las plazas.

---

## 1. Contexto

Hoy, si una subida falla (WiFi caído, router reiniciándose, Fog reiniciándose), la foto de ese ciclo
se pierde: el firmware no guarda nada entre ciclos. Lo único que llega después es
`wifi_fail_streak`, que dice cuántos ciclos faltaron, pero no qué pasó en ellos.

**Qué va a hacer el firmware (opción A):**

- Si la subida de la foto actual falla, la guarda en la partición `spiffs` de la flash interna
  (128 KB, LittleFS). No hay microSD: su pin D3 está cableado a GPIO13, que mide la batería.
- En el siguiente ciclo con conexión, sube **primero la foto actual** y después **como mucho 3
  fotos atrasadas**, de la más antigua a la más reciente, cada una con su `captured_at`.
- **Capacidad real:** las fotos pesan 30–40 KB, y hasta 70 KB en el peor caso. Caben de 1 a 3
  fotos, es decir, de **5 a 15 minutos de corte** con capturas cada 5 min. Si el buffer se llena, se
  descarta la foto más antigua.

**Consecuencia para los receptores:** van a llegar frames con `captured_at` de hasta ~15 min (en la
práctica, hasta unas horas si el Fog vuelve tarde) **después** de frames más recientes de la misma
cámara. Una foto atrasada es **historial**: nunca debe cambiar el estado actual de una plaza.

### Problemas que hay hoy en el código

| # | Dónde | Problema |
|---|---|---|
| P1 | `cloud-service` · `MonitoredSpaceCommandService.applyStatus()` (l. 72) y `MonitoredSpace.applyStatus()` (l. 100) | El estado de la plaza se sobrescribe **por orden de llegada**, sin comparar `occurredAt`. Una foto atrasada devuelve la plaza a un estado viejo. |
| P2 | `cloud-service` · `OccupancyIngestionService.handle()` (l. 68-84) | El resumen de zona se calcula con el estado **actual** y se guarda con el `occurredAt` **antiguo**: queda un `ZoneOccupancyLog` con datos mezclados. |
| P3 | `fog-application` · `camera_router.upload_frame` (l. 32) | `captured_at` se acepta sin validar: una fecha futura o de 1970 entra tal cual y la nube la rechaza más tarde (`OccupancyLog`, tolerancia de 5 min hacia el futuro), en segundo plano y sin que nadie se entere. |
| P4 | `fog-application` · `service.save_frame` | No hay idempotencia. Si el Fog acepta la foto pero la respuesta se pierde por timeout, el firmware la reenvía y queda guardada (y procesada) dos veces. |
| P5 | `fog-application` · `service._maybe_upload_frame` | El throttle de `screenshot_interval_minutes` se gasta con la primera foto que se procesa, aunque sea una atrasada. La foto actual se queda sin subir hasta el siguiente ciclo. |
| P6 | `fog-application` · `service.list_frames` (l. 584), `list_telemetry` (l. 618), `list_detections` (l. 630) | Se ordena por `received_at` / `detected_at`: una foto atrasada aparece como "la última" en las vistas de administración. |

Además, `process_frame` corre como `BackgroundTasks` por petición: dos subidas seguidas se procesan
**en paralelo** y no hay garantía de que la nube reciba los eventos en el orden en que el firmware
los envió. Por eso la solución tiene que estar en la nube (comparar `occurredAt`) y no en el orden
de envío.

---

## 2. `cloud-service` (occupancy)

### HU-C1 — Un evento atrasado no cambia el estado actual de la plaza

> **Como** usuario del mapa de ocupación, **quiero** que una foto atrasada no reemplace el estado de
> una plaza ya actualizado con una foto más reciente, **para** ver siempre el estado real y no uno
> de hace 15 minutos.

**Cambio propuesto**

- `MonitoredSpace`: nuevo campo `statusOccurredAt` (`Instant`, nullable), columna
  `status_occurred_at timestamptz NULL`, en la migración **`V15__estado_plaza_occurred_at.sql`**
  (o la siguiente libre).
- `MonitoredSpace.applyStatus(SpotStatus status, Instant occurredAt)` devuelve `boolean`:
  - aplica el estado si `statusOccurredAt == null` **o** `!occurredAt.isBefore(statusOccurredAt)`
    (un empate se aplica: es idempotente);
  - en otro caso no toca nada y devuelve `false`.
- `MonitoredSpaceCommandService.applyStatus(...)` recibe el `occurredAt` y devuelve el resumen de
  zona **solo si el estado se aplicó**.
- Hay que cambiar los **dos** llamadores: `OccupancyIngestionService` l. 53 (evento suelto) y l. 78
  (lote por cámara).
- El `OccupancyLog` se guarda **siempre**, atrasado o no: es el historial, y para la tesis es el dato
  que se recupera.

**Criterios de aceptación**

1. Llega un lote con `occurredAt = T` y después otro con `occurredAt = T − 10 min` para la misma
   plaza → el estado de la plaza es el de `T`; los dos `OccupancyLog` existen.
2. Plazas existentes con `status_occurred_at` nulo → el primer evento se aplica (no hace falta
   rellenar la columna en la migración).
3. Dos eventos con el mismo `occurredAt` → se aplica el último en llegar.
4. Hay tests unitarios de `MonitoredSpace.applyStatus` y un test de `OccupancyIngestionService` con
   un lote atrasado.

### HU-C2 — El historial de zona no mezcla estados de momentos distintos

> **Como** prediction-engine, **quiero** que cada `ZoneOccupancyLog` refleje la ocupación real en
> su `occurredAt`, **para** no entrenar los pronósticos con datos mezclados.

**Cambio propuesto (mínimo)**

- En `handle(RecordCameraOccupancyBatchCommand)`, guardar el `ZoneOccupancyLog` solo de las zonas
  en las que **al menos una** detección se aplicó (HU-C1). Un lote completamente atrasado deja solo
  `OccupancyLog` por plaza, sin resumen de zona.

**Opcional (fase 2, a decidir en la nube):** reconstruir el resumen de zona en el instante `T` del
lote atrasado a partir de `OccupancyLog` (por cada plaza de la zona, el último log con
`occurredAt ≤ T`) e insertarlo con ese `occurredAt`. Rellena el hueco en el historial de zona, pero
cuesta una consulta por zona y por lote atrasado. Con 5–15 min de buffer, el hueco es como mucho de
3 puntos.

**Criterios de aceptación**

1. Un lote atrasado no crea ningún `ZoneOccupancyLog` (versión mínima).
2. `findFirstByZoneIdOrderByOccurredAtDesc` sigue devolviendo el resumen más reciente.

### Lo que no cambia en la nube

- Los endpoints y DTOs no cambian: `occurredAt` ya viaja en `POST /cameras/{id}/events` y
  `capturedAt` en `POST /cameras/{id}/frames`.
- La foto "más reciente" ya se elige por `capturedAt` (`findFirstByCameraIdOrderByCapturedAtDesc`),
  así que una foto atrasada subida a R2 no pasa por delante de la actual.
- La tolerancia de 5 min hacia el futuro (`CLOCK_SKEW_TOLERANCE`) se mantiene. El Fog (HU-F1) y el
  firmware (§4) se encargan de no enviar fechas futuras.

> **Ojo:** `cloud-service` tiene ahora cambios sin commit (`.env.example`, `DEPLOY.md`,
> `docker-compose.yml`, `docs/plan-firmware-ota.md`) de otra sesión. Ninguno toca `occupancy`.

---

## 3. `fog-application` (vision)

### HU-F1 — Validar `captured_at` al recibir la foto

> **Como** Fog, **quiero** rechazar en el momento una foto con una hora de captura imposible,
> **para** que el firmware la descarte en lugar de reenviarla siempre, y para que la nube no la
> rechace después en segundo plano sin que nadie lo vea.

**Cambio propuesto** en `camera_router.upload_frame`, antes de `save_frame`:

- Un `captured_at` sin zona horaria se interpreta como UTC (el firmware siempre envía `…Z`).
- `captured_at > ahora + 5 min` → **`422`** (misma tolerancia que la nube).
- `captured_at < ahora − BACKLOG_MAX_AGE_HOURS` → **`422`**. Nuevo ajuste en `config.py`, defecto
  `24`.
- Sin `captured_at` → igual que hoy (se usa `received_at`).

El firmware borra una foto atrasada que reciba un 2xx **o un 4xx**, y la conserva solo ante un 5xx o
un error de red (§4). Un `422` aquí no hace perder un frame bueno: avisa de que esa foto no se puede
usar.

**Criterios de aceptación:** tests con una fecha futura (+10 min → 422), una muy antigua
(−25 h → 422), una válida (−10 min → 202) y sin el campo (→ 202).

### HU-F2 — Subida idempotente

> **Como** cámara que reintenta una foto cuya respuesta se perdió, **quiero** que el Fog reconozca
> el duplicado, **para** que la ocupación no se registre dos veces.

**Cambio propuesto**

- Si llega `captured_at` y ya existe un `CameraFrame` con el mismo `(camera_id, captured_at)` →
  responder **`202`** con el `frame_id` existente, sin guardar la imagen ni encolar
  `process_frame`.
- Índice único parcial `(camera_id, captured_at) WHERE captured_at IS NOT NULL`, con su migración.
  Los frames sin `captured_at` (la foto actual) no se deduplican.

**Criterios de aceptación:** dos POST iguales con el mismo `captured_at` → la misma respuesta, un
solo `CameraFrame` y una sola `Detection`.

### HU-F3 — Las fotos atrasadas no gastan el throttle de subida a la nube

> **Como** usuario del frontend, **quiero** que la foto de la cámara en la nube sea la del momento y
> no una atrasada, **para** que la foto y el estado de las plazas coincidan.

**Cambio propuesto** en `_maybe_upload_frame`: un frame es **atrasado** si tiene `captured_at` y
`received_at − captured_at > 60 s`. Para un frame atrasado:

- **Recomendado:** no subir la foto a la nube. La detección sí se reporta (`_report_to_cloud`) con
  su `occurredAt`, que es lo que alimenta el historial.
- **Alternativa:** subirla sin leer ni avanzar `_last_frame_upload_at`. Cuesta espacio en R2 y
  ancho de banda a cambio de tener la evidencia visual del hueco.

**Criterios de aceptación:** una ráfaga de una foto actual y 3 atrasadas → la nube recibe la foto
actual (con la variante recomendada, solo esa) y los 4 eventos de ocupación.

### HU-F4 — Las vistas de administración ordenan por hora de captura

> **Como** administrador del Fog, **quiero** ver los frames, la telemetría y las detecciones en el
> orden en que se capturaron, **para** que lo que aparece como "último" sea de verdad lo último.

**Cambio propuesto:** en `list_frames`, `list_detections` y `list_telemetry`, ordenar por
`coalesce(CameraFrame.captured_at, CameraFrame.received_at)` (con un join al frame donde haga
falta) en lugar de `received_at` / `detected_at`.

**Criterios de aceptación:** un frame atrasado recibido después de uno actual aparece **detrás**.

### HU-F5 — Documentar el contrato y la dependencia del reloj

- **Swagger y README de `camera_router.py`:** `captured_at` es ISO 8601 en UTC
  (`2026-10-18T14:05:12Z`). Solo lo envían las fotos atrasadas; la foto actual no lo lleva. Hay que
  documentar los dos `422` de HU-F1.
- **Cabecera `Date`:** el firmware pone su reloj en hora con la cabecera `Date` de cada respuesta
  2xx del Fog. Uvicorn la envía por defecto: **no desactivarla** (`--no-date-header`).
- **Reloj de la Pi:** pasa a ser la referencia de hora de las cámaras. La Raspberry Pi no tiene RTC:
  si arranca sin Internet, su hora puede ser incorrecta (fake-hwclock), y las horas de las fotos
  atrasadas lo serán también. Conviene comprobar que NTP está sincronizado antes de dar por buenas
  las horas.
- **De paso**, corregir lo que ya está desfasado en el README y en Swagger sobre la batería: siguen
  diciendo que el firmware no envía `battery_mv` y hablan del ADS1115 (ver discrepancia 3 en el
  `CLAUDE.md` raíz). Es opcional y no bloquea este trabajo.

### 3.6 Cómo usa el firmware la cabecera `Date` (respuesta a dudas del Fog)

Implementado en `esp32cam/src/clock/clock_module.cpp` y `upload_module.cpp`.

**Qué lee.** Solo la cabecera `Date` de la respuesta **2xx** al `POST /api/v1/camera/frames` de
la **foto actual**. Formato IMF-fixdate (`Sun, 18 Oct 2026 14:05:12 GMT`), el que genera uvicorn.
No usa el body, ni otras cabeceras, ni las respuestas a las fotos atrasadas, ni las de error. Sin
`Date`, o con una fecha anterior al 2026-01-01, la ignora (el reloj sigue como estaba).

**Qué hace con ella.**

1. `settimeofday(Date)`. El reloj sigue contando durante el deep sleep (contador del RTC de
   ESP-IDF). La resolución es de 1 s; el error de la latencia de red es despreciable.
2. Mide la deriva: entre la sincronización anterior (S₁) y esta (S₂), el reloj local contó
   `local − S₁` mientras pasaban de verdad `Date − S₁`. Con esa razón corrige las fotos guardadas
   que se capturaron entre S₁ y S₂ (renombra el fichero), siempre que la deriva sea creíble
   (≤ 10 %) y hayan pasado al menos 60 s. Si no, no las toca.
3. Después sube las fotos atrasadas con el `captured_at` ya corregido.

**Qué garantiza el firmware.**

- `captured_at` siempre en UTC con `Z`, sin milisegundos: `2026-10-18T14:05:12Z`.
- **La foto actual nunca lleva `captured_at`**: el Fog usa `received_at`. Así la foto normal no
  depende del reloj de la cámara para nada.
- **Una foto reintentada lleva siempre el mismo `captured_at`.** Solo se corrigen las fotos
  capturadas después de la sincronización anterior, así que una foto ya corregida no vuelve a
  cambiar de hora. La deduplicación de HU-F2 funciona con igualdad exacta.
- Tras una corrección, `captured_at ≤ Date` de la sincronización en la que se corrigió: no genera
  fechas futuras respecto al reloj del Fog.
- Sin sincronización desde el último arranque en frío (encendido, brownout, reset), la cámara **no
  guarda** fotos: no tiene con qué fecharlas.

**Casos que el Fog debe conocer.**

| Caso | Qué pasa | Qué puede hacer el Fog |
|---|---|---|
| La Pi arranca sin NTP y su hora está mal | La cámara copia esa hora y las fotos atrasadas salen con la misma hora errónea que `received_at` | Comprobar `timedatectl show -p NTPSynchronized` antes de servir, o al menos registrarlo en el log |
| La hora de la Pi salta (llega NTP tarde) | La deriva medida sale > 10 % y el firmware no corrige las fotos de ese tramo: quedan con un error del tamaño del salto | Si el error lleva una fecha futura o de más de 24 h, devolver `422` (HU-F1) y el firmware la borra |
| Fotos de antes de un corte de corriente | Conservan la hora con la que se guardaron, sin corregir (puede haber hasta ~5 % de deriva sobre el tiempo transcurrido desde la sincronización anterior) | Nada: la validación de HU-F1 basta |
| El Fog acepta la **foto actual** pero la respuesta se pierde (timeout) | El firmware la guarda y la reenvía en otro ciclo **con** `captured_at`. La original no lo llevaba, así que HU-F2 no la ve como duplicada | Es raro y el estado final es el mismo. Si se quiere evitar: deduplicar también por `sha256` de la imagen |
| Un proxy (nginx…) delante de uvicorn | También envía `Date` con la hora de su máquina | Que sea la misma máquina, o con NTP |

**Lo que no hace falta en el Fog:** ningún endpoint de hora, ningún campo nuevo en la respuesta 202
y ningún cambio en uvicorn, salvo **no** arrancarlo con `--no-date-header`.

---

## 4. `esp32cam` — lo que hará el firmware (para referencia)

No requiere acción de los otros repos. Se implementa cuando §2 esté desplegado.

| Aspecto | Comportamiento |
|---|---|
| Cuándo guarda | Solo si falla la subida de la foto actual (sin WiFi, error de red o 5xx). Se apaga el WiFi antes de escribir. |
| Dónde | LittleFS en la partición `spiffs` (`0x3D0000`, 128 KB). Sin cambio de particiones. Ficheros en `/q1/` (formato versionado). Se escribe con un nombre temporal y se renombra al terminar (operación atómica). |
| Capacidad | 1–3 fotos (5–15 min). Si se llena, se descarta la más antigua. |
| Orden de envío | Primero la foto actual, sin `captured_at`. Si da 2xx, como mucho 3 atrasadas, de la más antigua a la más reciente, con `captured_at`. |
| Reintentos | 2xx o 4xx → borrar. 5xx o error de red → conservar y parar el envío en este ciclo. |
| Hora | El reloj del sistema se fija con la cabecera `Date` de cada 2xx y sigue contando durante el deep sleep (oscilador RC, deriva ≤ ~5 %). Al resincronizar se mide la deriva real y se corrige el `captured_at` de las fotos guardadas antes de enviarlas. Una foto capturada sin hora de referencia (arranque en frío sin Fog) se descarta. |
| Telemetría de una foto atrasada | `fw_version`, `framesize`, `jpeg_quality`, `battery_mv`/`battery_valid` y `reset_reason` **del momento de la captura**. No se envían `rssi`, `wifi_connect_ms`, `awake_ms` ni `free_heap`: describen el ciclo que falló o el de la subida, no el de la captura. |
| Coste en ciclos normales | Ninguno: un contador en RAM RTC evita montar el sistema de archivos si no hay fotos pendientes. |

`captured_at` **ya existe** en el proveedor (`camera_router.py` l. 32), así que no es un campo
nuevo del contrato. Lo que cambia es que el firmware empieza a enviarlo, y habrá que añadirlo a la
tabla del invariante 3 de `esp32cam/CLAUDE.md`.

---

## 5. Decisiones abiertas

| # | Decisión | Propuesta |
|---|---|---|
| D1 | ¿Se suben a R2 las fotos atrasadas? | No (HU-F3, variante recomendada) |
| D2 | ¿Se reconstruye el resumen de zona de los lotes atrasados? | No por ahora (HU-C2 mínima); revisar si el prediction-engine nota los huecos |
| D3 | Antigüedad máxima aceptada en el Fog | 24 h (`BACKLOG_MAX_AGE_HOURS`) |
| D4 | ¿`{"cmd":"erase"}` del aprovisionamiento vacía también la cola de fotos? | Sí: es un reinicio completo de la placa |
