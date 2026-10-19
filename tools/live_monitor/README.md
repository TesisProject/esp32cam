# live_monitor — batería, WiFi y cámara en vivo (herramienta de banco/campo)

Sketch aparte, **no** es el firmware de campo: la ESP32-CAM se queda
despierta (sin deep sleep), no sube nada al Fog y no escribe la NVS. Junta en
una sola página web lo de `battery_monitor` y `wifi_rssi`, y añade la cámara
en tiempo real:

- **Cámara:** vídeo MJPEG en vivo, resolución y calidad cambiables, foto fija
  descargable, fps y KB por frame. Arranca con lo mismo que la foto de campo
  (SVGA, calidad 12), así que el encuadre es el que recibirá el Fog: sirve
  para orientar cada cámara sobre sus plazas.
- **Batería:** tensión, %, lectura cruda de GPIO13 y curva de las últimas
  horas (CSV descargable).
- **WiFi:** RSSI actual, media, mínimo y máximo, canal, BSSID y
  desconexiones.

El sketch principal no lo compila: Arduino solo compila la raíz y `src/`.

## Uso

1. Compila y sube con el **mismo** esquema de particiones que el firmware
   principal (otro esquema podría mover la NVS):

   ```
   cd tools/live_monitor
   arduino-cli compile --fqbn esp32:esp32:esp32cam:PartitionScheme=min_spiffs --build-path build .
   arduino-cli upload  --fqbn esp32:esp32:esp32cam:PartitionScheme=min_spiffs --build-path build -p COM5 .
   arduino-cli monitor -p COM5 -c baudrate=115200,dtr=off,rts=off
   ```

2. **WiFi:** si la placa ya se aprovisionó, usa la red de la NVS (solo la
   lee). Si no, la pide por el monitor serie (SSID, Enter, contraseña,
   Enter) y no la guarda. No hay credenciales en el binario. También lee de
   la NVS la calibración de batería y el id de la cámara (`CAM-00x`, sale en
   el título de la página); la `apiKey` y el `apiToken` no se usan y se
   borran de la RAM al leer el blob.
3. Abre `http://<IP de la placa>/` (sale en el log) desde un móvil o PC en
   la misma red.
4. Para volver al firmware de campo, súbelo de nuevo. La NVS no se toca, así
   que la placa sigue aprovisionada.

| Ruta | Contenido |
|---|---|
| `:80/` | Página (datos cada 1 s; la gráfica, cada 60 s) |
| `:80/api` | Todo en JSON |
| `:80/csv` | Historial de batería (`t_s,adc_mv,bat_mv,pct,valid`) |
| `:80/capture` | Una foto JPEG a la resolución y calidad actuales |
| `:80/control?…` | `framesize=VGA`, `quality=15`, `bat_interval=60` (0 = nunca), `bat_now=1`, `rssi_reset=1` |
| `:81/stream` | Vídeo MJPEG (un cliente a la vez) |

El vídeo va en el puerto 81 porque cada servidor HTTP atiende una petición a
la vez: con el stream en el 80, `/api` dejaría de responder mientras alguien
mira el vídeo.

Por el serie: una línea `MON:` cada 5 s con todo, `r` reinicia la
estadística de RSSI y `b` mide la batería ahora.

## Por qué el vídeo se corta cada minuto

GPIO13 es ADC2, y el ADC2 no se puede leer con el WiFi encendido. En cada
medida (60 s por defecto) la placa corta el vídeo, para los servidores,
apaga la cámara y el WiFi, espera 500 ms, toma 32 muestras (~70 ms) y lo
vuelve a levantar todo. El hueco es de **~3–5 s**, casi todo reconexión WiFi
y DHCP; la página reabre el vídeo sola. Para mirar el vídeo sin cortes (al
orientar la cámara), elige *Medir cada: nunca*; para vigilar una descarga,
5 min.

Se apaga también la cámara para que la medida se parezca a la de campo,
donde la batería se lee al despertar con el OV2640 en PWDN y la radio
apagada. Con la cámara encendida la lectura saldría más baja por la caída en
la resistencia interna de la celda.

## Consumo y brownout

Con vídeo, la radio transmite sin parar y la cámara captura a 20 MHz de
XCLK: es el peor caso de consumo de la placa, muy por encima de un ciclo de
campo (unos segundos despierta cada 5 min). Dos consecuencias:

- **La descarga es mucho más rápida que en campo.** La curva sirve para ver
  la forma, no la autonomía.
- **Riesgo de brownout con la celda + MT3608.** La potencia de TX es la de
  campo (17 dBm, `MON_TX_POWER_DBM`). Si la placa se reinicia en bucle
  (3 destellos largos del LED al arrancar, `Reset reason: 9` en el serie),
  bájala a 11 u 8 dBm: el RSSI medido no cambia (es lo que la placa
  recibe), pero baja el caudal del vídeo. Bajar la resolución a VGA o QVGA
  también reduce el tiempo de radio por frame.

El flash blanco (GPIO4) no se usa: ~150 mA falsearían la medida.

**LED rojo (GPIO33):**

| LED | Significa |
|---|---|
| Fijo encendido | Midiendo la batería (cámara y WiFi apagados) |
| Parpadeo rápido | Conectando o sin WiFi |
| N destellos cada 2 s | N barras de RSSI (0–4, mismos umbrales que `wifi_rssi`) |
| 3 destellos largos al arrancar | El arranque anterior acabó en *brownout* |

## Resoluciones

Con PSRAM, el driver reserva los buffers para UXGA al iniciar, así que se
puede subir hasta 1600×1200 en vivo (a ~2–4 fps). Sin PSRAM solo QVGA, como
en el firmware de campo. La resolución y la calidad elegidas se conservan
entre medidas, pero no se guardan: al reiniciar vuelve a SVGA/12. Para
cambiar lo que sube cada cámara en campo, eso es otro tema (E8,
configuración de captura desde la nube).

Valores orientativos con buena señal (dependen mucho de la escena y del
RSSI): QVGA 20–25 fps, VGA 12–18, SVGA 8–12, UXGA 2–4.

## Copias a mano

Los pines y ajustes de la cámara (`config.h` §5), la recta y la curva de
porcentaje de la batería (`src/battery/battery_module.cpp`) y la forma de
los blobs `wifi`, `bat_cal` y `cred` de la NVS
(`src/settings/settings_module.*`) están copiados en el `.ino`, porque el
IDE no puede incluir ficheros de fuera de la carpeta del sketch. Los
`static_assert` de los tamaños avisan si un blob cambiara de forma; los
demás valores hay que mantenerlos a mano. Si sube `SETTINGS_SCHEMA_VERSION`,
esta herramienta ignora la NVS, usa los defectos y lo avisa por el serie.
