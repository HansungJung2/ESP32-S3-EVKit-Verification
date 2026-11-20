#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include "debug_cli.h"

// ── RGB LED 설정 (WS2812B on GPIO48) ──────────────────────────
#define LED_PIN             48
#define NUM_LEDS            1
#define BRIGHTNESS_DEFAULT  80

CRGB leds[NUM_LEDS];

// ── LED 상태 변수 ──────────────────────────────────────────────
static uint8_t  g_mode         = 0;      // 0:rainbow 1:pulse 2:off 3:manual
static uint8_t  g_hue          = 0;
static uint8_t  g_brightness   = BRIGHTNESS_DEFAULT;
static uint8_t  g_pulse_val    = 0;
static int8_t   g_pulse_dir    = 1;
static CRGB     g_manual_rgb   = CRGB::Black;

static uint32_t g_last_led_ms  = 0;

// ── WebServer 객체 (Port 80) ──────────────────────────────────
static WebServer server(80);

// ── LED 제어 함수 (debug_cli.cpp에서 참조) ─────────────────────
void LED_SetColor(uint8_t r, uint8_t g, uint8_t b)
{
    g_manual_rgb = CRGB(r, g, b);
    g_mode       = 3; // manual mode
}

void LED_SetMode(uint8_t mode)
{
    g_mode = mode;
}

void LED_SetBrightness(uint8_t bright)
{
    g_brightness = bright;
    FastLED.setBrightness(bright);
}

uint8_t LED_GetMode(void)
{
    return g_mode;
}

