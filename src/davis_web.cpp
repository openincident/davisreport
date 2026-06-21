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
#include <LittleFS.h>
#include <time.h>
#include "davis_web.h"
#include "davis_radio.h"   // for radioBadCount() in /metrics
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
#ifndef WEB_OWNER
#define WEB_OWNER ""
#endif
#ifndef WEB_CALLSIGN
#define WEB_CALLSIGN ""
#endif
#ifndef WEB_REPO_URL
#define WEB_REPO_URL "https://github.com/openincident/davisreport"
#endif
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif
#ifndef NTP_TZ
#define NTP_TZ "MST7"
#endif

// True once the clock has been set from the internet (epoch is past 2023).
static bool timeIsValid() { return time(nullptr) > 1700000000; }

// ---------------------------------------------------------------------------
// One stored history point. We pack the values into small types so the whole
// history stays tiny in memory (~14 bytes each).
// ---------------------------------------------------------------------------
struct Sample {
  uint32_t t;        // real (epoch) time in seconds when this point was recorded
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

// --- Persistence: the live ring is saved to flash so it survives a reboot. ---
static bool     fsReady = false;            // did the flash filesystem mount?
static uint32_t lastSnapshotMs = 0;         // when we last wrote the ring to flash
#define WEB_SNAPSHOT_SECONDS 600            // save the live ring every 10 minutes
#define LIVE_FILE  "/live.dat"
#define LIVE_MAGIC 0x44415631               // "DAV1" — sanity tag at the file start

// Write the whole ring (plus its head/count) to flash.
static void saveLive() {
  if (!fsReady) return;
  File f = LittleFS.open(LIVE_FILE, "w");
  if (!f) return;
  uint32_t magic = LIVE_MAGIC;
  f.write((const uint8_t *)&magic, 4);
  f.write((const uint8_t *)&ringCount, 2);
  f.write((const uint8_t *)&ringHead, 2);
  f.write((const uint8_t *)ring, sizeof(ring));
  f.close();
}

// Restore the ring from flash at startup (if a valid file is there).
static void loadLive() {
  if (!fsReady) return;
  File f = LittleFS.open(LIVE_FILE, "r");
  if (!f) return;
  uint32_t magic = 0; uint16_t cnt = 0, head = 0;
  bool ok = f.read((uint8_t *)&magic, 4) == 4 && magic == LIVE_MAGIC &&
            f.read((uint8_t *)&cnt, 2) == 2 && f.read((uint8_t *)&head, 2) == 2 &&
            f.read((uint8_t *)ring, sizeof(ring)) == sizeof(ring);
  f.close();
  if (ok && cnt <= WEB_HISTORY_POINTS && head < WEB_HISTORY_POINTS) {
    ringCount = cnt; ringHead = head;
    Serial.printf("[web] restored %u history points from flash\n", ringCount);
  }
}

// ===========================================================================
// LONG-TERM STORE — one summary record per HOUR (min/avg/max), kept ~180 days
// in a circular flash file. This is the on-board archive that feeds the
// "long-term" charts on the page. It's tiny (~82 KB) and writes only once an
// hour, so flash wear is negligible (a sector erase roughly once a week).
// ===========================================================================
#define LONG_FILE   "/longterm.dat"
#define LONG_MAGIC  0x44415648              // "DAVH"
#define LONG_DAYS   180
#define LONG_POINTS (LONG_DAYS * 24)        // 4320 hourly records
#define LONG_HDR    8                       // magic(4) + count(2) + head(2)

// One stored hour. Packed so the on-flash size is predictable. Temps are °F x10.
struct __attribute__((packed)) HourRec {
  uint32_t hour;                  // epoch seconds at the start of the hour
  int16_t  tAvg, tMin, tMax;      // temperature °F x10
  int16_t  dAvg;                  // dew point °F x10
  uint8_t  hAvg, hMin, hMax;      // humidity %
  uint8_t  wAvg, gMax;            // wind avg, gust max (mph)
  uint16_t rain100;               // rain that hour, inches x100
};
static uint16_t longCount = 0, longHead = 0;

// The running accumulator for the hour currently in progress (lives in RAM).
struct HourAcc {
  uint32_t hour;                  // which epoch-hour we're summing (0 = none yet)
  uint32_t n;                     // how many samples so far this hour
  double   tSum, dSum;            // sums (in x10 units) for the averages
  int16_t  tMin, tMax;
  uint32_t hSum; uint8_t hMin, hMax;
  uint32_t wSum; uint8_t gMax;
  uint16_t rainStart100, lastRain100;
};
static HourAcc acc;

// Create the long-term file (full size, zero-filled) the first time, or read
// its header if it already exists.
static void longInit() {
  if (!fsReady) return;
  memset(&acc, 0, sizeof(acc));
  File f = LittleFS.open(LONG_FILE, "r");
  if (f) {
    uint32_t m = 0;
    if (f.read((uint8_t *)&m, 4) == 4 && m == LONG_MAGIC) {
      f.read((uint8_t *)&longCount, 2);
      f.read((uint8_t *)&longHead, 2);
      f.close();
      Serial.printf("[web] long-term store: %u hourly records\n", longCount);
      return;
    }
    f.close();
  }
  Serial.println(F("[web] creating long-term store (~82 KB)..."));
  File w = LittleFS.open(LONG_FILE, "w");
  if (!w) return;
  uint32_t magic = LONG_MAGIC; uint16_t z = 0;
  w.write((uint8_t *)&magic, 4); w.write((uint8_t *)&z, 2); w.write((uint8_t *)&z, 2);
  HourRec blank; memset(&blank, 0, sizeof(blank));
  for (uint16_t i = 0; i < LONG_POINTS; i++) w.write((uint8_t *)&blank, sizeof(blank));
  w.close();
  longCount = 0; longHead = 0;
}

// Append one finished hour to the circular file (overwriting the oldest once
// full), and update the header.
static void longAppend(const HourRec &r) {
  if (!fsReady) return;
  File f = LittleFS.open(LONG_FILE, "r+");
  if (!f) return;
  f.seek(LONG_HDR + (uint32_t)longHead * sizeof(HourRec));
  f.write((const uint8_t *)&r, sizeof(HourRec));
  longHead = (uint16_t)((longHead + 1) % LONG_POINTS);
  if (longCount < LONG_POINTS) longCount++;
  f.seek(4);
  f.write((uint8_t *)&longCount, 2);
  f.write((uint8_t *)&longHead, 2);
  f.close();
}

// Turn the in-progress accumulator into a stored record.
static void longFinalize() {
  if (acc.hour == 0 || acc.n == 0) return;
  HourRec r;
  r.hour    = acc.hour * 3600UL;
  r.tAvg    = (int16_t)lround(acc.tSum / acc.n);
  r.tMin    = acc.tMin; r.tMax = acc.tMax;
  r.dAvg    = (int16_t)lround(acc.dSum / acc.n);
  r.hAvg    = (uint8_t)(acc.hSum / acc.n); r.hMin = acc.hMin; r.hMax = acc.hMax;
  r.wAvg    = (uint8_t)(acc.wSum / acc.n); r.gMax = acc.gMax;
  r.rain100 = (acc.lastRain100 >= acc.rainStart100)
                ? (uint16_t)(acc.lastRain100 - acc.rainStart100) : 0;
  longAppend(r);
}

// HOUR_DIVISOR is normally 3600 (seconds per hour). It's a #define only so the
// test harness can shrink "an hour" to a minute and exercise the rollover fast.
#ifndef HOUR_DIVISOR
#define HOUR_DIVISOR 3600UL
#endif

// Feed one live sample into the current hour's accumulator; roll over and store
// when the hour changes. Called every web sample while locked.
static void longTick(uint32_t epoch, float tF, float dF,
                     uint8_t hum, uint8_t wind, uint8_t gust, uint16_t rain100) {
  uint32_t hr = epoch / HOUR_DIVISOR;
  int16_t t10 = (int16_t)lroundf(tF * 10.0f);
  if (acc.hour != hr) {                 // new hour (or the very first sample)
    if (acc.hour != 0) longFinalize();  // close out the previous hour
    memset(&acc, 0, sizeof(acc));
    acc.hour = hr;
    acc.tMin = acc.tMax = t10;
    acc.hMin = acc.hMax = hum;
    acc.rainStart100 = rain100;
  }
  acc.n++;
  acc.tSum += tF * 10.0f; acc.dSum += dF * 10.0f;
  if (t10 < acc.tMin) acc.tMin = t10;
  if (t10 > acc.tMax) acc.tMax = t10;
  acc.hSum += hum; if (hum < acc.hMin) acc.hMin = hum; if (hum > acc.hMax) acc.hMax = hum;
  acc.wSum += wind; if (gust > acc.gMax) acc.gMax = gust;
  if (rain100 < acc.rainStart100) acc.rainStart100 = rain100;  // rain counter reset on reboot
  acc.lastRain100 = rain100;
}

// The most recent values (kept fresh every call, for the page header).
static Sample   cur;
static uint16_t curDir = 0;
static bool     curLocked = false;
static bool     curAlert = false;
static char     curReason[20] = "none";
// Extra current values, kept for the /metrics (Prometheus) endpoint.
static uint16_t curSolar = 0;
static uint16_t curSupercap100 = 0;   // supercap volts x 100
static uint32_t curGood = 0;          // good-packet count
static bool     curBattLow = false;

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
  a { color:var(--accent); text-decoration:none; }
  a:hover { text-decoration:underline; }
  #needle { transition:transform .6s ease; }
  #compass text { font-family:system-ui,sans-serif; }
  h2 { font-size:16px; margin:26px 0 8px; }
  .lt-controls { margin-bottom:12px; }
  .lt-controls button { background:var(--card); color:var(--ink); border:1px solid #2a3445;
    padding:6px 12px; border-radius:8px; margin-right:6px; cursor:pointer; font-size:13px; }
  .lt-controls button.on { background:var(--accent); color:#04202e; border-color:var(--accent); font-weight:600; }
</style>
</head>
<body>
<div id="alert"></div>
<header>
  <h1>🌤️ Davis Weather</h1>
  <span class="sub" id="status">connecting…</span>
  <span class="sub" id="tagtop" style="margin-left:auto"></span>
</header>
<div class="wrap">
  <div class="cards" id="cards"></div>
  <div class="chartcard">
    <h3>Wind Direction</h3>
    <div style="display:flex;align-items:center;gap:28px;justify-content:center;flex-wrap:wrap">
      <svg id="compass" width="180" height="180" viewBox="0 0 200 200">
        <circle cx="100" cy="100" r="88" fill="#141a24" stroke="#2a3445" stroke-width="2"/>
        <g id="ticks" stroke="#3a465a" stroke-width="2"></g>
        <text x="100" y="27" text-anchor="middle" fill="#ff5252" font-size="16" font-weight="700">N</text>
        <text x="185" y="106" text-anchor="middle" fill="#8b98a9" font-size="14">E</text>
        <text x="100" y="191" text-anchor="middle" fill="#8b98a9" font-size="14">S</text>
        <text x="15" y="106" text-anchor="middle" fill="#8b98a9" font-size="14">W</text>
        <g id="needle" transform="rotate(0 100 100)">
          <polygon points="100,32 93,104 107,104" fill="#ff5252"/>
          <polygon points="100,168 93,96 107,96" fill="#56627a"/>
          <circle cx="100" cy="100" r="7" fill="#e6edf3"/>
        </g>
      </svg>
      <div style="text-align:center">
        <div class="val" id="windText" style="font-size:34px">—</div>
        <div class="label" id="windSub">wind from</div>
      </div>
    </div>
  </div>
  <div class="chartcard"><h3>Temperature</h3><canvas id="cTemp" height="110"></canvas></div>
  <div class="chartcard"><h3>Dew Point</h3><canvas id="cDew" height="100"></canvas></div>
  <div class="chartcard"><h3>Humidity</h3><canvas id="cHum" height="100"></canvas></div>
  <div class="chartcard"><h3>Wind &amp; Gust</h3><canvas id="cWind" height="100"></canvas></div>
  <div class="chartcard"><h3>Rain (since startup)</h3><canvas id="cRain" height="100"></canvas></div>

  <h2>Long-term history</h2>
  <div class="lt-controls" id="ltSpan">
    <button data-d="7">7 days</button><button data-d="30">30 days</button>
    <button data-d="90" class="on">90 days</button><button data-d="180">180 days</button>
  </div>
  <div class="chartcard"><h3>Temperature — avg with min/max band</h3><canvas id="cLT_temp" height="120"></canvas></div>
  <div class="chartcard"><h3>Humidity — avg with min/max band</h3><canvas id="cLT_hum" height="100"></canvas></div>
  <div class="chartcard"><h3>Wind avg &amp; gust max</h3><canvas id="cLT_wind" height="100"></canvas></div>
  <div class="chartcard"><h3>Rain per hour</h3><canvas id="cLT_rain" height="100"></canvas></div>

  <footer id="foot">Served by the LilyGO board · live history resets when the board reboots</footer>
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
function buildCompass() {
  // Draw the tick marks around the compass face (long ones at N/E/S/W).
  const ticks = document.getElementById('ticks');
  for (let a = 0; a < 360; a += 30) {
    const r1 = (a % 90 === 0) ? 72 : 80, rad = a * Math.PI / 180;
    const ln = document.createElementNS('http://www.w3.org/2000/svg', 'line');
    ln.setAttribute('x1', (100 + r1*Math.sin(rad)).toFixed(1));
    ln.setAttribute('y1', (100 - r1*Math.cos(rad)).toFixed(1));
    ln.setAttribute('x2', (100 + 88*Math.sin(rad)).toFixed(1));
    ln.setAttribute('y2', (100 - 88*Math.cos(rad)).toFixed(1));
    ticks.appendChild(ln);
  }
}
function init() {
  buildCompass();
  charts.temp = mkChart('cTemp', [{label:'Temp', color:'#ff8a5b', area:true, fill:'rgba(255,138,91,.12)'}]);
  charts.dew  = mkChart('cDew',  [{label:'Dew Point', color:'#4cc2ff', area:true, fill:'rgba(76,194,255,.12)'}]);
  charts.hum  = mkChart('cHum',  [{label:'Humidity %', color:'#7ee787', area:true, fill:'rgba(126,231,135,.12)'}], {y:{min:0,max:100}});
  charts.wind = mkChart('cWind', [{label:'Wind', color:'#a5a5ff'}, {label:'Gust', color:'#ff5252'}], {y:{min:0}});
  charts.rain = mkChart('cRain', [{label:'Rain', color:'#4cc2ff', area:true, fill:'rgba(76,194,255,.18)'}], {y:{min:0}});
  initLong();
  refresh();
  setInterval(refresh, 30000);
}
// ---- Long-term charts (fed by /longterm.json, hourly min/avg/max) ----
const lt = {}; let ltDays = 90;
function mkBand(id, color, band, opts={}) {
  // 3 datasets: max (fills down to min = the shaded band), min (invisible), avg (line).
  return new Chart(document.getElementById(id), {
    type:'line',
    data:{labels:[], datasets:[
      {label:'max', data:[], borderColor:'transparent', backgroundColor:band, fill:'+1', pointRadius:0, tension:.3},
      {label:'min', data:[], borderColor:'transparent', fill:false, pointRadius:0, tension:.3},
      {label:'avg', data:[], borderColor:color, borderWidth:2, fill:false, pointRadius:0, tension:.3} ]},
    options:{animation:false, responsive:true, interaction:{intersect:false,mode:'index'},
      scales:{x:{ticks:{color:'#8b98a9',maxTicksLimit:8},grid:{color:'#232c3b'}},
              y:{ticks:{color:'#8b98a9'},grid:{color:'#232c3b'},...(opts.y||{})}},
      plugins:{legend:{display:false}}}});
}
function initLong() {
  lt.temp = mkBand('cLT_temp', '#ff8a5b', 'rgba(255,138,91,.16)');
  lt.hum  = mkBand('cLT_hum',  '#7ee787', 'rgba(126,231,135,.16)', {y:{min:0,max:100}});
  lt.wind = mkChart('cLT_wind', [{label:'Wind avg', color:'#a5a5ff'}, {label:'Gust max', color:'#ff5252'}], {y:{min:0}});
  lt.rain = new Chart(document.getElementById('cLT_rain'), {type:'bar',
    data:{labels:[], datasets:[{label:'Rain/hr', data:[], backgroundColor:'#4cc2ff'}]},
    options:{animation:false, responsive:true,
      scales:{x:{ticks:{color:'#8b98a9',maxTicksLimit:8},grid:{display:false}},
              y:{min:0, ticks:{color:'#8b98a9'}, grid:{color:'#232c3b'}}},
      plugins:{legend:{display:false}}}});
  document.querySelectorAll('#ltSpan button').forEach(b => b.onclick = () => {
    ltDays = +b.dataset.d;
    document.querySelectorAll('#ltSpan button').forEach(x => x.classList.toggle('on', x===b));
    refreshLong();
  });
  refreshLong();
  setInterval(refreshLong, 300000);   // long-term changes hourly; refresh every 5 min
}
async function refreshLong() {
  let d; try { d = await (await fetch('longterm.json?days='+ltDays)).json(); } catch(e){ return; }
  const R = d.rows;   // [hour,tAvg,tMin,tMax,dAvg,hAvg,hMin,hMax,wAvg,gMax,rain]
  const fmt = ltDays <= 7 ? {month:'short',day:'numeric',hour:'2-digit'} : {month:'short',day:'numeric'};
  const labels = R.map(r => new Date(r[0]*1000).toLocaleDateString([], fmt));
  const c = i => R.map(r => r[i]);
  setBand(lt.temp, labels, c(3), c(2), c(1));        // tMax,tMin,tAvg
  setBand(lt.hum,  labels, c(7), c(6), c(5), {floor:0,ceil:100});  // hMax,hMin,hAvg
  setData(lt.wind, labels, [c(8), c(9)], {floor:0}); // wAvg, gMax
  lt.rain.data.labels = labels; lt.rain.data.datasets[0].data = c(10); lt.rain.update();
}
function setBand(ch, labels, max, min, avg, range) {
  ch.data.labels = labels;
  ch.data.datasets[0].data = max; ch.data.datasets[1].data = min; ch.data.datasets[2].data = avg;
  autoRange(ch, range);   // spans min..max already since all three series are included
  ch.update();
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
  // Owner / callsign / repo tagline, top and bottom.
  const tag = [d.owner, d.callsign].filter(Boolean).join(' · ');
  document.getElementById('tagtop').textContent = tag;
  const repoShort = (d.repo||'').replace(/^https?:\/\//,'');
  document.getElementById('foot').innerHTML =
    (tag ? tag+' · ' : '') +
    (d.repo ? `<a href="${d.repo}" target="_blank" rel="noopener">${repoShort}</a> · ` : '') +
    'history resets when the board reboots';
  // Current cards
  document.getElementById('cards').innerHTML =
    card('Temperature', c.temp.toFixed(1), u.temp) +
    card('Dew Point', c.dew.toFixed(0), u.temp) +
    card('Humidity', c.hum.toFixed(0), '%') +
    `<div class="card"><div class="label">Wind</div><div class="val">${c.wind}<small> ${u.wind} ${compass(c.dir)}
       <span class="arrow" style="transform:rotate(${c.dir}deg)">↑</span></small></div></div>` +
    card('Gust', c.gust, u.wind) +
    card('Rain', c.rain.toFixed(2), u.rain);
  // Wind compass: rotate the needle so its red tip points to where the wind is
  // coming FROM (like a weather vane). 0° = N at the top, turning clockwise.
  document.getElementById('needle').setAttribute('transform', `rotate(${c.dir} 100 100)`);
  document.getElementById('windText').textContent = `${c.dir}° ${compass(c.dir)}`;
  document.getElementById('windSub').textContent =
    `wind from · ${c.wind} ${u.wind}` + (c.gust ? ` (gust ${c.gust})` : '');
  // Charts
  const H = d.history;            // [t, temp, dew, hum, wind, gust, rain]  (t = epoch seconds)
  const labels = H.map(r => new Date(r[0]*1000)
    .toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'}));
  const col = i => H.map(r => r[i]);
  setData(charts.temp, labels, [col(1)]);                          // temperature: percentile
  setData(charts.dew,  labels, [col(2)]);                          // dew point: its own scale
  setData(charts.hum,  labels, [col(3)], {floor:0, ceil:100});     // humidity: 0..100 cap
  setData(charts.wind, labels, [col(4), col(5)], {floor:0});       // wind & gust: never below 0
  setData(charts.rain, labels, [col(6)], {lo:0, hi:1, floor:0});   // rain: full range from 0
}
// Pick a y-axis range from the data's 5th-95th percentiles (plus 10% padding)
// so each chart zooms into the meaningful variation and a single spike or
// dropout doesn't squash everything flat. `o` may set lo/hi percentiles and a
// floor/ceil (e.g. humidity can't drop below 0 or rise above 100).
function pctile(sorted, p){ const i=(sorted.length-1)*p, lo=Math.floor(i), hi=Math.ceil(i);
  return sorted[lo] + (sorted[hi]-sorted[lo])*(i-lo); }
function autoRange(ch, o){ o=o||{};
  const vals=ch.data.datasets.flatMap(d=>d.data).filter(v=>v!=null&&!isNaN(v)).sort((a,b)=>a-b);
  const y=ch.options.scales.y;
  if(vals.length<2){ y.min=undefined; y.max=undefined; return; }
  let lo=pctile(vals, o.lo!=null?o.lo:0.05), hi=pctile(vals, o.hi!=null?o.hi:0.95);
  if(hi-lo < 1e-6){ lo-=1; hi+=1; }              // flat data: give it some height
  const pad=(hi-lo)*0.1;
  let min=lo-pad, max=hi+pad;
  if(o.floor!=null) min=Math.max(o.floor, min);
  if(o.ceil!=null)  max=Math.min(o.ceil, max);
  y.min=Math.round(min*10)/10; y.max=Math.round(max*10)/10;
}
function setData(ch, labels, cols, range){ ch.data.labels=labels;
  cols.forEach((c,i)=>ch.data.datasets[i].data=c); autoRange(ch, range); ch.update(); }
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

  // Identity (name/callsign/repo) + units. Sent as its own chunk so we don't
  // crowd the buffer.
  snprintf(buf, sizeof(buf),
    "{\"host\":\"%s\",\"owner\":\"%s\",\"callsign\":\"%s\",\"repo\":\"%s\",\"uptime_s\":%lu,"
    "\"units\":{\"temp\":\"%s\",\"wind\":\"%s\",\"rain\":\"%s\"},",
    WEB_HOSTNAME, WEB_OWNER, WEB_CALLSIGN, WEB_REPO_URL,
    (unsigned long)(millis() / 1000UL), tUnit, wUnit, rUnit);
  server.sendContent(buf);

  // Current conditions.
  snprintf(buf, sizeof(buf),
    "\"current\":{\"temp\":%.1f,\"dew\":%.1f,\"hum\":%u,\"wind\":%u,\"gust\":%u,"
    "\"dir\":%u,\"rain\":%.2f,\"rssi\":%d,\"locked\":%u,\"alert\":%u,\"alert_reason\":\"%s\"},",
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

// ---------------------------------------------------------------------------
// /metrics — Prometheus exposition format. A tool like Grafana Alloy scrapes
// this every few seconds and ships it to Grafana Cloud (or any Prometheus), so
// long-term history lives there with no work on the board's part. Each line is
// "metric_name value"; HELP/TYPE lines describe them.
// ---------------------------------------------------------------------------
static void handleMetrics() {
  char b[1700];
  // We always export base imperial units (°F / mph / inches) with explicit
  // names, so the meaning is unambiguous no matter the display setting.
  snprintf(b, sizeof(b),
    "# HELP davis_temperature_fahrenheit Outdoor temperature\n# TYPE davis_temperature_fahrenheit gauge\ndavis_temperature_fahrenheit %.1f\n"
    "# HELP davis_dew_point_fahrenheit Dew point\n# TYPE davis_dew_point_fahrenheit gauge\ndavis_dew_point_fahrenheit %.1f\n"
    "# HELP davis_humidity_percent Relative humidity\n# TYPE davis_humidity_percent gauge\ndavis_humidity_percent %u\n"
    "# HELP davis_wind_speed_mph Wind speed\n# TYPE davis_wind_speed_mph gauge\ndavis_wind_speed_mph %u\n"
    "# HELP davis_wind_gust_mph Wind gust\n# TYPE davis_wind_gust_mph gauge\ndavis_wind_gust_mph %u\n"
    "# HELP davis_wind_direction_degrees Wind direction\n# TYPE davis_wind_direction_degrees gauge\ndavis_wind_direction_degrees %u\n"
    "# HELP davis_rain_total_inches Rain since startup\n# TYPE davis_rain_total_inches gauge\ndavis_rain_total_inches %.2f\n"
    "# HELP davis_solar_wm2 Solar radiation\n# TYPE davis_solar_wm2 gauge\ndavis_solar_wm2 %u\n"
    "# HELP davis_supercap_volts ISS solar capacitor voltage\n# TYPE davis_supercap_volts gauge\ndavis_supercap_volts %.2f\n"
    "# HELP davis_signal_dbm Radio signal strength\n# TYPE davis_signal_dbm gauge\ndavis_signal_dbm %d\n"
    "# HELP davis_radio_locked Receiver locked onto the station (1/0)\n# TYPE davis_radio_locked gauge\ndavis_radio_locked %u\n"
    "# HELP davis_alert Severe-weather alert active (1/0)\n# TYPE davis_alert gauge\ndavis_alert %u\n"
    "# HELP davis_battery_low ISS battery low (1/0)\n# TYPE davis_battery_low gauge\ndavis_battery_low %u\n"
    "# HELP davis_packets_good_total Valid packets received\n# TYPE davis_packets_good_total counter\ndavis_packets_good_total %lu\n"
    "# HELP davis_packets_bad_total Failed-checksum receptions\n# TYPE davis_packets_bad_total counter\ndavis_packets_bad_total %lu\n"
    "# HELP davis_uptime_seconds Seconds since boot\n# TYPE davis_uptime_seconds counter\ndavis_uptime_seconds %lu\n",
    cur.temp10 / 10.0f, cur.dew10 / 10.0f, cur.hum, cur.wind, cur.gust, curDir,
    cur.rain100 / 100.0f, curSolar, curSupercap100 / 100.0f, cur.rssi,
    curLocked ? 1 : 0, curAlert ? 1 : 0, curBattLow ? 1 : 0,
    (unsigned long)curGood, (unsigned long)radioBadCount(),
    (unsigned long)(millis() / 1000UL));
  server.send(200, "text/plain; version=0.0.4", b);
}

// ---------------------------------------------------------------------------
// /longterm.json?days=N — the hourly archive for the long-term charts. Returns
// rows [hourEpoch, tAvg, tMin, tMax, dAvg, hAvg, hMin, hMax, wAvg, gMax, rain]
// for the last N days, downsampled to ~600 points so the chart stays snappy.
// ---------------------------------------------------------------------------
static void handleLongterm() {
  int days = server.hasArg("days") ? server.arg("days").toInt() : 180;
  if (days < 1) days = 1;
  if (days > LONG_DAYS) days = LONG_DAYS;
  uint32_t now = (uint32_t)time(nullptr);
  uint32_t cutoff = (now > (uint32_t)days * 86400UL) ? now - (uint32_t)days * 86400UL : 0;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  char buf[220];
  snprintf(buf, sizeof(buf), "{\"now\":%lu,\"days\":%d,\"tempUnit\":\"%s\",\"rows\":[",
           (unsigned long)now, days, USE_IMPERIAL_UNITS ? "\\u00b0F" : "\\u00b0C");
  server.sendContent(buf);

  if (fsReady && longCount > 0) {
    File f = LittleFS.open(LONG_FILE, "r");
    if (f) {
      uint16_t oldest = (uint16_t)((longHead + LONG_POINTS - longCount) % LONG_POINTS);
      // Downsample: at most ~600 points. Records are hourly, so days*24 bounds it.
      uint32_t inSpan = (uint32_t)days * 24; if (inSpan > longCount) inSpan = longCount;
      uint16_t stride = (inSpan > 600) ? (uint16_t)(inSpan / 600) : 1;
      uint16_t seen = 0; bool first = true;
      for (uint16_t i = 0; i < longCount; i++) {
        uint16_t idx = (uint16_t)((oldest + i) % LONG_POINTS);
        HourRec r;
        f.seek(LONG_HDR + (uint32_t)idx * sizeof(HourRec));
        f.read((uint8_t *)&r, sizeof(r));
        if (r.hour == 0 || r.hour < cutoff) continue;
        if ((seen++ % stride) != 0) continue;
        snprintf(buf, sizeof(buf),
          "%s[%lu,%.1f,%.1f,%.1f,%.1f,%u,%u,%u,%u,%u,%.2f]",
          first ? "" : ",", (unsigned long)r.hour,
          outTemp(r.tAvg / 10.0f), outTemp(r.tMin / 10.0f), outTemp(r.tMax / 10.0f),
          outTemp(r.dAvg / 10.0f), r.hAvg, r.hMin, r.hMax,
          (unsigned)outWind(r.wAvg), (unsigned)outWind(r.gMax), outRain(r.rain100 / 100.0f));
        server.sendContent(buf);
        first = false;
      }
      f.close();
    }
  }
  server.sendContent("]}");
}

// /clear?confirm=yes — wipe all stored history (live ring + long-term store).
static void handleClear() {
  if (server.arg("confirm") != "yes") {
    server.send(400, "text/plain", "To erase all stored history, call /clear?confirm=yes");
    return;
  }
  if (fsReady) { LittleFS.remove(LIVE_FILE); LittleFS.remove(LONG_FILE); }
  ringCount = 0; ringHead = 0;
  longCount = 0; longHead = 0;
  memset(&acc, 0, sizeof(acc));
  longInit();
  server.send(200, "text/plain", "All stored history cleared.");
}

void webBegin() {
  if (!WEB_ENABLE) return;
  // Mount the flash filesystem (format it the first time) and restore any saved
  // history, so the live charts pick up where they left off after a reboot.
  fsReady = LittleFS.begin(true);
  if (fsReady) { loadLive(); longInit(); }
  else Serial.println(F("[web] LittleFS mount failed; history won't persist"));
  // Register the URLs. We actually start listening lazily in webLoop(),
  // once WiFi is connected.
  server.on("/", handleRoot);
  server.on("/data.json", handleData);
  server.on("/longterm.json", handleLongterm);
  server.on("/clear", handleClear);
  server.on("/metrics", handleMetrics);
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
    // Ask the internet for the current time (the board has no clock of its own).
    configTzTime(NTP_TZ, NTP_SERVER);
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
  s.t       = (uint32_t)time(nullptr);   // real (epoch) time
  s.temp10  = (int16_t)lroundf(data->tempF * 10.0f);
  s.dew10   = (int16_t)lroundf(dewF * 10.0f);
  s.hum     = (uint8_t)lroundf(data->humidityPct);
  s.wind    = (uint8_t)lroundf(data->windSpeedMph);
  s.gust    = (uint8_t)lroundf(data->windGustMph);
  s.rain100 = (uint16_t)lroundf(data->rainClicksTotal * 0.01f * 100.0f);
  // Clamp into int8 range so the pre-lock "-999" sentinel doesn't wrap to a
  // bogus positive number; it shows as -128 ("no signal") instead.
  s.rssi    = (int8_t)constrain((int)lroundf(rssi), -128, 127);

  // Keep the header values fresh on every call.
  cur = s;
  curDir = data->windDirDegrees;
  curLocked = locked;
  curAlert = alert;
  curSolar = data->solarWm2;
  curSupercap100 = (uint16_t)lroundf(data->superCapVolts * 100.0f);
  curGood = data->goodPacketCount;
  curBattLow = data->batteryLow;
  strncpy(curReason, alertReason && alertReason[0] ? alertReason : "none", sizeof(curReason) - 1);
  curReason[sizeof(curReason) - 1] = '\0';

  // Record only when we're locked, the clock is set, AND we've actually heard a
  // real temperature and humidity at least once. Right after lock, the station's
  // first packet is wind only — temp/humidity arrive a few seconds later — so
  // this stops a placeholder 0°F/0% from leaking into the stored stats.
  bool haveData = locked && timeIsValid() &&
                  data->lastTempUpdate != 0 && data->lastHumidityUpdate != 0;

  // Feed the long-term (hourly) store on every good sample.
  if (haveData) {
    longTick((uint32_t)time(nullptr), data->tempF, dewF, s.hum, s.wind, s.gust, s.rain100);
  }

  // Only add a point to the live history graphs once per sample interval.
  uint32_t now = millis();
  if (haveData &&
      (lastSampleMs == 0 || (now - lastSampleMs) >= (uint32_t)WEB_SAMPLE_SECONDS * 1000UL)) {
    lastSampleMs = now;
    ring[ringHead] = s;
    ringHead = (uint16_t)((ringHead + 1) % WEB_HISTORY_POINTS);
    if (ringCount < WEB_HISTORY_POINTS) ringCount++;
  }

  // Save the ring to flash every so often so a reboot doesn't lose it.
  if (ringCount > 0 && (now - lastSnapshotMs) >= (uint32_t)WEB_SNAPSHOT_SECONDS * 1000UL) {
    lastSnapshotMs = now;
    saveLive();
  }
}
