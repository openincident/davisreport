// ===========================================================================
// davis_web.cpp  —  The board's own little weather website
// ---------------------------------------------------------------------------
// HOW IT WORKS, in plain English:
//   - We keep a "ring buffer" of recent readings: a fixed-size list that, once
//     full, overwrites its oldest entry. That gives us a rolling window of
//     history (24 hours by default) without ever using more memory.
//   - A small web server answers two kinds of requests:
//       "/"           -> the web PAGE (HTML + charts), sent once.
//       "/data.json"  -> the DATA (current conditions + the history list).
//     The page asks for the data every 30 seconds and redraws the charts.
//   - "davis.local" is published so you can use a friendly name instead of the
//     board's IP address.
//
// We keep every request handler fast and small so serving a page never stalls
// the radio's frequency-hopping for long. The history rows are streamed out in
// little chunks rather than built into one giant chunk of memory.
// ===========================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "davis_web.h"
#include "config.h"

// Sensible fallbacks if an older config.h doesn't define these.
#ifndef WEB_ENABLE
#define WEB_ENABLE 0
#endif
#ifndef WEB_HOSTNAME
#define WEB_HOSTNAME "davis"
#endif
#ifndef WEB_SAMPLE_SECONDS
#define WEB_SAMPLE_SECONDS 120
#endif
#ifndef WEB_HISTORY_POINTS
#define WEB_HISTORY_POINTS 720
#endif

// ---------------------------------------------------------------------------
// One stored history point. We pack the values into small types so the whole
// history stays tiny in memory (~14 bytes each).
// ---------------------------------------------------------------------------
struct Sample {
  uint32_t t;        // board uptime in seconds when this point was recorded
  int16_t  temp10;   // temperature in °F x 10  (e.g. 825 = 82.5 °F)
  int16_t  dew10;    // dew point in °F x 10
  uint8_t  hum;      // humidity %
  uint8_t  wind;     // wind speed, mph
  uint8_t  gust;     // wind gust, mph
  uint16_t rain100;  // rain since uptime, inches x 100
  int8_t   rssi;     // signal strength, dBm
};

static Sample   ring[WEB_HISTORY_POINTS];   // the rolling history
static uint16_t ringCount = 0;              // how many points we have so far
static uint16_t ringHead  = 0;              // where the next point will be written
static uint32_t lastSampleMs = 0;           // when we last recorded a history point

// The most recent values (kept fresh every call, for the page header).
static Sample   cur;
static uint16_t curDir = 0;
static bool     curLocked = false;
static bool     curAlert = false;
static char     curReason[20] = "none";

static WebServer server(80);
static bool serverStarted = false;

// ---------------------------------------------------------------------------
// Unit helpers — we store everything in °F / mph / inches and convert here so
// the page shows whatever you picked with USE_IMPERIAL_UNITS.
// ---------------------------------------------------------------------------
static float outTemp(float f)  { return USE_IMPERIAL_UNITS ? f : (f - 32.0f) * 5.0f / 9.0f; }
static float outWind(float m)  { return USE_IMPERIAL_UNITS ? m : m * 1.60934f; }
static float outRain(float in) { return USE_IMPERIAL_UNITS ? in : in * 25.4f; }