// ── HTML 웹 대시보드 페이지 (실시간 리소스 그래프 포함) ────────
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32-S3 Resource Monitor & Controller</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #121214; color: #e1e1e6; text-align: center; margin: 0; padding: 15px; }
    .card { background: #202024; border-radius: 12px; padding: 16px; margin: 12px auto; max-width: 480px; box-shadow: 0 4px 12px rgba(0,0,0,0.4); text-align: left; }
    h1 { font-size: 19px; color: #04d361; margin: 0 0 4px 0; text-align: center; }
    .subtitle { font-size: 12px; color: #8d8d99; margin-bottom: 12px; text-align: center; }
    .section-title { font-size: 14px; font-weight: 600; color: #04d361; border-bottom: 1px solid #323238; padding-bottom: 5px; margin: 12px 0 8px 0; }
    
    /* Meter Bars */
    .metric { margin: 8px 0; font-size: 12px; }
    .metric-header { display: flex; justify-content: space-between; margin-bottom: 4px; }
    .bar-bg { background: #323238; height: 10px; border-radius: 5px; overflow: hidden; }
    .bar-fill { height: 100%; width: 0%; border-radius: 5px; transition: width 0.3s; }
    .fill-green { background: #04d361; }
    .fill-blue  { background: #1ea2df; }
    .fill-orange{ background: #fba94c; }

    /* Chart Canvas */
    canvas { background: #18181b; border: 1px solid #29292e; border-radius: 8px; width: 100%; height: 110px; display: block; margin: 8px 0; }

    /* Controls */
    .ctrl-row { display: flex; align-items: center; justify-content: space-around; margin: 10px 0; }
    .btn { background: #323238; color: #fff; border: 1px solid #4a4a52; border-radius: 6px; padding: 7px 12px; font-size: 12px; cursor: pointer; transition: 0.2s; }
    .btn:hover { background: #04d361; color: #000; border-color: #04d361; }
    .btn-red { border-color: #f75a68; color: #f75a68; }
    .btn-red:hover { background: #f75a68; color: #fff; }
    input[type=color] { border: none; width: 44px; height: 44px; border-radius: 50%; cursor: pointer; background: transparent; }
    input[type=range] { width: 100%; accent-color: #04d361; }

    /* CLI */
    #cli-input { width: calc(100% - 70px); padding: 7px; background: #121214; border: 1px solid #4a4a52; border-radius: 6px; color: #fff; box-sizing: border-box; }
    #cli-log { background: #000; color: #04d361; font-family: monospace; font-size: 11px; padding: 8px; border-radius: 6px; height: 100px; overflow-y: scroll; margin-top: 8px; white-space: pre-wrap; }
    .badge { background: #29292e; padding: 2px 6px; border-radius: 4px; font-size: 11px; }
  </style>
</head>
<body>
  <div class="card">
    <h1>ESP32-S3 Resource Monitor</h1>
    <div class="subtitle">Xtensa Dual-Core 240MHz | 16MB Flash | 8MB OPI PSRAM</div>

    <!-- 1. Realtime Hardware Stats -->
    <div class="section-title">📊 Real-Time Resource Usage</div>
    
    <div class="metric">
      <div class="metric-header">
        <span>SRAM Heap (<span id="heap-text">0 / 0 KB</span>)</span>
        <span id="heap-pct" class="badge">0%</span>
      </div>
      <div class="bar-bg"><div id="heap-bar" class="bar-fill fill-green"></div></div>
    </div>

    <div class="metric">
      <div class="metric-header">
        <span>8MB PSRAM (<span id="psram-text">0 / 0 MB</span>)</span>
        <span id="psram-pct" class="badge">0%</span>
      </div>
      <div class="bar-bg"><div id="psram-bar" class="bar-fill fill-blue"></div></div>
    </div>

    <div style="display: flex; justify-content: space-between; font-size: 12px; margin-top: 8px;">
      <div>🌡️ Chip Temp: <span id="temp-val" class="badge">--.- °C</span></div>
      <div>📶 Wi-Fi RSSI: <span id="rssi-val" class="badge">-- dBm</span></div>
      <div>⏱️ Uptime: <span id="uptime-val" class="badge">0s</span></div>
    </div>

    <!-- 2. Dynamic Realtime Chart -->
    <div class="section-title">📈 Memory & Temp History (Live 30s)</div>
    <canvas id="liveChart" width="440" height="110"></canvas>
    <div style="font-size: 10px; display: flex; justify-content: space-around; color: #8d8d99;">
      <span style="color:#04d361">■ Heap Usage (%)</span>
      <span style="color:#1ea2df">■ PSRAM Usage (%)</span>
      <span style="color:#fba94c">■ Chip Temp (°C)</span>
    </div>

    <!-- 3. RGB LED Controller -->
    <div class="section-title">🎨 GPIO48 RGB LED Control</div>
    <div class="ctrl-row">
      <input type="color" id="picker" value="#ff0000" onchange="sendColor(this.value)">
      <button class="btn" onclick="sendMode('rainbow')">🌈 Rainbow</button>
      <button class="btn" onclick="sendMode('pulse')">💙 Pulse</button>
      <button class="btn btn-red" onclick="sendMode('off')">⬛ OFF</button>
    </div>
    <div style="font-size: 12px; margin-bottom: 2px;">Brightness: <span id="bright-val">80</span></div>
    <input type="range" min="0" max="255" value="80" oninput="document.getElementById('bright-val').innerText=this.value; sendBright(this.value)">

    <!-- 4. Web CLI -->
    <div class="section-title">💻 Web CLI Terminal</div>
    <div style="display:flex; justify-content:space-between;">
      <input type="text" id="cli-input" placeholder="top, sysinfo, help..." onkeydown="if(event.key==='Enter') sendCLI()">
      <button class="btn" onclick="sendCLI()" style="width:60px;">Send</button>
    </div>
    <div id="cli-log">ESP32-S3 Web Terminal Ready...\n</div>
  </div>

  <script>
    const MAX_POINTS = 30;
    let heapHist = new Array(MAX_POINTS).fill(0);
    let psramHist = new Array(MAX_POINTS).fill(0);
    let tempHist = new Array(MAX_POINTS).fill(0);

    function log(msg) {
      const el = document.getElementById('cli-log');
      el.innerText += msg + '\n';
      el.scrollTop = el.scrollHeight;
    }

    function sendColor(hex) {
      const r = parseInt(hex.substr(1,2), 16);
      const g = parseInt(hex.substr(3,2), 16);
      const b = parseInt(hex.substr(5,2), 16);
      fetch(`/api/led?r=${r}&g=${g}&b=${b}`).then(r => r.text()).then(log);
    }
    function sendMode(m) { fetch(`/api/mode?val=${m}`).then(r => r.text()).then(log); }
    function sendBright(b) { fetch(`/api/bright?val=${b}`).then(r => r.text()).then(log); }
    function sendCLI() {
      const input = document.getElementById('cli-input');
      const cmd = input.value.trim();
      if (!cmd) return;
      log('> ' + cmd);
      fetch(`/api/cli?cmd=${encodeURIComponent(cmd)}`).then(r => r.text()).then(log);
      input.value = '';
    }

    // ── Live Chart Drawer ──
    const canvas = document.getElementById('liveChart');
    const ctx = canvas.getContext('2d');

    function drawChart() {
      const w = canvas.width;
      const h = canvas.height;
      ctx.clearRect(0, 0, w, h);

      // Grid lines
      ctx.strokeStyle = '#29292e';
      ctx.lineWidth = 1;
      for (let y = 0; y <= h; y += 27.5) {
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(w, y);
        ctx.stroke();
      }

      function drawLine(data, color, maxVal) {
        ctx.strokeStyle = color;
        ctx.lineWidth = 2;
        ctx.beginPath();
        for (let i = 0; i < data.length; i++) {
          const x = (i / (MAX_POINTS - 1)) * w;
          const y = h - (data[i] / maxVal) * h;
          if (i === 0) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        }
        ctx.stroke();
      }

      drawLine(heapHist, '#04d361', 100);  // 0~100%
      drawLine(psramHist, '#1ea2df', 100); // 0~100%
      drawLine(tempHist, '#fba94c', 100);  // 0~100 °C
    }

    // ── Stats Polling (Every 1 sec) ──
    function fetchStats() {
      fetch('/api/stats')
        .then(res => res.json())
        .then(data => {
          // Heap
          const hUsed = (data.heap_used / 1024).toFixed(0);
          const hTotal = (data.heap_total / 1024).toFixed(0);
          document.getElementById('heap-text').innerText = `${hUsed} / ${hTotal} KB`;
          document.getElementById('heap-pct').innerText = `${data.heap_pct}%`;
          document.getElementById('heap-bar').style.width = `${data.heap_pct}%`;

          // PSRAM
          const pUsed = (data.psram_used / (1024*1024)).toFixed(2);
          const pTotal = (data.psram_total / (1024*1024)).toFixed(1);
          document.getElementById('psram-text').innerText = `${pUsed} / ${pTotal} MB`;
          document.getElementById('psram-pct').innerText = `${data.psram_pct}%`;
          document.getElementById('psram-bar').style.width = `${data.psram_pct}%`;

          // Temp, RSSI, Uptime
          document.getElementById('temp-val').innerText = `${data.temp} °C`;
          document.getElementById('rssi-val').innerText = data.rssi ? `${data.rssi} dBm` : 'N/A';
          document.getElementById('uptime-val').innerText = `${data.uptime}s`;

          // Push to history
          heapHist.shift(); heapHist.push(data.heap_pct);
          psramHist.shift(); psramHist.push(data.psram_pct);
          tempHist.shift(); tempHist.push(data.temp);

          drawChart();
        })
        .catch(err => console.log(err));
    }

    setInterval(fetchStats, 1000);
    fetchStats();
  </script>
</body>
</html>
)rawliteral";

// ── WebServer 라우트 핸들러 ────────────────────────────────────
void handleRoot() {
    server.send(200, "text/html", INDEX_HTML);
}

void handleApiStats() {
    uint32_t totalHeap = ESP.getHeapSize();
    uint32_t freeHeap  = ESP.getFreeHeap();
    uint32_t usedHeap  = totalHeap - freeHeap;
    float heapPct = totalHeap ? ((float)usedHeap / totalHeap) * 100.0f : 0.0f;

    uint32_t totalPsram = ESP.getPsramSize();
    uint32_t freePsram  = ESP.getFreePsram();
    uint32_t usedPsram  = totalPsram - freePsram;
    float psramPct = totalPsram ? ((float)usedPsram / totalPsram) * 100.0f : 0.0f;

    float tempC = temperatureRead();
    int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    uint32_t uptimeSec = millis() / 1000;

    char json[256];
    snprintf(json, sizeof(json),
        "{\"heap_total\":%lu,\"heap_used\":%lu,\"heap_pct\":%.1f,"
        "\"psram_total\":%lu,\"psram_used\":%lu,\"psram_pct\":%.1f,"
        "\"temp\":%.1f,\"rssi\":%d,\"uptime\":%lu}",
        (unsigned long)totalHeap, (unsigned long)usedHeap, heapPct,
        (unsigned long)totalPsram, (unsigned long)usedPsram, psramPct,
        tempC, rssi, (unsigned long)uptimeSec);

    server.send(200, "application/json", json);
}

void handleApiLed() {
    if (server.hasArg("r") && server.hasArg("g") && server.hasArg("b")) {
        int r = server.arg("r").toInt();
        int g = server.arg("g").toInt();
        int b = server.arg("b").toInt();
        LED_SetColor(r, g, b);
        server.send(200, "text/plain", String("[LED] Set RGB: ") + r + "," + g + "," + b);
    } else {
        server.send(400, "text/plain", "Missing r, g, b");
    }
}

void handleApiMode() {
    if (server.hasArg("val")) {
        String m = server.arg("val");
        if (m == "rainbow") { LED_SetMode(0); server.send(200, "text/plain", "[LED] Mode: Rainbow"); }
        else if (m == "pulse") { LED_SetMode(1); server.send(200, "text/plain", "[LED] Mode: Pulse"); }
        else if (m == "off") { LED_SetMode(2); server.send(200, "text/plain", "[LED] Mode: OFF"); }
        else server.send(400, "text/plain", "Invalid mode");
    }
}

void handleApiBright() {
    if (server.hasArg("val")) {
        int val = server.arg("val").toInt();
        LED_SetBrightness(val);
        server.send(200, "text/plain", String("[LED] Brightness: ") + val);
    }
}

void handleApiCli() {
    if (server.hasArg("cmd")) {
        String cmd = server.arg("cmd");
        String output = CLI_ExecuteWebCommand(cmd.c_str());
        server.send(200, "text/plain", output);
    } else {
        server.send(400, "text/plain", "Missing cmd argument");
    }
}

// ─────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    unsigned long t = millis();
    while (!Serial && (millis() - t < 3000)) delay(10);
    delay(300);

    // FastLED 초기화
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(g_brightness);
    FastLED.clear();
    FastLED.show();

    // ── Wi-Fi SoftAP (자체 공유기 모드) 시작 ───────────────────
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("ESP32-S3-WIFI", "12345678");

    // ── 웹 서버 핸들러 등록 ─────────────────────────────────────
    server.on("/", handleRoot);
    server.on("/api/stats", handleApiStats);
    server.on("/api/led", handleApiLed);
    server.on("/api/mode", handleApiMode);
    server.on("/api/bright", handleApiBright);
    server.on("/api/cli", handleApiCli);
    server.begin();

    // CLI 초기화 (BLE + 시리얼 쉘)
    CLI_Init();

    Serial.println(F("\r\n======================================================="));
    Serial.println(F("[WIFI] Access Point Created!"));
    Serial.println(F("  SSID     : ESP32-S3-WIFI"));
    Serial.println(F("  Password : 12345678"));
    Serial.println(F("  Web URL  : http://192.168.4.1"));
    Serial.println(F("======================================================="));
    Serial.print(F(CLI_PROMPT));
}

// ─────────────────────────────────────────────────────────────
void loop()
{
    uint32_t now = millis();

    // ── 웹서버 요청 처리 ──────────────────────────────────────
    server.handleClient();

    // ── CLI 시리얼/BLE 처리 ───────────────────────────────────
    CLI_Process();

    // ── LED 업데이트 (20ms마다 → 50fps) ──────────────────────
    if (now - g_last_led_ms >= 20) {
        g_last_led_ms = now;

        switch (g_mode) {
            case 0: // Rainbow Cycle
                leds[0] = CHSV(g_hue++, 255, 255);
                break;
            case 1: // Color Pulse (파랑)
                g_pulse_val += g_pulse_dir * 3;
                if (g_pulse_val >= 252) { g_pulse_val = 252; g_pulse_dir = -1; }
                if (g_pulse_val <= 0)   { g_pulse_val = 0;   g_pulse_dir =  1; }
                leds[0] = CHSV(160, 255, g_pulse_val);
                break;
            case 2: // OFF
                leds[0] = CRGB::Black;
                break;
            case 3: // Manual color
                leds[0] = g_manual_rgb;
                break;
        }
        FastLED.show();
    }
}
