# ESP32-CAM: Fotos a microSD + Streaming web

Sketch para el módulo **AI-Thinker ESP32-CAM** que:

- Muestra el **video en vivo** de la cámara en el navegador (MJPEG).
- Permite **tomar fotos** con un botón y guardarlas en la **tarjeta microSD** (carpeta `/fotos`).
- Tiene un **modo automático** que guarda una foto cada N segundos.

## Requisitos

- Módulo ESP32-CAM (AI-Thinker) con cámara OV2640.
- Tarjeta microSD formateada en **FAT32** (recomendado hasta 32 GB).
- Adaptador USB-serial (FTDI) o placa base ESP32-CAM-MB para programarlo.
- Arduino IDE con el soporte de placas **esp32** instalado (Gestor de placas → buscar "esp32" de Espressif).

## Configuración antes de compilar

En `esp32cam_fotos_stream.ino`, cambiar estas dos líneas con los datos de tu WiFi (debe ser una red de **2.4 GHz**):

```cpp
const char *WIFI_SSID = "TU_RED_WIFI";
const char *WIFI_PASS = "TU_PASSWORD";
```

## Ajustes en Arduino IDE

| Opción | Valor |
|---|---|
| Placa | **AI Thinker ESP32-CAM** |
| Partition Scheme | Huge APP (3MB No OTA) |
| Upload Speed | 115200 (subir a 460800 si funciona estable) |

## Cableado para programar (si usás FTDI en vez de la placa MB)

| FTDI | ESP32-CAM |
|---|---|
| 5V | 5V |
| GND | GND |
| TX | U0R (GPIO 3) |
| RX | U0T (GPIO 1) |

Además, **puentear GPIO 0 a GND** durante la carga. Después de cargar, quitar el puente y presionar RESET.

## Uso

1. Insertar la microSD y alimentar la placa (fuente de 5V con al menos 500 mA; la alimentación débil es la causa #1 de fallas).
2. Abrir el **Monitor Serie a 115200 baudios** y presionar RESET: va a mostrar la IP asignada, por ejemplo:
   ```
   Listo. Abrir en el navegador: http://192.168.1.45
   ```
3. Abrir esa IP en el navegador (desde un dispositivo en la misma red WiFi):
   - Se ve el **video en vivo**.
   - Botón **"Tomar foto"** → guarda `foto_N.jpg` en `/fotos` de la SD.
   - **"Activar auto"** → guarda una foto cada N segundos automáticamente.

El contador de fotos se conserva aunque se reinicie la placa (no se sobreescriben fotos anteriores).

## Endpoints disponibles

| URL | Función |
|---|---|
| `http://IP/` | Página de control con el video embebido |
| `http://IP/foto` | Captura y guarda una foto en la SD |
| `http://IP/auto?seg=10` | Foto automática cada 10 s (`seg=0` la apaga) |
| `http://IP:81/stream` | Stream MJPEG directo (usable en otras apps) |

## Notas

- La SD se monta en **modo 1 bit**: es un poco más lenta pero deja libre el GPIO 4 (LED flash).
- Resolución por defecto: 800x600 (SVGA) si la placa tiene PSRAM. Se puede cambiar en `iniciarCamara()` (`FRAMESIZE_UXGA` = 1600x1200 para fotos más grandes, a costa de menos fluidez en el stream).
- Si aparece "Camara no disponible", revisar que el cable flex de la cámara esté bien asentado en el conector.
