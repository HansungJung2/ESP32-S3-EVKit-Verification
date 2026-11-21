#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include "debug_cli.h"
#include "mmap_storage.h"

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

// ── HTML 웹 대시보드 페이지 ─────────────────────────────────────
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32-S3 Resource & MMAP Storage</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #121214; color: #e1e1e6; text-align: center; margin: 0; padding: 12px; }
    .card { background: #202024; border-radius: 12px; padding: 16px; margin: 10px auto; max-width: 480px; box-shadow: 0 4px 12px rgba(0,0,0,0.4); text-align: left; }
    h1 { font-size: 19px; color: #04d361; margin: 0 0 4px 0; text-align: center; }
    .subtitle { font-size: 12px; color: #8d8d99; margin-bottom: 12px; text-align: center; }
    .section-title { font-size: 13px; font-weight: 600; color: #04d361; border-bottom: 1px solid #323238; padding-bottom: 4px; margin: 12px 0 8px 0; }
    
    /* Meter Bars */
    .metric { margin: 6px 0; font-size: 12px; }
    .metric-header { display: flex; justify-content: space-between; margin-bottom: 3px; }
    .bar-bg { background: #323238; height: 10px; border-radius: 5px; overflow: hidden; }
    .bar-fill { height: 100%; width: 0%; border-radius: 5px; transition: width 0.3s; }
    .fill-green { background: #04d361; }
    .fill-blue  { background: #1ea2df; }
    .fill-purple{ background: #a370f7; }

    /* Chart Canvas */
    canvas { background: #18181b; border: 1px solid #29292e; border-radius: 8px; width: 100%; height: 100px; display: block; margin: 6px 0; }

    /* Buttons & Controls */
    .btn-group { display: flex; flex-wrap: wrap; gap: 6px; margin: 6px 0; }
    .btn { background: #323238; color: #fff; border: 1px solid #4a4a52; border-radius: 6px; padding: 6px 11px; font-size: 12px; cursor: pointer; transition: 0.2s; flex: 1; min-width: 75px; text-align: center; }
    .btn:hover { background: #04d361; color: #000; border-color: #04d361; }
    .btn-blue { border-color: #1ea2df; color: #1ea2df; }
    .btn-blue:hover { background: #1ea2df; color: #000; }
    .btn-purple { border-color: #a370f7; color: #a370f7; }
    .btn-purple:hover { background: #a370f7; color: #000; }
    .btn-red { border-color: #f75a68; color: #f75a68; }
    .btn-red:hover { background: #f75a68; color: #fff; }
    .btn-orange { border-color: #fba94c; color: #fba94c; }
    .btn-orange:hover { background: #fba94c; color: #000; }

    .ctrl-row { display: flex; align-items: center; justify-content: space-around; margin: 8px 0; }
    input[type=color] { border: none; width: 40px; height: 40px; border-radius: 50%; cursor: pointer; background: transparent; }
    input[type=range] { width: 100%; accent-color: #04d361; }
    input[type=text] { padding: 6px 8px; background: #121214; border: 1px solid #4a4a52; border-radius: 6px; color: #fff; box-sizing: border-box; }

    /* Terminal & Dump */
    #cli-log { background: #000; color: #04d361; font-family: monospace; font-size: 11px; padding: 8px; border-radius: 6px; height: 85px; overflow-y: scroll; margin-top: 6px; white-space: pre-wrap; }
    .badge { background: #29292e; padding: 2px 6px; border-radius: 4px; font-size: 11px; }
  </style>
</head>
<body>
  <div class="card">
    <h1>ESP32-S3 Resource & MMAP</h1>
    <div class="subtitle">16MB Flash | 8MB Octal PSRAM | 128KB MMAP Storage</div>

    <!-- 1. Realtime Hardware Stats -->
    <div class="section-title">📊 Memory & Flash Storage</div>
    
    <div class="metric">
      <div class="metric-header">
        <span style="color:#a370f7; font-weight:bold;">💾 16MB SPI Flash (App: <span id="flash-text">0 / 0 MB</span>)</span>
        <span id="flash-pct" class="badge" style="color:#a370f7;">0%</span>
      </div>
      <div class="bar-bg"><div id="flash-bar" class="bar-fill fill-purple"></div></div>
    </div>

    <div class="metric">
      <div class="metric-header">
        <span style="color:#1ea2df; font-weight:bold;">⚡ 8MB Octal PSRAM (<span id="psram-text">0 / 0 MB</span>)</span>
        <span id="psram-pct" class="badge" style="color:#1ea2df;">0%</span>
      </div>
      <div class="bar-bg"><div id="psram-bar" class="bar-fill fill-blue"></div></div>
    </div>

    <div class="metric">
      <div class="metric-header">
        <span>📦 Internal SRAM Heap (<span id="heap-text">0 / 0 KB</span>)</span>
        <span id="heap-pct" class="badge">0%</span>
      </div>
      <div class="bar-bg"><div id="heap-bar" class="bar-fill fill-green"></div></div>
    </div>

    <div style="display: flex; justify-content: space-between; font-size: 11px; margin-top: 6px;">
      <div>🌡️ Temp: <span id="temp-val" class="badge">--.- °C</span></div>
      <div>📶 RSSI: <span id="rssi-val" class="badge">-- dBm</span></div>
      <div>⏱️ Uptime: <span id="uptime-val" class="badge">0s</span></div>
    </div>

    <!-- 2. Dynamic Realtime Chart -->
    <div class="section-title">📈 Memory & Temp History (Live 30s)</div>
    <canvas id="liveChart" width="440" height="100"></canvas>
    <div style="font-size: 10px; display: flex; justify-content: space-around; color: #8d8d99;">
      <span style="color:#04d361">■ Heap (%)</span>
      <span style="color:#1ea2df; font-weight:bold;">■ 8MB PSRAM (%)</span>
      <span style="color:#fba94c">■ Chip Temp (°C)</span>
    </div>

    <!-- 3. 128KB Flash MMAP Storage -->
    <div class="section-title">💾 128KB Flash MMAP NV-Storage</div>
    <div style="font-size: 11px; margin-bottom: 4px;">
      Physical: <span class="badge" id="mmap-phys">0x00F10000</span> | 
      MMU Ptr: <span class="badge" id="mmap-virt">0x3C000000</span> | 
      Boots: <span class="badge" id="mmap-boots" style="color:#04d361;">0</span>
    </div>
    <div style="display:flex; gap:6px; margin-top:4px;">
      <input type="text" id="mmap-input" placeholder="비휘발성 메모 입력 (전원 꺼져도 유지)" style="flex:1;">
      <button class="btn btn-purple" onclick="saveMmapMemo()" style="flex:0 0 85px;">Flash 저장</button>
    </div>
    <div style="font-size:11px; color:#a8a8b3; margin-top:4px;">
      현재 플래시 저장값: <b id="mmap-current-memo" style="color:#fff;">--</b>
    </div>
    <div class="btn-group" style="margin-top:6px;">
      <button class="btn" onclick="fetchMmapDump(64)">🔍 64B Hex Dump</button>
      <button class="btn" onclick="fetchMmapDump(128)">🔍 128B Hex Dump</button>
    </div>

    <!-- 4. PSRAM Stress & Test Tool -->
    <div class="section-title">🧪 8MB PSRAM Stress & Test Tool</div>
    <div class="btn-group">
      <button class="btn btn-blue" onclick="testPsramAlloc(1)">+1MB 할당</button>
      <button class="btn btn-blue" onclick="testPsramAlloc(2)">+2MB 할당</button>
      <button class="btn btn-blue" onclick="testPsramAlloc(4)">+4MB 할당</button>
      <button class="btn btn-red" onclick="testPsramFree()">전체 해제</button>
      <button class="btn btn-orange" onclick="testPsramSpeed()">⚡ R/W 속도</button>
    </div>

    <!-- 5. RGB LED Controller -->
    <div class="section-title">🎨 GPIO48 RGB LED Control</div>
    <div class="ctrl-row">
      <input type="color" id="picker" value="#ff0000" onchange="sendColor(this.value)">
      <button class="btn" onclick="sendMode('rainbow')">🌈 Rainbow</button>
      <button class="btn" onclick="sendMode('pulse')">💙 Pulse</button>
      <button class="btn btn-red" onclick="sendMode('off')">⬛ OFF</button>
    </div>
    <div style="font-size: 11px; margin-bottom: 2px;">Brightness: <span id="bright-val">80</span></div>
    <input type="range" min="0" max="255" value="80" oninput="document.getElementById('bright-val').innerText=this.value; sendBright(this.value)">

    <!-- 6. Web CLI -->
    <div class="section-title">💻 Web CLI Terminal</div>
    <div style="display:flex; justify-content:space-between;">
      <input type="text" id="cli-input" placeholder="mmap, psram, top, sysinfo..." onkeydown="if(event.key==='Enter') sendCLI()" style="width:calc(100% - 70px);">
      <button class="btn" onclick="sendCLI()" style="width:60px; flex:none;">Send</button>
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

    // ── MMAP Storage Actions ──
    function saveMmapMemo() {
      const memo = document.getElementById('mmap-input').value.trim();
      if (!memo) return;
      log(`[MMAP] Writing to 128KB Flash: "${memo}"...`);
      fetch(`/api/mmap/set?memo=${encodeURIComponent(memo)}`)
        .then(r => r.text())
        .then(t => {
          log(t);
          fetchMmapInfo();
          document.getElementById('mmap-input').value = '';
        });
    }

    function fetchMmapInfo() {
      fetch('/api/mmap/get')
        .then(r => r.json())
        .then(data => {
          document.getElementById('mmap-phys').innerText = '0x' + data.phys.toString(16).toUpperCase();
          document.getElementById('mmap-virt').innerText = '0x' + data.virt.toString(16).toUpperCase();
          document.getElementById('mmap-boots').innerText = data.boots;
          document.getElementById('mmap-current-memo').innerText = data.memo;
        });
    }

    function fetchMmapDump(len) {
      log(`[MMAP] Reading ${len} bytes via MMU Pointer...`);
      fetch(`/api/mmap/dump?len=${len}`)
        .then(r => r.text())
        .then(log);
    }

    // ── PSRAM Test Actions ──
    function testPsramAlloc(mb) {
      log(`[PSRAM] Allocating ${mb}MB in Octal PSRAM...`);
      fetch(`/api/psram/alloc?mb=${mb}`).then(r => r.text()).then(t => { log(t); fetchStats(); });
    }
    function testPsramFree() {
      log(`[PSRAM] Releasing all allocated test buffers...`);
      fetch(`/api/psram/free`).then(r => r.text()).then(t => { log(t); fetchStats(); });
    }
    function testPsramSpeed() {
      log(`[PSRAM] Running 1MB Read/Write Benchmark...`);
      fetch(`/api/psram/speed`).then(r => r.text()).then(log);
    }

    // ── LED & CLI Actions ──
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

      ctx.strokeStyle = '#29292e';
      ctx.lineWidth = 1;
      for (let y = 0; y <= h; y += 25) {
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(w, y);
        ctx.stroke();
      }

      function drawLine(data, color, maxVal, lineWidth) {
        ctx.strokeStyle = color;
        ctx.lineWidth = lineWidth;
        ctx.beginPath();
        for (let i = 0; i < data.length; i++) {
          const x = (i / (MAX_POINTS - 1)) * w;
          const y = h - (data[i] / maxVal) * h;
          if (i === 0) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        }
        ctx.stroke();
      }

      drawLine(heapHist, '#04d361', 100, 1.5);
      drawLine(psramHist, '#1ea2df', 100, 2.5);
      drawLine(tempHist, '#fba94c', 100, 1.5);
    }

    // ── Stats Polling (Every 1 sec) ──
    function fetchStats() {
      fetch('/api/stats')
        .then(res => res.json())
        .then(data => {
          // Flash
          const fApp = (data.flash_app / (1024*1024)).toFixed(2);
          const fTotal = (data.flash_total / (1024*1024)).toFixed(0);
          document.getElementById('flash-text').innerText = `${fApp} / ${fTotal} MB`;
          document.getElementById('flash-pct').innerText = `${data.flash_pct}%`;
          document.getElementById('flash-bar').style.width = `${data.flash_pct}%`;

          // PSRAM
          const pUsed = (data.psram_used / (1024*1024)).toFixed(2);
          const pTotal = (data.psram_total / (1024*1024)).toFixed(1);
          document.getElementById('psram-text').innerText = `${pUsed} / ${pTotal} MB`;
          document.getElementById('psram-pct').innerText = `${data.psram_pct}%`;
          document.getElementById('psram-bar').style.width = `${data.psram_pct}%`;

          // Heap
          const hUsed = (data.heap_used / 1024).toFixed(0);
          const hTotal = (data.heap_total / 1024).toFixed(0);
          document.getElementById('heap-text').innerText = `${hUsed} / ${hTotal} KB`;
          document.getElementById('heap-pct').innerText = `${data.heap_pct}%`;
          document.getElementById('heap-bar').style.width = `${data.heap_pct}%`;

          // Temp, RSSI, Uptime
          document.getElementById('temp-val').innerText = `${data.temp} °C`;
          document.getElementById('rssi-val').innerText = data.rssi ? `${data.rssi} dBm` : 'N/A';
          document.getElementById('uptime-val').innerText = `${data.uptime}s`;

          heapHist.shift(); heapHist.push(data.heap_pct);
          psramHist.shift(); psramHist.push(data.psram_pct);
          tempHist.shift(); tempHist.push(data.temp);

          drawChart();
        })
        .catch(err => console.log(err));
    }

    setInterval(fetchStats, 1000);
    fetchStats();
    fetchMmapInfo();
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

    uint32_t totalFlash = ESP.getFlashChipSize();
    uint32_t appFlash   = ESP.getSketchSize();
    float flashPct = totalFlash ? ((float)appFlash / totalFlash) * 100.0f : 0.0f;

    float tempC = temperatureRead();
    int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    uint32_t uptimeSec = millis() / 1000;

    char json[320];
    snprintf(json, sizeof(json),
        "{\"heap_total\":%lu,\"heap_used\":%lu,\"heap_pct\":%.1f,"
        "\"psram_total\":%lu,\"psram_used\":%lu,\"psram_pct\":%.1f,"
        "\"flash_total\":%lu,\"flash_app\":%lu,\"flash_pct\":%.1f,"
        "\"temp\":%.1f,\"rssi\":%d,\"uptime\":%lu}",
        (unsigned long)totalHeap, (unsigned long)usedHeap, heapPct,
        (unsigned long)totalPsram, (unsigned long)usedPsram, psramPct,
        (unsigned long)totalFlash, (unsigned long)appFlash, flashPct,
        tempC, rssi, (unsigned long)uptimeSec);

    server.send(200, "application/json", json);
}

void handleApiMmapGet() {
    const mmap_nv_header_t *hdr = MMAP_GetHeader();
    if (!hdr) {
        server.send(500, "application/json", "{\"error\":\"MMAP not initialized\"}");
        return;
    }
    char json[512];
    snprintf(json, sizeof(json),
        "{\"phys\":%lu,\"virt\":%lu,\"boots\":%lu,\"writes\":%lu,\"tag\":\"%s\",\"memo\":\"%s\"}",
        (unsigned long)MMAP_GetPhysicalAddress(),
        (unsigned long)MMAP_GetVirtualAddress(),
        (unsigned long)hdr->boot_count,
        (unsigned long)hdr->write_count,
        hdr->device_tag,
        hdr->user_memo);
    server.send(200, "application/json", json);
}

void handleApiMmapSet() {
    if (server.hasArg("memo")) {
        String memo = server.arg("memo");
        if (MMAP_SaveMemo(memo.c_str())) {
            server.send(200, "text/plain", "[MMAP] Successfully saved to 128KB Flash (Persisted across reboot)!");
        } else {
            server.send(500, "text/plain", "[MMAP] Error writing to Flash");
        }
    } else {
        server.send(400, "text/plain", "Missing memo arg");
    }
}

void handleApiMmapDump() {
    size_t len = server.hasArg("len") ? server.arg("len").toInt() : 64;
    char buf[1024];
    MMAP_HexDump(0, len, buf, sizeof(buf));
    server.send(200, "text/plain", buf);
}

void handleApiPsramAlloc() {
    int mb = server.hasArg("mb") ? server.arg("mb").toInt() : 1;
    if (mb <= 0 || mb > 6) {
        server.send(400, "text/plain", "Invalid MB count (1-6)");
        return;
    }
    if (PSRAM_AllocMB(mb)) {
        size_t totalAlloc = PSRAM_GetAllocatedBytes();
        server.send(200, "text/plain", String("[PSRAM] +") + mb + "MB allocated successfully! Total Test: " + (totalAlloc / (1024*1024)) + "MB");
    } else {
        server.send(500, "text/plain", "[PSRAM] Allocation failed! Out of PSRAM or limit reached.");
    }
}

void handleApiPsramFree() {
    PSRAM_FreeAll();
    server.send(200, "text/plain", "[PSRAM] All test buffers successfully released!");
}

void handleApiPsramSpeed() {
    String output = CLI_ExecuteWebCommand("psram speed");
    server.send(200, "text/plain", output);
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

    // ── 128KB Flash MMAP 초기화 ────────────────────────────────
    MMAP_Init();

    // ── Wi-Fi SoftAP (자체 공유기 모드) 시작 ───────────────────
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("ESP32-S3-WIFI", "12345678");

    // ── 웹 서버 핸들러 등록 ─────────────────────────────────────
    server.on("/", handleRoot);
    server.on("/api/stats", handleApiStats);
    server.on("/api/mmap/get", handleApiMmapGet);
    server.on("/api/mmap/set", handleApiMmapSet);
    server.on("/api/mmap/dump", handleApiMmapDump);
    server.on("/api/psram/alloc", handleApiPsramAlloc);
    server.on("/api/psram/free", handleApiPsramFree);
    server.on("/api/psram/speed", handleApiPsramSpeed);
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
    Serial.printf( "  Flash    : %lu MB (128KB MMAP Storage Active)\r\n", (unsigned long)(ESP.getFlashChipSize() / (1024*1024)));
    Serial.printf( "  PSRAM    : %lu bytes (%.2f MB OPI)\r\n", (unsigned long)ESP.getPsramSize(), (float)ESP.getPsramSize() / (1024*1024));
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
