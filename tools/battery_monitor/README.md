# battery_monitor — página web con la batería (herramienta de banco)

Sketch aparte, **no** es el firmware de campo: la ESP32-CAM se queda despierta
(sin deep sleep ni cámara), **crea su propia red WiFi** y sirve una página
con la tensión de la celda, el porcentaje, la lectura cruda de GPIO13 y la
curva de descarga de las últimas 6 h. Sirve para vigilar una descarga o para
calibrar con el multímetro mirando el móvil. No necesita router.

El sketch principal no lo compila: Arduino solo compila la raíz y `src/`.

## Uso

1. Compila y sube con el **mismo** esquema de particiones que el firmware
   principal (otro esquema podría mover la NVS):

   ```
   cd tools/battery_monitor
   arduino-cli compile --fqbn esp32:esp32:esp32cam:PartitionScheme=min_spiffs --build-path build .
   arduino-cli upload  --fqbn esp32:esp32:esp32cam:PartitionScheme=min_spiffs --build-path build -p COM5 .
   ```

   Con la base ESP32-CAM-MB, la subida entra sola en modo descarga. Sin ella,
   GPIO0 a GND y RST antes de subir, y quitar el puente después.
2. Conecta el móvil o el portátil a la red **`ParkVision-BAT`** (clave
   `parkvision`). La página se abre sola (portal cautivo); si no, ve a
   `http://192.168.4.1/`. Si el móvil avisa de que la red "no tiene
   Internet", elige mantener la conexión.
3. Para volver al firmware de campo, súbelo de nuevo. La NVS no se toca, así
   que la placa sigue aprovisionada.

El SSID, la clave y el canal están al principio del `.ino`
(`MON_AP_*`). La clave no es un secreto del proyecto: solo evita que
cualquiera se conecte a la red de banco.

| Ruta | Contenido |
|---|---|
| `/` | Página (se actualiza cada 5 s; la gráfica, cada 30 s) |
| `/api` | Última medida en JSON |
| `/csv` | Historial completo (`t_s,adc_mv,bat_mv,pct,valid`) |

Calibración: si la placa se aprovisionó con `batteryCal`, la lee de la NVS
(solo lectura). Si no, usa los defectos de `config.h`. La página indica cuál
está usando.

## Por qué la red desaparece cada 30 s

GPIO13 es ADC2, y el ADC2 no se puede leer con el WiFi encendido. En cada
medida la placa apaga el access point, espera 500 ms, toma 32 muestras
(~70 ms) y lo vuelve a levantar. El móvil pierde la red durante uno o pocos
segundos y vuelve a conectarse solo; la página sigue preguntando y se salta
ese hueco. Si la placa no apagara el WiFi, todas las lecturas fallarían
(`fails` = 32 en `/api`).

## Diferencias con el firmware de campo

- **La lectura sale algo más baja.** Aquí la celda alimenta la placa con el
  WiFi encendido casi todo el tiempo (~100–150 mA desde 5 V, ~200–250 mA
  desde la celda). En 500 ms la celda no recupera del todo la caída por su
  resistencia interna. En campo se mide al despertar, tras minutos de deep
  sleep. Cuenta con unas decenas de mV de diferencia. Para calibrar, mide con
  el multímetro **en el mismo instante** en que aparece la línea `BAT:` en el
  serie: así el error de carga entra en los dos lados por igual.
- **La descarga es mucho más rápida que en campo.** Sirve para ver la forma
  de la curva, no la autonomía real.
- **Copias a mano.** La recta, la curva de porcentaje, los umbrales y la forma
  del blob de calibración están copiados de `config.h` y
  `src/settings/settings_module.*`, porque el IDE no puede incluir ficheros de
  fuera de la carpeta del sketch. Si cambian allí, hay que cambiarlos aquí.
  Si sube `SETTINGS_SCHEMA_VERSION`, esta herramienta ignora la NVS, usa los
  defectos y lo avisa por el serie.
