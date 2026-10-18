# Handoff — Batería de las cámaras: porcentaje, estado y última lectura

**Fecha:** 2026-10-18 · **Origen:** `esp32cam` · **Destinatario:** `cloud-service` (occupancy)

**Orden:** solo hay trabajo en la nube. El firmware y el Fog no cambian: ya envían y reenvían
la tensión. El frontend consume lo que salga de aquí (§6).

---

## 1. Contexto

Cada ESP32-CAM va alimentada por una celda Li-ion 18650 (1S, 3,0–4,2 V): TP4056 con protección,
MT3608 a 5 V. Mide la tensión de la celda en reposo, con un divisor en GPIO13, al despertar y antes
de encender el WiFi.

**Qué llega ya a la nube.** El Fog solo reenvía: no mide ni deriva nada. Pasa dentro del `metadata`
de cada foto (`fog-application/src/vision/cloud_client.py`):

| Campo | Qué es |
|---|---|
| `batteryMv` | Tensión de la celda en reposo, en mV. Ya calibrada por placa (dos puntos en la NVS). |
| `batteryValid` | `true` si la lectura es la de una celda conectada (2500–4500 mV). Con `false`, `batteryMv` llega `null`: placa por USB sin celda, o sin divisor. |
| `batteryLoadMv` | **Siempre `null`.** GPIO13 es ADC2 y no se puede leer con el WiFi transmitiendo. No usarlo. |

La nube ya los guarda por foto: `camera_frame.battery_mv` y `battery_valid` (V4). También los
devuelve en `CameraFrameResource.Telemetry`.

**Qué falta.** Todo lo derivado: el porcentaje, el estado de la batería y la última lectura de cada
cámara sin tener que recorrer sus fotos.

**Frecuencia.** La batería solo llega con las fotos que el Fog sube a la nube, como mucho una cada
`SCREENSHOT_INTERVAL_MINUTES` (5 min en `fog-application/.env.example`). Esa es la resolución
temporal de la batería en la nube, y para la batería basta.

---

## 2. HU-B1 — Curva mV → % (se deriva al leer)

**Como** operador, **quiero** ver el porcentaje de batería de cada cámara, **para** saber cuándo hay
que cargarla o cambiarla.

**Tabla** (tensión en reposo → %). Es **la misma** que usa el firmware para su log serie
(`esp32cam/config.h`, `BATTERY_CURVE_MV/PCT`) y la herramienta de banco `tools/battery_monitor`:

| mV | 3300 | 3500 | 3650 | 3750 | 3850 | 4000 | 4200 |
|---|---|---|---|---|---|---|---|
| **%** | 0 | 9 | 28 | 43 | 59 | 79 | 100 |

**Reglas:**

1. Interpolación lineal entre los dos puntos vecinos, con **división entera truncada**, igual que el
   firmware:
   `pct = P[i-1] + (mv - M[i-1]) * (P[i] - P[i-1]) / (M[i] - M[i-1])`.
2. Saturada: `mv ≤ 3300` → 0; `mv ≥ 4200` → 100.
3. Si `batteryValid` no es `true` o `batteryMv` es `null` → **`null`**, nunca 0. Un 0 % diría
   "batería vacía" cuando lo que pasa es que no hay medida.
4. **No se persiste.** Se calcula al leer. Si cambia la curva, el histórico no se invalida, porque la
   medida es la tensión y no el %. Por eso no hace falta migración.
5. Va en el dominio, por ejemplo un value object `BatteryLevel` en `occupancy`, para que el
   recurso de foto y el de cámara usen el mismo cálculo.

**Por qué 0 % = 3300 mV.** Por debajo de 3300 mV (`BATTERY_CUTOFF_MV`) el firmware ya no enciende
cámara ni WiFi, y no vuelve a transmitir hasta superar 3400 mV (`BATTERY_RESUME_MV`). Para el
sistema, 3,30 V es "vacía". La celda aún tiene algo de carga, pero no conviene bajar de ahí. El
DW01 del TP4056 la desconecta hacia 2,5 V.

**Precisión.** Con la calibración de dos puntos, el error es de **±20–30 mV**. En la zona plana
(3650–3850 mV, unos 15–16 % por cada 100 mV) eso son **±3–5 puntos**. Sin calibrar, el error del ADC
puede llegar a varias decenas de mV. El % es orientativo: **mostrar siempre los mV al lado**.

**Criterios de aceptación** (tests unitarios del value object):

| `batteryMv` | `batteryValid` | % esperado |
|---|---|---|
| 3000 | true | 0 |
| 3300 | true | 0 |
| 3400 | true | 4 |
| 3600 | true | 21 |
| 3710 | true | 37 |
| 3834 | true | 56 |
| 3900 | true | 65 |
| 4100 | true | 89 |
| 4199 | true | 99 |
| 4200 | true | 100 |
| 4300 | true | 100 |
| `null` | false | `null` |
| 3834 | false | `null` |
| `null` | `null` | `null` (fotos sin telemetría) |

Los valores salen de la misma interpolación que el firmware; están verificados.

---

## 3. HU-B2 — Estado de la batería

**Como** operador, **quiero** un estado claro (bien / baja / crítica), **para** no tener que
interpretar tensiones.

Los umbrales salen del firmware, así que significan algo concreto en la cámara:

