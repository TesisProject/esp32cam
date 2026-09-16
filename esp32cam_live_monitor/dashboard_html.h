/* ============================================================================
 *  dashboard_html.h  -  Dashboard web empotrado en el firmware
 *
 *  En la version Arduino IDE el HTML NO viaja en LittleFS: va compilado dentro
 *  del binario como cadena en flash. Asi solo hay UN flasheo y no hace falta
 *  ningun plugin de subida de sistema de ficheros.
 *
 *  En el ESP32 la flash esta mapeada en memoria, asi que esta cadena se puede
 *  leer con un puntero normal (no hacen falta pgm_read_*).
 *
 *  OJO: este fichero NO contiene configuracion. El dashboard pide /config al
 *  arrancar y de ahi saca intervalos, rutas, modo de video y umbrales. Todo
 *  se cambia en config.h.
 *
 *  Tamano aproximado: 13278 bytes de flash.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>

static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>ESP32-CAM Monitor</title>
<!--
  Dashboard servido desde LittleFS por el propio ESP32-CAM.

  IMPORTANTE: este fichero no contiene NINGUN valor de configuracion.
  Al cargar pide /config al firmware y de ahi saca el intervalo de refresco,
  el modo de video, el puerto del stream, los umbrales de bateria y las rutas.
  Para cambiar cualquiera de esas cosas se toca include/config.h, no esto.
-->
<style>
  :root{
    --bg:#0d1117; --panel:#161b22; --panel-2:#1c2330; --line:#293042;
    --txt:#e6edf3; --muted:#8b949e;
    --ok:#3fb950; --warn:#d29922; --crit:#f85149; --accent:#58a6ff;
    --radius:14px;
  }
  *{box-sizing:border-box}
  body{
    margin:0; padding:20px 16px 40px;
    background:radial-gradient(1200px 600px at 50% -10%,#16202e 0%,var(--bg) 60%);
    color:var(--txt);
    font:15px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
    min-height:100vh;
  }
  .wrap{max-width:900px;margin:0 auto}

  header{display:flex;align-items:center;gap:12px;margin-bottom:20px;flex-wrap:wrap}
  header h1{font-size:1.15rem;margin:0;font-weight:600;letter-spacing:.2px}
  .dot{width:9px;height:9px;border-radius:50%;background:var(--muted);flex:none;
       box-shadow:0 0 0 0 rgba(63,185,80,.5)}
  .dot.on{background:var(--ok);animation:pulse 2.4s infinite}
  .dot.off{background:var(--crit)}
  @keyframes pulse{
    0%{box-shadow:0 0 0 0 rgba(63,185,80,.45)}
    70%{box-shadow:0 0 0 7px rgba(63,185,80,0)}
    100%{box-shadow:0 0 0 0 rgba(63,185,80,0)}
  }
  .spacer{flex:1}
  .chip{font-size:.72rem;letter-spacing:.7px;text-transform:uppercase;
        padding:4px 9px;border-radius:999px;border:1px solid var(--line);
        color:var(--muted);background:var(--panel)}
  .chip.live{color:#fff;background:var(--crit);border-color:var(--crit)}

  .grid{display:grid;grid-template-columns:1fr;gap:16px}
  @media(min-width:760px){.grid{grid-template-columns:1.6fr 1fr}}

  .card{background:var(--panel);border:1px solid var(--line);
        border-radius:var(--radius);overflow:hidden}
  .card h2{margin:0;padding:13px 16px;font-size:.78rem;font-weight:600;
           letter-spacing:.9px;text-transform:uppercase;color:var(--muted);
           border-bottom:1px solid var(--line)}
  .card .body{padding:16px}

  /* ---- Video ---- */
  .video{position:relative;background:#000;aspect-ratio:4/3;display:flex;
         align-items:center;justify-content:center}
  .video img{width:100%;height:100%;object-fit:contain;display:block}
  /* Una regla de autor gana siempre a la del navegador: sin esto el atributo
     hidden del <img> no surtiria efecto y se veria el icono de imagen rota. */
  .video img[hidden]{display:none}
  .video .msg{position:absolute;color:var(--muted);font-size:.85rem;
              text-align:center;padding:0 20px}
  .toolbar{display:flex;gap:8px;padding:12px 16px;border-top:1px solid var(--line);
           flex-wrap:wrap;align-items:center}
  button{font:inherit;font-size:.85rem;color:var(--txt);background:var(--panel-2);
         border:1px solid var(--line);border-radius:9px;padding:8px 14px;
         cursor:pointer;transition:.15s background,.15s border-color}
  button:hover:not(:disabled){background:#242c3b;border-color:#3b465c}
  button:disabled{opacity:.45;cursor:not-allowed}
  button.primary{background:var(--accent);border-color:var(--accent);color:#08121f;
                 font-weight:600}
  button.primary:hover:not(:disabled){background:#79b8ff;border-color:#79b8ff}
  button.stop{background:var(--crit);border-color:var(--crit);color:#fff;font-weight:600}
  .toolbar .hint{color:var(--muted);font-size:.78rem;margin-left:auto}

  /* ---- Bateria ---- */
  .batt-top{display:flex;align-items:flex-end;gap:14px;margin-bottom:16px}
  .pct{font-size:2.9rem;font-weight:650;line-height:1;letter-spacing:-1px;
       font-variant-numeric:tabular-nums}
  .volts{color:var(--muted);font-size:.95rem;padding-bottom:5px;
         font-variant-numeric:tabular-nums}

  .gauge{display:flex;align-items:center;gap:7px;margin-bottom:14px}
  .shell{flex:1;height:26px;border:2px solid var(--line);border-radius:7px;
         padding:3px;background:#0b0f16}
  .fill{height:100%;border-radius:3px;width:0%;background:var(--ok);
        transition:width .6s ease,background-color .4s ease}
  .cap{width:4px;height:11px;border-radius:0 2px 2px 0;background:var(--line)}

  .state{display:inline-flex;align-items:center;gap:7px;font-size:.82rem;
         padding:5px 11px;border-radius:999px;border:1px solid var(--line);
         background:var(--panel-2)}
  .state .sq{width:8px;height:8px;border-radius:2px;background:var(--muted)}

  dl{display:grid;grid-template-columns:auto 1fr;gap:7px 14px;margin:18px 0 0;
     font-size:.83rem}
  dt{color:var(--muted)}
  dd{margin:0;text-align:right;font-variant-numeric:tabular-nums}

  footer{margin-top:18px;color:var(--muted);font-size:.76rem;text-align:center}
  .err{color:var(--crit)}
</style>
</head>
<body>
<div class="wrap">

  <header>
    <span id="dot" class="dot"></span>
    <h1 id="title">ESP32-CAM Monitor</h1>
    <span class="spacer"></span>
    <span id="modeChip" class="chip">iniciando</span>
  </header>

  <div class="grid">

    <!-- ================= CAMARA ================= -->
    <section class="card">
      <h2>Camara</h2>
      <div class="video">
        <img id="cam" alt="Imagen de la camara" hidden>
        <div id="camMsg" class="msg">Conectando con la camara...</div>
      </div>
      <div class="toolbar">
        <button id="liveBtn" class="primary" hidden>Ver en vivo</button>
        <button id="shotBtn">Actualizar foto</button>
        <span id="camHint" class="hint"></span>
      </div>
    </section>

    <!-- ================= BATERIA ================= -->
    <section class="card">
      <h2>Bateria</h2>
      <div class="body">
        <div class="batt-top">
          <span id="pct" class="pct">--%</span>
          <span id="volts" class="volts">-- V</span>
        </div>

        <div class="gauge">
          <div class="shell"><div id="fill" class="fill"></div></div>
          <div class="cap"></div>
        </div>

        <span id="state" class="state"><span class="sq"></span>sin datos</span>

        <dl>
          <dt>Capacidad</dt>      <dd id="cap">--</dd>
          <dt>Ultima lectura</dt> <dd id="age">--</dd>
          <dt>Uptime</dt>         <dd id="uptime">--</dd>
          <dt>WiFi</dt>           <dd id="rssi">--</dd>
        </dl>
      </div>
    </section>

  </div>

  <footer id="foot">Servido por el propio ESP32-CAM &middot; sin servidor externo</footer>
</div>

<script>
"use strict";

/* Estado de la pagina. Todo lo relevante llega desde /config. */
var CFG = null;
var live = false;            // true mientras el MJPEG esta activo
var snapTimer = null;
var battTimer = null;
var statusTimer = null;
var failures = 0;

var $ = function (id) { return document.getElementById(id); };

/* ---------------------------------------------------------------- arranque */
function boot() {
  fetch("/config", { cache: "no-store" })
    .then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    })
    .then(function (cfg) {
      CFG = cfg;
      applyConfig();
      startLoops();
    })
    .catch(function (e) {
      $("camMsg").textContent = "No se pudo leer /config: " + e.message;
      $("camMsg").className = "msg err";
      setOnline(false);
      setTimeout(boot, 5000);
    });
}

function applyConfig() {
  document.title = CFG.device;
  $("title").textContent = CFG.device;

  var b = CFG.battery;
  $("cap").textContent = b.capacityMah + " mAh / " +
                         b.cells + "S / " + b.fullVolts.toFixed(2) + " V";

  /* El boton LIVE solo tiene sentido en modo hibrido.
     En modo stream el video arranca solo; en snapshot no existe. */
  if (CFG.mode === "hybrid") {
    $("liveBtn").hidden = false;
    $("modeChip").textContent = "hibrido";
    $("camHint").textContent = "Foto cada " + (CFG.snapshotRefreshMs / 1000) +
                               " s. El directo consume mas bateria.";
  } else if (CFG.mode === "stream") {
    $("modeChip").textContent = "directo";
    $("camHint").textContent = "MJPEG continuo";
  } else {
    $("modeChip").textContent = "snapshot";
    $("camHint").textContent = "Foto cada " + (CFG.snapshotRefreshMs / 1000) + " s";
  }
}

function startLoops() {
  if (CFG.mode === "stream") {
    startLive();
  } else {
    refreshSnapshot();
    snapTimer = setInterval(function () {
      if (!live) refreshSnapshot();
    }, CFG.snapshotRefreshMs);
  }

  refreshBattery();
  battTimer = setInterval(refreshBattery, CFG.batteryRefreshMs);

  refreshStatus();
  statusTimer = setInterval(refreshStatus, CFG.batteryRefreshMs);
}

/* ------------------------------------------------------------------ camara */
function snapshotURL() {
  return CFG.routes.snapshot + "?t=" + Date.now();
}

function streamURL() {
  /* El stream vive en otro puerto (ver config.h): hay que construir la URL
     a mano a partir del host actual. */
  return "http://" + location.hostname + ":" + CFG.streamPort + CFG.routes.stream;
}

function refreshSnapshot() {
  if (live) return;
  /* Precarga en memoria y luego intercambia: evita el parpadeo a negro. */
  var pre = new Image();
  pre.onload = function () {
    var img = $("cam");
    img.src = pre.src;
    img.hidden = false;
    $("camMsg").hidden = true;
    setOnline(true);
  };
  pre.onerror = function () {
    showCamError("El ESP32 no devuelve imagen en " + CFG.routes.snapshot +
                 ". Abre /status y comprueba cameraReady.");
  };
  pre.src = snapshotURL();
}

function showCamError(text) {
  var img = $("cam");
  img.hidden = true;
  var m = $("camMsg");
  m.hidden = false;
  m.className = "msg err";
  m.textContent = text;
  setOnline(false);
}

function startLive() {
  live = true;
  var img = $("cam");
  img.hidden = false;
  $("camMsg").hidden = true;

  /* Si el stream no arranca (camara caida, puerto 81 bloqueado, o ya hay
     otro cliente viendo), el navegador dispara error sobre el <img>. */
  img.onerror = function () {
    if (!live) return;
    showCamError("No se pudo abrir el directo en " + streamURL() +
                 ". Puede que ya haya otro cliente viendolo.");
  };
  img.src = streamURL();

  $("modeChip").textContent = "en directo";
  $("modeChip").classList.add("live");

  var btn = $("liveBtn");
  btn.textContent = "Detener directo";
  btn.className = "stop";
  $("shotBtn").disabled = true;
}

function stopLive() {
  live = false;
  /* Vaciar el src corta la conexion MJPEG: el handler del ESP32 detecta el
     socket cerrado, sale del bucle y vuelve a su perfil de bajo consumo. */
  $("cam").src = "";
  $("modeChip").textContent = "hibrido";
  $("modeChip").classList.remove("live");

  var btn = $("liveBtn");
  btn.textContent = "Ver en vivo";
  btn.className = "primary";
  $("shotBtn").disabled = false;

  refreshSnapshot();
}

$("liveBtn").addEventListener("click", function () {
  if (live) { stopLive(); } else { startLive(); }
});

$("shotBtn").addEventListener("click", function () {
  if (!live) refreshSnapshot();
});

/* ----------------------------------------------------------------- bateria */
function refreshBattery() {
  fetch(CFG.routes.battery, { cache: "no-store" })
    .then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    })
    .then(function (b) {
      setOnline(true);
      paintBattery(b);
    })
    .catch(function () { setOnline(false); });
}

