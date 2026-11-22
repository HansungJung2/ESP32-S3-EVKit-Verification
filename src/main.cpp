#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <esp_ota_ops.h>
#include "debug_cli.h"
#include "mmap_storage.h"
#include "fs_manager.h"

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
static File      g_uploadFile;

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
  <title>ESP32-S3 Verification & Dashboard</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #121214; color: #e1e1e6; text-align: center; margin: 0; padding: 12px; }
    .card { background: #202024; border-radius: 12px; padding: 16px; margin: 10px auto; max-width: 500px; box-shadow: 0 4px 12px rgba(0,0,0,0.4); text-align: left; }
    h1 { font-size: 19px; color: #04d361; margin: 0 0 4px 0; text-align: center; }
    .subtitle { font-size: 11px; color: #8d8d99; margin-bottom: 12px; text-align: center; }
    .section-title { font-size: 13px; font-weight: 600; color: #04d361; border-bottom: 1px solid #323238; padding-bottom: 4px; margin: 14px 0 8px 0; }
    
    /* Meter Bars */
    .metric { margin: 6px 0; font-size: 12px; }
    .metric-header { display: flex; justify-content: space-between; margin-bottom: 3px; }
    .bar-bg { background: #323238; height: 9px; border-radius: 5px; overflow: hidden; }
    .bar-fill { height: 100%; width: 0%; border-radius: 5px; transition: width 0.3s; }
    .fill-green { background: #04d361; }
    .fill-blue  { background: #1ea2df; }
    .fill-purple{ background: #a370f7; }
    .fill-orange{ background: #fba94c; }

    /* Chart Canvas */
    canvas { background: #18181b; border: 1px solid #29292e; border-radius: 8px; width: 100%; height: 95px; display: block; margin: 6px 0; }

    /* Buttons & Controls */
    .btn-group { display: flex; flex-wrap: wrap; gap: 6px; margin: 6px 0; }
    .btn { background: #323238; color: #fff; border: 1px solid #4a4a52; border-radius: 6px; padding: 5px 10px; font-size: 11px; cursor: pointer; transition: 0.2s; flex: 1; min-width: 70px; text-align: center; }
    .btn:hover { background: #04d361; color: #000; border-color: #04d361; }
    .btn-sm { padding: 3px 7px; font-size: 11px; min-width: auto; flex: none; }
    .btn-blue { border-color: #1ea2df; color: #1ea2df; }
    .btn-blue:hover { background: #1ea2df; color: #000; }
    .btn-purple { border-color: #a370f7; color: #a370f7; }
    .btn-purple:hover { background: #a370f7; color: #000; }
    .btn-red { border-color: #f75a68; color: #f75a68; }
    .btn-red:hover { background: #f75a68; color: #fff; }
    .btn-orange { border-color: #fba94c; color: #fba94c; }
    .btn-orange:hover { background: #fba94c; color: #000; }

    .ctrl-row { display: flex; align-items: center; justify-content: space-around; margin: 8px 0; }
    input[type=color] { border: none; width: 36px; height: 36px; border-radius: 50%; cursor: pointer; background: transparent; }
    input[type=range] { width: 100%; accent-color: #04d361; }
    input[type=text], input[type=file] { padding: 5px 8px; background: #121214; border: 1px solid #4a4a52; border-radius: 6px; color: #fff; box-sizing: border-box; font-size: 11px; }

    /* Tables */
    table { width: 100%; border-collapse: collapse; font-size: 11px; margin-top: 6px; }
    th, td { padding: 5px 6px; text-align: left; border-bottom: 1px solid #29292e; }
    th { color: #8d8d99; }
    tr:hover { background: #29292e; }

    /* Terminal & Dump */
    #cli-log { background: #000; color: #04d361; font-family: monospace; font-size: 11px; padding: 8px; border-radius: 6px; height: 80px; overflow-y: scroll; margin-top: 6px; white-space: pre-wrap; }
    .badge { background: #29292e; padding: 2px 6px; border-radius: 4px; font-size: 11px; }
    pre { background: #18181b; padding: 8px; border-radius: 6px; font-size: 11px; color: #e1e1e6; max-height: 120px; overflow-y: auto; white-space: pre-wrap; margin-top: 6px; border: 1px solid #323238; }
  </style>
</head>
<body>
  <div class="card">
    <h1>ESP32-S3 EV-Kit Dashboard</h1>
    <div class="subtitle">16MB Flash | 8MB PSRAM | 7MB LittleFS | Dual OTA</div>

    <!-- 1. Realtime Hardware Stats -->
    <div class="section-title">📊 System Memory & Storages</div>
    
    <div class="metric">
      <div class="metric-header">
        <span style="color:#a370f7; font-weight:bold;">💾 16MB Flash (App: <span id="flash-text">0 / 0 MB</span>)</span>
        <span id="flash-pct" class="badge" style="color:#a370f7;">0%</span>
      </div>
      <div class="bar-bg"><div id="flash-bar" class="bar-fill fill-purple"></div></div>
    </div>

    <div class="metric">
      <div class="metric-header">
        <span style="color:#fba94c; font-weight:bold;">📁 7MB LittleFS FileSystem (<span id="fs-text">0 / 0 MB</span>)</span>
        <span id="fs-pct" class="badge" style="color:#fba94c;">0%</span>
      </div>
      <div class="bar-bg"><div id="fs-bar" class="bar-fill fill-orange"></div></div>
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
    <div class="section-title">📈 Live 30s Resource Chart</div>
    <canvas id="liveChart" width="460" height="95"></canvas>
    <div style="font-size: 10px; display: flex; justify-content: space-around; color: #8d8d99;">
      <span style="color:#04d361">■ Heap (%)</span>
      <span style="color:#1ea2df; font-weight:bold;">■ 8MB PSRAM (%)</span>
      <span style="color:#fba94c">■ Chip Temp (°C)</span>
    </div>

    <!-- 3. LittleFS 7MB File Manager -->
    <div class="section-title">📁 7MB LittleFS 파일 관리자 (txt, json, csv)</div>
    <div style="display:flex; gap:6px; margin-top:4px;">
      <input type="file" id="fs-upload-input" style="flex:1;">
      <button class="btn btn-orange" onclick="uploadFsFile()" style="flex:0 0 75px;">파일 업로드</button>
    </div>
    
    <div class="btn-group" style="margin-top:6px;">
      <button class="btn" onclick="fetchFsFiles()">🔄 목록 새로고침</button>
      <button class="btn" onclick="createFsSamples()">📄 샘플파일 생성</button>
      <button class="btn btn-red" onclick="formatFs()">⚠️ 7MB 포맷</button>
    </div>

    <table>
      <thead>
        <tr><th>파일명</th><th style="width:70px;">크기</th><th style="width:130px; text-align:right;">액션</th></tr>
      </thead>
      <tbody id="fs-file-tbody">
        <tr><td colspan="3" style="text-align:center; color:#8d8d99;">로딩 중...</td></tr>
      </tbody>
    </table>

    <div id="fs-preview-box" style="display:none; margin-top:8px;">
      <div style="display:flex; justify-content:space-between; font-size:11px; color:#fba94c; font-weight:bold;">
        <span id="fs-preview-title">파일 내용 미리보기</span>
        <button class="btn btn-sm" onclick="document.getElementById('fs-preview-box').style.display='none'">닫기</button>
      </div>
      <pre id="fs-preview-content"></pre>
    </div>

    <!-- 4. OTA Firmware Update -->
    <div class="section-title">🚀 Dual OTA 무선 펌웨어 업데이트</div>
    <div style="font-size: 11px; margin-bottom: 4px;">
      실행 파티션: <span class="badge" id="ota-running" style="color:#04d361;">--</span> | 
      다음 타겟: <span class="badge" id="ota-next" style="color:#1ea2df;">--</span> |
      여유 공간: <span class="badge" id="ota-free">--</span>
    </div>
    <div style="display:flex; gap:6px; margin-top:4px;">
      <input type="file" id="ota-file-input" accept=".bin" style="flex:1;">
      <button class="btn btn-purple" onclick="uploadOtaFirmware()" style="flex:0 0 95px;">OTA 펌웨어 업로드</button>
    </div>
    <div id="ota-progress-container" style="display:none; margin-top:6px;">
      <div class="bar-bg"><div id="ota-progress-bar" class="bar-fill fill-purple" style="width:0%;"></div></div>
      <div id="ota-status-text" style="font-size:11px; color:#a370f7; margin-top:3px; text-align:center;">0% 전송 중...</div>
    </div>

    <!-- 5. 128KB Flash MMAP Storage -->
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

    <!-- 6. PSRAM Stress & Test Tool -->
    <div class="section-title">🧪 8MB PSRAM Stress & Test Tool</div>
    <div class="btn-group">
      <button class="btn btn-blue" onclick="testPsramAlloc(1)">+1MB 할당</button>
      <button class="btn btn-blue" onclick="testPsramAlloc(2)">+2MB 할당</button>
      <button class="btn btn-blue" onclick="testPsramAlloc(4)">+4MB 할당</button>
      <button class="btn btn-red" onclick="testPsramFree()">전체 해제</button>
      <button class="btn btn-orange" onclick="testPsramSpeed()">⚡ R/W 속도</button>
    </div>

    <!-- 7. RGB LED Controller -->
    <div class="section-title">🎨 GPIO48 RGB LED Control</div>
    <div class="ctrl-row">
      <input type="color" id="picker" value="#ff0000" onchange="sendColor(this.value)">
      <button class="btn" onclick="sendMode('rainbow')">🌈 Rainbow</button>
      <button class="btn" onclick="sendMode('pulse')">💙 Pulse</button>
      <button class="btn btn-red" onclick="sendMode('off')">⬛ OFF</button>
    </div>
    <div style="font-size: 11px; margin-bottom: 2px;">Brightness: <span id="bright-val">80</span></div>
    <input type="range" min="0" max="255" value="80" oninput="document.getElementById('bright-val').innerText=this.value; sendBright(this.value)">

    <!-- 8. Web CLI -->
    <div class="section-title">💻 Web CLI Terminal</div>
    <div style="display:flex; justify-content:space-between;">
      <input type="text" id="cli-input" placeholder="fs, ota, mmap, psram, top, sysinfo..." onkeydown="if(event.key==='Enter') sendCLI()" style="width:calc(100% - 70px);">
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

    // ── LittleFS File Actions ──
    function fetchFsFiles() {
      fetch('/api/fs/list')
        .then(r => r.json())
        .then(data => {
          const totalMB = (data.total / (1024*1024)).toFixed(1);
          const usedKB = (data.used / 1024).toFixed(1);
          const pct = data.total ? ((data.used / data.total) * 100).toFixed(1) : 0;
          document.getElementById('fs-text').innerText = `${usedKB} KB / ${totalMB} MB`;
          document.getElementById('fs-pct').innerText = `${pct}%`;
          document.getElementById('fs-bar').style.width = `${Math.min(pct, 100)}%`;

          const tbody = document.getElementById('fs-file-tbody');
          if (!data.files || data.files.length === 0) {
            tbody.innerHTML = '<tr><td colspan="3" style="text-align:center; color:#8d8d99;">파일이 없습니다.</td></tr>';
            return;
          }
          let html = '';
          data.files.forEach(f => {
            const sizeStr = f.size > 1024 ? (f.size / 1024).toFixed(1) + ' KB' : f.size + ' B';
            html += `<tr>
              <td><b>${f.name}</b></td>
              <td>${sizeStr}</td>
              <td style="text-align:right;">
                <button class="btn btn-sm btn-blue" onclick="viewFsFile('${f.name}')">보기</button>
                <a href="/api/fs/download?file=/${f.name}" download="${f.name}"><button class="btn btn-sm btn-orange">다운로드</button></a>
                <button class="btn btn-sm btn-red" onclick="deleteFsFile('${f.name}')">삭제</button>
              </td>
            </tr>`;
          });
          tbody.innerHTML = html;
        });
    }

    function viewFsFile(fname) {
      log(`[LittleFS] Reading /${fname}...`);
      fetch(`/api/fs/view?file=/${fname}`)
        .then(r => r.text())
        .then(txt => {
          document.getElementById('fs-preview-title').innerText = `📄 미리보기: /${fname}`;
          document.getElementById('fs-preview-content').innerText = txt;
          document.getElementById('fs-preview-box').style.display = 'block';
        });
    }

    function deleteFsFile(fname) {
      if (!confirm(`'/${fname}' 파일을 정말 삭제하시겠습니까?`)) return;
      fetch(`/api/fs/delete?file=/${fname}`, { method: 'POST' })
        .then(r => r.text())
        .then(t => { log(t); fetchFsFiles(); });
    }

    function uploadFsFile() {
      const input = document.getElementById('fs-upload-input');
      if (!input.files || input.files.length === 0) {
        alert('업로드할 파일을 선택하세요.');
        return;
      }
      const file = input.files[0];
      const formData = new FormData();
      formData.append('file', file, file.name);

      log(`[LittleFS] Uploading '${file.name}' (${file.size} bytes)...`);
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/fs/upload', true);
      xhr.onload = function() {
        log(`[LittleFS] Upload complete!`);
        input.value = '';
        fetchFsFiles();
      };
      xhr.onerror = function() { log('[LittleFS] Upload error!'); };
      xhr.send(formData);
    }

    function createFsSamples() {
      log('[LittleFS] Generating default sample files...');
      fetch('/api/fs/sample', { method: 'POST' })
        .then(r => r.text())
        .then(t => { log(t); fetchFsFiles(); });
    }

    function formatFs() {
      if (!confirm('경고: 7MB LittleFS 파일 시스템을 전체 포맷하시겠습니까?\n모든 파일이 영구 삭제됩니다.')) return;
      log('[LittleFS] Formatting filesystem...');
      fetch('/api/fs/format', { method: 'POST' })
        .then(r => r.text())
        .then(t => { log(t); fetchFsFiles(); });
    }

    // ── OTA Firmware Actions ──
    function fetchOtaInfo() {
      fetch('/api/ota/info')
        .then(r => r.json())
        .then(data => {
          document.getElementById('ota-running').innerText = data.running;
          document.getElementById('ota-next').innerText = data.next;
          document.getElementById('ota-free').innerText = (data.free_space / (1024*1024)).toFixed(1) + ' MB';
        });
    }

    function uploadOtaFirmware() {
      const input = document.getElementById('ota-file-input');
      if (!input.files || input.files.length === 0) {
        alert('OTA 업데이트용 firmware.bin 파일을 선택하세요.');
        return;
      }
      const file = input.files[0];
      if (!file.name.endsWith('.bin')) {
        alert('확장자가 .bin인 펌웨어 파일만 업로드할 수 있습니다.');
        return;
      }
      if (!confirm(`'${file.name}' (${(file.size/(1024*1024)).toFixed(2)} MB) 파일로 OTA 무선 펌웨어 업데이트를 진행하시겠습니까?`)) return;

      const pContainer = document.getElementById('ota-progress-container');
      const pBar = document.getElementById('ota-progress-bar');
      const pStatus = document.getElementById('ota-status-text');
      pContainer.style.display = 'block';

      const formData = new FormData();
      formData.append('update', file, file.name);

      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/ota/upload', true);

      xhr.upload.onprogress = function(e) {
        if (e.lengthComputable) {
          const pct = Math.round((e.loaded / e.total) * 100);
          pBar.style.width = pct + '%';
          pStatus.innerText = `${pct}% 전송 중 (${(e.loaded/1024).toFixed(0)} / ${(e.total/1024).toFixed(0)} KB)...`;
        }
      };

      xhr.onload = function() {
        if (xhr.status === 200 && xhr.responseText.includes('OTA_SUCCESS')) {
          pBar.style.width = '100%';
          pStatus.innerText = '✅ OTA 업데이트 성공! ESP32-S3가 재부팅됩니다. 잠시 후 새로고침하세요.';
          log('[OTA] Update successful! Rebooting...');
        } else {
          pStatus.innerText = '❌ OTA 업데이트 실패: ' + xhr.responseText;
          log('[OTA] Update failed!');
        }
      };

      xhr.onerror = function() {
        pStatus.innerText = '❌ 네트워크 전송 오류 발생!';
        log('[OTA] Network error during update');
      };

      xhr.send(formData);
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
    function sendMode(m) {
      fetch(`/api/mode?val=${m}`).then(r => r.text()).then(log);
    }
    function sendBright(b) {
      fetch(`/api/bright?val=${b}`).then(r => r.text()).then(log);
    }
    function sendCLI() {
      const inp = document.getElementById('cli-input');
      const cmd = inp.value.trim();
      if (!cmd) return;
      log(`ESP32> ${cmd}`);
      fetch(`/api/cli?cmd=${encodeURIComponent(cmd)}`)
        .then(r => r.text())
        .then(txt => { log(txt); inp.value = ''; });
    }

    // ── Chart & Stats Polling ──
    function drawChart() {
      const canvas = document.getElementById('liveChart');
      const ctx = canvas.getContext('2d');
      const W = canvas.width, H = canvas.height;
      ctx.clearRect(0, 0, W, H);

      ctx.strokeStyle = '#29292e';
      ctx.lineWidth = 1;
      for (let y = 0; y <= H; y += H / 4) {
        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
      }

      function plot(data, color, maxVal) {
        ctx.strokeStyle = color;
        ctx.lineWidth = 2;
        ctx.beginPath();
        for (let i = 0; i < data.length; i++) {
          const x = (i / (MAX_POINTS - 1)) * W;
          const y = H - ((data[i] / maxVal) * H);
          if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        }
        ctx.stroke();
      }

      plot(heapHist, '#04d361', 100);
      plot(psramHist, '#1ea2df', 100);
      plot(tempHist, '#fba94c', 80);
    }

    function fetchStats() {
      fetch('/api/stats')
        .then(r => r.json())
        .then(data => {
          const totalMB = (data.flash_total / (1024*1024)).toFixed(0);
          const appMB   = (data.flash_app / (1024*1024)).toFixed(2);
          document.getElementById('flash-text').innerText = `${appMB} / ${totalMB} MB`;
          document.getElementById('flash-pct').innerText = `${data.flash_pct.toFixed(1)}%`;
          document.getElementById('flash-bar').style.width = `${Math.min(data.flash_pct, 100)}%`;

          const psramTotalMB = (data.psram_total / (1024*1024)).toFixed(1);
          const psramUsedMB  = (data.psram_used  / (1024*1024)).toFixed(2);
          document.getElementById('psram-text').innerText = `${psramUsedMB} / ${psramTotalMB} MB`;
          document.getElementById('psram-pct').innerText = `${data.psram_pct.toFixed(1)}%`;
          document.getElementById('psram-bar').style.width = `${Math.min(data.psram_pct, 100)}%`;

          document.getElementById('heap-text').innerText = `${(data.heap_used/1024).toFixed(0)} / ${(data.heap_total/1024).toFixed(0)} KB`;
          document.getElementById('heap-pct').innerText = `${data.heap_pct.toFixed(1)}%`;
          document.getElementById('heap-bar').style.width = `${Math.min(data.heap_pct, 100)}%`;

          document.getElementById('temp-val').innerText = `${data.temp.toFixed(1)} °C`;
          document.getElementById('rssi-val').innerText = `${data.rssi} dBm`;
          document.getElementById('uptime-val').innerText = `${data.uptime}s`;

          heapHist.shift(); heapHist.push(data.heap_pct);
          psramHist.shift(); psramHist.push(data.psram_pct);
          tempHist.shift(); tempHist.push(data.temp);
          drawChart();
        })
        .catch(() => {});
    }

    setInterval(fetchStats, 1000);
    fetchStats();
    fetchMmapInfo();
    fetchFsFiles();
    fetchOtaInfo();
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

    char json[384];
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

// ── LittleFS Web Handlers ─────────────────────────────────────
void handleApiFsList() {
    size_t total = FS_GetTotalBytes();
    size_t used  = FS_GetUsedBytes();
    String fileList = FS_GetFileListJSON();
    char header[128];
    snprintf(header, sizeof(header), "{\"total\":%lu,\"used\":%lu,\"files\":", (unsigned long)total, (unsigned long)used);
    String json = String(header) + fileList + "}";
    server.send(200, "application/json", json);
}

void handleApiFsView() {
    if (!server.hasArg("file")) {
        server.send(400, "text/plain", "Missing file param");
        return;
    }
    String path = server.arg("file");
    if (!path.startsWith("/")) path = "/" + path;
    if (!LittleFS.exists(path)) {
        server.send(404, "text/plain", "File not found");
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        server.send(500, "text/plain", "Failed to open file");
        return;
    }
    String mime = "text/plain";
    if (path.endsWith(".json")) mime = "application/json";
    else if (path.endsWith(".csv")) mime = "text/csv";
    else if (path.endsWith(".html")) mime = "text/html";
    server.streamFile(f, mime);
    f.close();
}

void handleApiFsDownload() {
    if (!server.hasArg("file")) {
        server.send(400, "text/plain", "Missing file param");
        return;
    }
    String path = server.arg("file");
    if (!path.startsWith("/")) path = "/" + path;
    if (!LittleFS.exists(path)) {
        server.send(404, "text/plain", "File not found");
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        server.send(500, "text/plain", "Failed to open file");
        return;
    }
    String fname = path;
    int idx = fname.lastIndexOf('/');
    if (idx >= 0) fname = fname.substring(idx + 1);
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + fname + "\"");
    server.streamFile(f, "application/octet-stream");
    f.close();
}

void handleApiFsUpload() {
    server.send(200, "text/plain", "[LittleFS] Upload finished successfully!");
}

void handleApiFsUploadProcess() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        String filename = upload.filename;
        if (!filename.startsWith("/")) filename = "/" + filename;
        Serial.printf("[LittleFS] Upload Start: %s\r\n", filename.c_str());
        g_uploadFile = LittleFS.open(filename, "w");
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (g_uploadFile) {
            g_uploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (g_uploadFile) {
            g_uploadFile.close();
            Serial.printf("[LittleFS] Upload Complete: %u bytes\r\n", upload.totalSize);
        }
    }
}

void handleApiFsDelete() {
    if (!server.hasArg("file")) {
        server.send(400, "text/plain", "Missing file param");
        return;
    }
    String path = server.arg("file");
    if (!path.startsWith("/")) path = "/" + path;
    if (FS_DeleteFile(path.c_str())) {
        server.send(200, "text/plain", "[LittleFS] File deleted: " + path);
    } else {
        server.send(500, "text/plain", "[LittleFS] Delete failed for: " + path);
    }
}

void handleApiFsSample() {
    FS_CreateSampleFiles();
    server.send(200, "text/plain", "[LittleFS] Sample files created: config.json, sensor_log.csv, readme.txt");
}

void handleApiFsFormat() {
    if (FS_Format()) {
        server.send(200, "text/plain", "[LittleFS] 7MB Partition formatted successfully!");
    } else {
        server.send(500, "text/plain", "[LittleFS] Format failed!");
    }
}

// ── OTA Web Handlers ──────────────────────────────────────────
void handleApiOtaInfo() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(NULL);
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"running\":\"%s\",\"next\":\"%s\",\"sketch_size\":%lu,\"free_space\":%lu}",
        running ? running->label : "unknown",
        next ? next->label : "unknown",
        (unsigned long)ESP.getSketchSize(),
        (unsigned long)ESP.getFreeSketchSpace());
    server.send(200, "application/json", buf);
}

void handleApiOtaUpload() {
    bool ok = !Update.hasError();
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", ok ? "OTA_SUCCESS" : "OTA_FAIL");
    if (ok) {
        Serial.println(F("[OTA] Success! Restarting in 1 second..."));
        delay(1000);
        ESP.restart();
    }
}

void handleApiOtaUploadProcess() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Firmware Upload Start: %s\r\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("[OTA] Update Finished: %u bytes successfully written!\r\n", upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

// ── MMAP Storage Handlers ─────────────────────────────────────
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

    // ── 7MB LittleFS 파일 시스템 초기화 ─────────────────────────
    FS_Init();

    // ── Wi-Fi SoftAP (자체 공유기 모드) 시작 ───────────────────
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("ESP32-S3-WIFI", "12345678");

    // ── Arduino OTA 초기화 (Wi-Fi 무선 플래싱) ──────────────────
    ArduinoOTA.setHostname("ESP32-S3-EVKit");
    ArduinoOTA.begin();

    // ── 웹 서버 핸들러 등록 ─────────────────────────────────────
    server.on("/", handleRoot);
    server.on("/api/stats", handleApiStats);

    // LittleFS Routes
    server.on("/api/fs/list", HTTP_GET, handleApiFsList);
    server.on("/api/fs/view", HTTP_GET, handleApiFsView);
    server.on("/api/fs/download", HTTP_GET, handleApiFsDownload);
    server.on("/api/fs/upload", HTTP_POST, handleApiFsUpload, handleApiFsUploadProcess);
    server.on("/api/fs/delete", HTTP_POST, handleApiFsDelete);
    server.on("/api/fs/sample", HTTP_POST, handleApiFsSample);
    server.on("/api/fs/format", HTTP_POST, handleApiFsFormat);

    // OTA Routes
    server.on("/api/ota/info", HTTP_GET, handleApiOtaInfo);
    server.on("/api/ota/upload", HTTP_POST, handleApiOtaUpload, handleApiOtaUploadProcess);

    // MMAP Routes
    server.on("/api/mmap/get", handleApiMmapGet);
    server.on("/api/mmap/set", handleApiMmapSet);
    server.on("/api/mmap/dump", handleApiMmapDump);

    // PSRAM Routes
    server.on("/api/psram/alloc", handleApiPsramAlloc);
    server.on("/api/psram/free", handleApiPsramFree);
    server.on("/api/psram/speed", handleApiPsramSpeed);

    // Hardware Controls
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
    Serial.printf( "  LittleFS : %lu MB Mounted (7MB Partition)\r\n", (unsigned long)(FS_GetTotalBytes() / (1024*1024)));
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

    // ── Arduino OTA 처리 ──────────────────────────────────────
    ArduinoOTA.handle();

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