// ===========================================================================
// THE WEB PAGE (HTML + CSS + JavaScript).
// ---------------------------------------------------------------------------
// This whole page is stored in flash (PROGMEM) and sent as-is. It contains no
// live data — instead its JavaScript fetches "/data.json" and fills everything
// in, then refreshes every 30 seconds. Chart.js (loaded from the internet) does
// the graph drawing.
// ===========================================================================
static const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Davis Weather</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
<style>
  :root { --bg:#0e1116; --card:#1b2230; --ink:#e6edf3; --muted:#8b98a9; --accent:#4cc2ff; --warn:#ff5252; }
  * { box-sizing:border-box; }
  body { margin:0; background:var(--bg); color:var(--ink); font-family:system-ui,Segoe UI,Roboto,sans-serif; }
  header { padding:16px 20px; display:flex; flex-wrap:wrap; align-items:baseline; gap:8px 16px; border-bottom:1px solid #232c3b; }
  header h1 { font-size:20px; margin:0; }
  header .sub { color:var(--muted); font-size:13px; }
  #alert { display:none; background:var(--warn); color:#fff; padding:10px 20px; font-weight:700; letter-spacing:.5px; }
  .wrap { padding:16px 20px; max-width:1100px; margin:0 auto; }
  .cards { display:grid; grid-template-columns:repeat(auto-fit,minmax(140px,1fr)); gap:12px; margin-bottom:18px; }
  .card { background:var(--card); border-radius:14px; padding:14px 16px; }
  .card .label { color:var(--muted); font-size:12px; text-transform:uppercase; letter-spacing:.6px; }
  .card .val { font-size:30px; font-weight:700; margin-top:4px; }
  .card .val small { font-size:15px; color:var(--muted); font-weight:500; }
  .arrow { display:inline-block; transition:transform .4s; }
  .chartcard { background:var(--card); border-radius:14px; padding:12px 14px; margin-bottom:16px; }
  .chartcard h3 { margin:2px 0 8px; font-size:14px; color:var(--muted); font-weight:600; }
  canvas { width:100% !important; }
  footer { color:var(--muted); font-size:12px; text-align:center; padding:18px; }
</style>
</head>
<body>
<div id="alert"></div>
<header>
  <h1>🌤️ Davis Weather</h1>
  <span class="sub" id="status">connecting…</span>
</header>
<div class="wrap">
  <div class="cards" id="cards"></div>
  <div class="chartcard"><h3>Temperature &amp; Dew Point</h3><canvas id="cTemp" height="120"></canvas></div>
  <div class="chartcard"><h3>Humidity</h3><canvas id="cHum" height="100"></canvas></div>
  <div class="chartcard"><h3>Wind &amp; Gust</h3><canvas id="cWind" height="100"></canvas></div>
  <div class="chartcard"><h3>Rain (since startup)</h3><canvas id="cRain" height="100"></canvas></div>
  <footer>Served by the LilyGO board · history resets when the board reboots</footer>
</div>
<script>
const charts = {};
function mkChart(id, series, opts={}) {
  const ctx = document.getElementById(id);
  return new Chart(ctx, {
    type:'line',
    data:{ labels:[], datasets: series.map(s => ({
      label:s.label, data:[], borderColor:s.color, backgroundColor:s.fill||s.color,
      borderWidth:2, pointRadius:0, tension:0.3, fill:s.area||false })) },
    options:{ animation:false, responsive:true, interaction:{intersect:false,mode:'index'},
      scales:{ x:{ ticks:{ color:'#8b98a9', maxTicksLimit:8 }, grid:{ color:'#232c3b' } },
               y:{ ticks:{ color:'#8b98a9' }, grid:{ color:'#232c3b' }, ...(opts.y||{}) } },
      plugins:{ legend:{ labels:{ color:'#e6edf3' } } } }
  });
}
function init() {
  charts.temp = mkChart('cTemp', [
    {label:'Temp', color:'#ff8a5b'}, {label:'Dew Point', color:'#4cc2ff'} ]);
  charts.hum  = mkChart('cHum',  [{label:'Humidity %', color:'#7ee787', area:true, fill:'rgba(126,231,135,.12)'}], {y:{min:0,max:100}});
  charts.wind = mkChart('cWind', [{label:'Wind', color:'#a5a5ff'}, {label:'Gust', color:'#ff5252'}], {y:{min:0}});
  charts.rain = mkChart('cRain', [{label:'Rain', color:'#4cc2ff', area:true, fill:'rgba(76,194,255,.18)'}], {y:{min:0}});
  refresh();
  setInterval(refresh, 30000);
}
function card(label, val, unit) {
  return `<div class="card"><div class="label">${label}</div><div class="val">${val}<small> ${unit||''}</small></div></div>`;
}
function compass(deg){ const dirs=['N','NNE','NE','ENE','E','ESE','SE','SSE','S','SSW','SW','WSW','W','WNW','NW','NNW'];
  return dirs[Math.round(deg/22.5)%16]; }
async function refresh() {
  let d;
  try { d = await (await fetch('data.json')).json(); }
  catch(e){ document.getElementById('status').textContent='offline — retrying…'; return; }
  const u = d.units, c = d.current;
  // Alert banner
  const ab = document.getElementById('alert');
  if (c.alert) { ab.style.display='block'; ab.textContent='⚠ WEATHER ALERT — '+c.alert_reason; }
  else ab.style.display='none';
  // Status line
  const up = d.uptime_s; const h=Math.floor(up/3600), m=Math.floor(up%3600/60);
  document.getElementById('status').textContent =
    `${d.host}.local · ${c.locked?'receiving':'searching'} · ${c.rssi} dBm · up ${h}h ${m}m`;
  // Current cards
  document.getElementById('cards').innerHTML =
    card('Temperature', c.temp.toFixed(1), u.temp) +
    card('Dew Point', c.dew.toFixed(0), u.temp) +
    card('Humidity', c.hum.toFixed(0), '%') +
    `<div class="card"><div class="label">Wind</div><div class="val">${c.wind}<small> ${u.wind} ${compass(c.dir)}
       <span class="arrow" style="transform:rotate(${c.dir}deg)">↑</span></small></div></div>` +
    card('Gust', c.gust, u.wind) +
    card('Rain', c.rain.toFixed(2), u.rain);
  // Charts
  const H = d.history;            // [t, temp, dew, hum, wind, gust, rain]
  const now = d.uptime_s, wall = Date.now();
  const labels = H.map(r => { const dt = new Date(wall - (now - r[0])*1000);
    return dt.toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'}); });
  const col = i => H.map(r => r[i]);
  setData(charts.temp, labels, [col(1), col(2)]);
  setData(charts.hum,  labels, [col(3)]);
  setData(charts.wind, labels, [col(4), col(5)]);
  setData(charts.rain, labels, [col(6)]);
}
function setData(ch, labels, cols){ ch.data.labels=labels;
  cols.forEach((c,i)=>ch.data.datasets[i].data=c); ch.update(); }
init();
</script>
</body>
</html>)HTML";

// ===========================================================================
// Request handlers.
// ===========================================================================

static void handleRoot() {
  // Send the whole page straight from flash.
  server.send_P(200, "text/html", PAGE_HTML);
}

static void handleData() {
  // Stream the JSON in small pieces so we never need one big buffer.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  char buf[256];
  const char *tUnit = USE_IMPERIAL_UNITS ? "\\u00b0F" : "\\u00b0C";
  const char *wUnit = USE_IMPERIAL_UNITS ? "mph" : "km/h";
  const char *rUnit = USE_IMPERIAL_UNITS ? "in" : "mm";

  // Header + current conditions.
  snprintf(buf, sizeof(buf),
    "{\"host\":\"%s\",\"uptime_s\":%lu,"
    "\"units\":{\"temp\":\"%s\",\"wind\":\"%s\",\"rain\":\"%s\"},"
    "\"current\":{\"temp\":%.1f,\"dew\":%.1f,\"hum\":%u,\"wind\":%u,\"gust\":%u,"
    "\"dir\":%u,\"rain\":%.2f,\"rssi\":%d,\"locked\":%u,\"alert\":%u,\"alert_reason\":\"%s\"},",
    WEB_HOSTNAME, (unsigned long)(millis() / 1000UL),
    tUnit, wUnit, rUnit,
    outTemp(cur.temp10 / 10.0f), outTemp(cur.dew10 / 10.0f), cur.hum,
    (unsigned)outWind(cur.wind), (unsigned)outWind(cur.gust),
    curDir, outRain(cur.rain100 / 100.0f), cur.rssi,
    curLocked ? 1 : 0, curAlert ? 1 : 0, curReason);
  server.sendContent(buf);

  // History: an array of [t, temp, dew, hum, wind, gust, rain] rows, oldest
  // first. We walk the ring buffer from its oldest entry to its newest.
  server.sendContent("\"history\":[");
  uint16_t idx = (uint16_t)((ringHead + WEB_HISTORY_POINTS - ringCount) % WEB_HISTORY_POINTS);
  for (uint16_t i = 0; i < ringCount; i++) {
    const Sample &s = ring[idx];
    snprintf(buf, sizeof(buf), "%s[%lu,%.1f,%.1f,%u,%u,%u,%.2f]",
      (i == 0 ? "" : ","), (unsigned long)s.t,
      outTemp(s.temp10 / 10.0f), outTemp(s.dew10 / 10.0f), s.hum,
      (unsigned)outWind(s.wind), (unsigned)outWind(s.gust),
      outRain(s.rain100 / 100.0f));
    server.sendContent(buf);
    idx = (uint16_t)((idx + 1) % WEB_HISTORY_POINTS);
  }
  server.sendContent("]}");
}

// ===========================================================================
// Public functions.
// ===========================================================================

void webBegin() {
  if (!WEB_ENABLE) return;
  // Register the two URLs. We actually start listening lazily in webLoop(),
  // once WiFi is connected.
  server.on("/", handleRoot);
  server.on("/data.json", handleData);
}

void webLoop() {
  if (!WEB_ENABLE) return;

  // Only run once WiFi is up. If WiFi drops, reset so we re-announce on return.
  if (WiFi.status() != WL_CONNECTED) {
    serverStarted = false;
    return;
  }

  if (!serverStarted) {
    server.begin();
    // Publish the friendly "davis.local" name and advertise the web service.
    if (MDNS.begin(WEB_HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
    }
    Serial.print(F("[web] dashboard ready at http://"));
    Serial.print(WiFi.localIP());
    Serial.print(F("/  and  http://"));
    Serial.print(WEB_HOSTNAME);
    Serial.println(F(".local/"));
    serverStarted = true;
  }

  server.handleClient();
}

void webSample(const DavisData *data, float rssi, bool locked,
               bool alert, const char *alertReason) {
  if (!WEB_ENABLE) return;

  // Build a fresh snapshot of the current values.
  float dewF = davisDewPointF(data->tempF, data->humidityPct);
  Sample s;
  s.t       = millis() / 1000UL;
  s.temp10  = (int16_t)lroundf(data->tempF * 10.0f);
  s.dew10   = (int16_t)lroundf(dewF * 10.0f);
  s.hum     = (uint8_t)lroundf(data->humidityPct);
  s.wind    = (uint8_t)lroundf(data->windSpeedMph);
  s.gust    = (uint8_t)lroundf(data->windGustMph);
  s.rain100 = (uint16_t)lroundf(data->rainClicksTotal * 0.01f * 100.0f);
  s.rssi    = (int8_t)rssi;

  // Keep the header values fresh on every call.
  cur = s;
  curDir = data->windDirDegrees;
  curLocked = locked;
  curAlert = alert;
  strncpy(curReason, alertReason && alertReason[0] ? alertReason : "none", sizeof(curReason) - 1);
  curReason[sizeof(curReason) - 1] = '\0';

  // Only add a point to the history graphs once per sample interval.
  uint32_t now = millis();
  if (lastSampleMs == 0 || (now - lastSampleMs) >= (uint32_t)WEB_SAMPLE_SECONDS * 1000UL) {
    lastSampleMs = now;
    ring[ringHead] = s;
    ringHead = (uint16_t)((ringHead + 1) % WEB_HISTORY_POINTS);
    if (ringCount < WEB_HISTORY_POINTS) ringCount++;
  }
}
