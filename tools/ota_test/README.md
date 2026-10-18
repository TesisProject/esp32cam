# ota_test — banco de pruebas del OTA y del rollback

Sketch aparte, **no** es el firmware de campo ni el cliente OTA (E5, que
está en `src/ota/` y descarga del Fog). Empuja un `.bin` a mano, sin Fog ni
firma, para comprobar en la placa real lo que no depende de la descarga:

- el `.bin` cabe en una ranura de Minimal SPIFFS y arranca desde la otra;
- el firmware solo confirma la imagen tras una subida 2xx al Fog
  (`ota_confirm_if_pending()`), y si no, el bootloader vuelve solo a la
  imagen anterior al despertar;
- la NVS (WiFi, Fog, credencial) sobrevive al cambio de imagen.

La herramienta hace de **versión anterior**: si el firmware nuevo no se
confirma, el bootloader vuelve a ella y lo dice en el serie y en la web.

> **Sin autenticación.** Mientras corre, cualquiera en la LAN puede flashear
> la placa. Solo en el banco.

## Lo que esta prueba no cubre

La descarga desde el Fog, la firma, la comparación de versiones y la clave
`ota_rej`: todo eso es el cliente E5 y se prueba con el Fog (ver la sección
"Cliente OTA (E5)" del `CLAUDE.md`). Aquí el `.bin` se empuja a mano.

## Preparación

1. Placa ya aprovisionada (la herramienta lee la WiFi de la NVS; si no la
   tiene, la pide por el serie y no la guarda).
2. Subir la herramienta por USB con el **mismo** esquema de particiones:

   ```
   cd tools/ota_test
   arduino-cli compile --fqbn esp32:esp32:esp32cam:PartitionScheme=min_spiffs --build-path build .
   arduino-cli upload  --fqbn esp32:esp32:esp32cam:PartitionScheme=min_spiffs --build-path build -p COM5 .
   arduino-cli monitor -p COM5 -c baudrate=115200,dtr=off,rts=off
   ```

   Al arrancar muestra las dos ranuras, por ejemplo:

   ```
   OTA: corriendo en app0
   OTA:   app0: UNDEFINED (flasheo USB) | ota_test compilado Oct 18 2026 ...
   OTA:   app1: sin otadata | sin imagen valida  <- aqui se escribira el .bin
   WIFI: conectado, IP 192.168.1.57
   ```

3. Compilar el firmware principal (desde la raíz del repo). Para no esperar
   5 min en la prueba de rollback, se puede compilar con
   `CAPTURE_INTERVAL_MS 30000UL`, **sin hacer commit de ese cambio**.

## Prueba 1 — OTA correcto y confirmación

Con el Fog en marcha:

```
curl -F "firmware=@build/esp32cam.ino.bin" http://192.168.1.57/update
```

o desde `http://192.168.1.57/` con el formulario. Usa `esp32cam.ino.bin`,
**no** `esp32cam.ino.merged.bin` (lleva bootloader y particiones; Update lo
rechaza por el byte mágico).

En el serie debería salir:

```
OTA: 1232245 bytes escritos y verificados en ... ms
OTA: reiniciando en la imagen nueva (estado NEW -> PENDING_VERIFY)
...
OTA: FW_VERSION=3  particion activa=app1  estado=PENDING_VERIFY
NVS: camera=CAM-001  wifi="..."  fog=...        ← la NVS sobrevivió, no pide aprovisionar
UPLOAD: ok (202)
OTA: imagen de app1 confirmada (ciclo completo con subida ok)
```

En los ciclos siguientes, `estado=VALID`. **Prueba superada.**

## Prueba 2 — rollback

1. Volver a subir la herramienta por USB (el firmware principal no tiene
   cliente OTA para volver a ella).
2. **Parar el Fog** (o desconectar su red) para que la subida falle.
3. Subir el `.bin` como en la prueba 1.
4. El firmware arranca en `PENDING_VERIFY`, la subida falla y se duerme
   **sin confirmar**.
5. Al despertar (5 min, o 30 s con el intervalo de prueba), el bootloader
   ve la imagen sin confirmar, la marca `ABORTED` y arranca la herramienta:

   ```
   OTA: corriendo en app0
   OTA: ROLLBACK: la imagen de app1 se descarto (no se confirmo) y se volvio a esta
   ```

**Prueba superada** si vuelve sola a `ota_test` sin tocar la placa.

> El rollback lo decide el bootloader en el siguiente arranque, sea por
> deep sleep o por reset. Pulsar RST antes de que el firmware confirme
> produce el mismo resultado.

## Volver al firmware de campo

Subir el firmware principal por USB. La NVS no se toca en ningún paso.