| `state` | Condición | Qué pasa en la cámara |
|---|---|---|
| `OK` | ≥ 3600 mV | Funciona y acepta actualizaciones OTA (`OTA_MIN_BATTERY_MV`). |
| `LOW` | 3400–3599 mV | Sigue capturando, pero **ya no instala OTA**. |
| `CRITICAL` | < 3400 mV | Está a punto del corte: a 3300 mV deja de transmitir. |
| `UNKNOWN` | % `null` (lectura no válida o sin telemetría) | |

Los umbrales en mV, no en %: así no dependen de la curva.

**Aceptación:** 3600 → `OK`; 3599 → `LOW`; 3400 → `LOW`; 3399 → `CRITICAL`;
`batteryValid=false` → `UNKNOWN`.

---

## 4. HU-B3 — Última lectura por cámara y antigüedad

**Como** operador, **quiero** ver en la lista de cámaras la batería de cada una y si el dato es
reciente, **para** distinguir una cámara sin batería de una que funciona.

**El problema que resuelve.** Cuando la celda llega al corte, **la cámara deja de enviar**. Su
última lectura se queda congelada en ~3,3–3,4 V. Sin una marca de antigüedad, la web la mostraría
como si fuera actual. Una lectura `CRITICAL` seguida de silencio es casi siempre "se quedó sin
batería", no "se desconectó".

**Qué añadir a `CameraResource`** (`GET /cameras` y `GET /cameras/{id}`):

```json
"battery": {
  "mv": 3834,
  "percent": 56,
  "state": "OK",
  "capturedAt": "2026-10-18T14:05:12Z",
  "stale": false
}
```

**Reglas:**

1. Sale de la foto **más reciente por `capturedAt`** (no por `receivedAt`) que tenga
   `batteryValid = true`. Con las fotos atrasadas del firmware
   (`docs/handoff-buffer-fotos-offline.md`), una foto vieja puede llegar después de una nueva. Si se
   ordena por llegada, la batería "retrocede".
2. `stale = true` si `capturedAt` es más antiguo que un umbral configurable, por ejemplo
   `parkvision.battery.stale-after` = `PT20M`. Eso son 3–4 intervalos del Fog: una foto perdida no
   la marca como antigua. La nube no conoce el intervalo del Fog; por eso va como propiedad.
3. Si la cámara no tiene ninguna lectura válida: `"battery": null`.
4. Una sola consulta para toda la lista: la última lectura por cámara con
   `DISTINCT ON (camera_id) … ORDER BY camera_id, captured_at DESC`, o equivalente. No hacer una
   consulta por cámara. Si hace falta, añadir un índice `(camera_id, captured_at DESC)` en
   `camera_frame`, filtrado por `battery_valid`.

**Aceptación:**

- La cámara con una lectura de 3834 mV de hace 2 min da `percent 56`, `state OK` y `stale false`.
- La misma lectura de hace 1 h da `stale true`.
- Una foto con `capturedAt` anterior que llega después no cambia el bloque.
- Una cámara sin fotos con batería da `battery: null`.

---

## 5. HU-B4 — % y estado en cada foto

Añadir a `CameraFrameResource.Telemetry` dos campos calculados con HU-B1 y HU-B2:

- `batteryPercent` (`Integer`, puede ser `null`)
- `batteryState` (`String`)

No cambia nada de lo que ya se guarda.

---

## 6. HU-B5 — Documentar para el frontend

En `frontend-app/API-Frontend.md`:

- el bloque `battery` de la cámara y los dos campos nuevos de la telemetría;
- que el % es orientativo y se muestran siempre los mV;
- qué significa `stale`.

## 7. HU-B6 (opcional, para la tesis) — Curva de descarga

`GET /api/v1/occupancy/cameras/{id}/battery/history?from=…&to=…&bucket=PT1H`: la mediana o la media
de `battery_mv` por intervalo, solo con lecturas válidas.

Sirve para medir la autonomía real de cada cámara y contrastarla con la estimada en el README del
firmware (18650 de ~2500 mAh). Hoy se puede sacar de `GET /cameras/{id}/frames`, pero es paginado y
trae las URLs de R2.

---

## 8. Decisiones

| # | Decisión | Motivo |
|---|---|---|
| D1 | El % se deriva en la nube, al leer; no se guarda. | Cambiar la curva no invalida el histórico. El firmware no envía `battery_percent`, y el Fog lo ignoraría. |
| D2 | 0 % = 3300 mV. | Es el corte del firmware: por debajo la cámara no transmite. |
| D3 | Una sola tabla en tres sitios: `config.h`, `tools/battery_monitor` y la nube. | Si no, el log serie y la web muestran porcentajes distintos para la misma lectura. Cualquier cambio de la curva se hace en los tres a la vez. |
| D4 | El estado se decide por mV, no por %. | Los umbrales son los del firmware (OTA, corte). |
| D5 | La capacidad de la celda (mAh) no interviene. | El % sale de la tensión. La capacidad solo serviría para estimar "horas restantes", que no se pide aquí. |

## 9. Fuera de alcance

- **Alertas** (correo o notificación al pasar a `CRITICAL`): se puede hacer después sobre HU-B2.
- **`batteryLoadMv` y la caída de tensión bajo carga:** el hardware no permite medirla con el WiFi
  encendido.
- **Curva medida con la celda real:** la tabla es la típica de una 18650 en reposo. Si se mide una
  descarga completa (HU-B6 da los datos), se puede ajustar en los tres sitios sin tocar el
  histórico.
