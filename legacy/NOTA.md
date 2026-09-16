# Carpeta legacy

Aqui esta archivado el proyecto anterior, que sigue siendo valido pero ya no es
el que se desarrolla:

- `esp32cam_fotos_stream.ino` - sketch de fotos a microSD + streaming MJPEG.
- `README.md` - su documentacion original.

El proyecto activo es `../esp32cam_live_monitor/`.

Diferencia principal: el nuevo NO usa microSD. Sus pines (GPIO14/15) pasan a ser
el bus I2C del ADS1115 que mide la bateria. Si algun dia necesitas volver a
guardar fotos en la tarjeta, este sketch es el punto de partida.
