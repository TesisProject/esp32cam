# wifi_rssi — nivel de señal WiFi (herramienta de medida)

Sketch aparte, **no** es el firmware de campo: la ESP32-CAM se queda
despierta (sin deep sleep ni cámara), se conecta a la red como estación y
mide el RSSI del AP 5 veces por segundo. Sirve para comparar antena PCB y
antena externa, o para decidir dónde se monta cada cámara.

El sketch principal no lo compila: Arduino solo compila la raíz y `src/`.

## Uso

1. Compila y sube con el **mismo** esquema de particiones que el firmware
   principal (otro esquema podría mover la NVS):

   ```
   cd tools/wifi_rssi
   arduino-cli compile --fqbn esp32:esp32:esp32cam:PartitionScheme=min_spiffs --build-path build .
   arduino-cli upload  --fqbn esp32:esp32:esp32cam:PartitionScheme=min_spiffs --build-path build -p COM5 .
   arduino-cli monitor -p COM5 -c baudrate=115200,dtr=off,rts=off
   ```

2. **WiFi:** si la placa ya se aprovisionó, usa la red de la NVS (solo la
   lee). Si no, la pide por el monitor serie (SSID, Enter, contraseña,
   Enter) y no la guarda. No hay credenciales en el binario.
3. Al arrancar hace un escaneo y lista cada AP con ese SSID (BSSID, canal y
   RSSI). Después, una línea por segundo:

   ```
   RSSI:  -64 dBm [### ] buena     | media  -64.8  min  -68  max  -62  n=150  desc=0
   ```

4. Desde un móvil en la misma red, `http://<IP de la placa>/` (sale en el
   log) muestra lo mismo y se actualiza cada segundo.
5. Envía `r` por el serie (o pulsa *Reiniciar estadística* en la web) para
   poner a cero mínimo, máximo y media al cambiar de antena o de posición.
6. Para volver al firmware de campo, súbelo de nuevo. La NVS no se toca.

## Cómo leer el valor

| RSSI | Barras / LED | Para subir una foto de 30–60 KB |
|---|---|---|
| ≥ −60 dBm | 4 | Sobrado |
| −60 a −67 | 3 | Bien |
| −67 a −75 | 2 | Funciona, con reintentos de vez en cuando |
| −75 a −82 | 1 | Justo: subidas lentas, más tiempo despierto |
| < −82 | 0 | Fallos frecuentes |

El LED rojo (GPIO33) da tantos destellos cada 2 s como barras; parpadeo
rápido = conectando o sin red. Compara siempre la **media** de al menos
30 s en la misma posición: el valor instantáneo oscila ±3–5 dB.