function paintBattery(b) {
  if (!b.valid) {
    $("pct").textContent = "--%";
    $("volts").textContent = "sin ADC";
    setState("unknown", "sensor no disponible");
    $("fill").style.width = "0%";
    return;
  }

  $("pct").textContent = b.percent + "%";
  $("volts").textContent = b.voltage.toFixed(2) + " V";
  $("age").textContent = Math.round(b.ageMs / 1000) + " s";

  var fill = $("fill");
  fill.style.width = Math.max(2, Math.min(100, b.percent)) + "%";

  var color = "var(--ok)", label = "nivel correcto";
  if (b.state === "critical") { color = "var(--crit)"; label = "bateria critica"; }
  else if (b.state === "low") { color = "var(--warn)"; label = "bateria baja"; }
  fill.style.background = color;
  setState(b.state, label);
}

function setState(state, label) {
  var el = $("state");
  var color = state === "critical" ? "var(--crit)"
            : state === "low"      ? "var(--warn)"
            : state === "ok"       ? "var(--ok)"
            : "var(--muted)";
  el.innerHTML = '<span class="sq"></span>' + label;
  el.querySelector(".sq").style.background = color;
}

/* ------------------------------------------------------------------ estado */
function refreshStatus() {
  fetch(CFG.routes.status, { cache: "no-store" })
    .then(function (r) { return r.json(); })
    .then(function (s) {
      if (!s.cameraReady) {
        showCamError("La camara no arranco (cameraReady=false). Revisa el " +
                     "cable plano, habilita PSRAM en Herramientas y comprueba " +
                     "la alimentacion: el sensor pide ~300 mA al inicializar.");
      }
      $("uptime").textContent = fmtUptime(s.uptimeS);
      $("rssi").textContent = s.rssi + " dBm";
      $("foot").textContent = s.ip + " | " + s.resolution + " | " +
                              Math.round(s.heapFree / 1024) + " kB RAM libre | " +
                              s.cpuMhz + " MHz";
    })
    .catch(function () { /* el estado es opcional, no rompe el dashboard */ });
}

function fmtUptime(s) {
  var d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600),
      m = Math.floor((s % 3600) / 60);
  if (d > 0) return d + "d " + h + "h";
  if (h > 0) return h + "h " + m + "m";
  return m + "m " + (s % 60) + "s";
}

function setOnline(ok) {
  if (ok) {
    failures = 0;
    $("dot").className = "dot on";
  } else {
    failures++;
    if (failures >= 2) $("dot").className = "dot off";
  }
}

/* Al volver de segundo plano, refresca ya en vez de esperar al intervalo. */
document.addEventListener("visibilitychange", function () {
  if (!document.hidden && CFG) {
    refreshBattery();
    if (!live && CFG.mode !== "stream") refreshSnapshot();
  }
});

boot();
</script>
</body>
</html>
)rawliteral";
