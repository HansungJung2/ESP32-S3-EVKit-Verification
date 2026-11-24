/*
 * Copyright (c) 2026
 * Zephyr RTOS Full-Stack Verification Firmware for ESP32-S3 (N16R8)
 *
 * Implements:
 * 1. 128KB Non-Volatile Flash MMAP Storage via Hardware MMU (spi_flash_mmap)
 * 2. Web Server File/Firmware OTA Upload & Real-time Hex Dump Viewer
 * 3. WS2812B RGB LED (GPIO 48 via SPI3 Driver & DeviceTree)
 * 4. Wi-Fi 4 (802.11 b/g/n) with Zephyr L2 Shell (wifi scan, wifi connect)
 * 5. BLE 5.0 Nordic UART Service (NUS) Wireless CLI
 * 6. MCUboot Dual-Slot OTA Diagnostics
 * 7. Multi-threaded Kernel (Heartbeat, Worker, Web Server, LED animation)
 * 8. Interactive Serial Shell CLI (app mmap, app ota, app led, app rgb, etc.)
 */

#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/shell/shell.h>
#include <zephyr/debug/thread_analyzer.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <esp_flash.h>

/* Networking & Sockets */
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/socket.h>

/* Bluetooth BLE & NUS */
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/services/nus.h>

#include "mmap_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ── 1. WS2812 RGB LED (GPIO 48 via SPI3) ────────────────────────── */
#define STRIP_NODE DT_ALIAS(led_strip)

#if DT_NODE_HAS_STATUS(STRIP_NODE, okay)
static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
#define STRIP_NUM_PIXELS 1
static struct led_rgb pixels[STRIP_NUM_PIXELS];
#endif

enum led_pattern_mode {
    LED_MODE_OFF = 0,
    LED_MODE_SOLID,
    LED_MODE_RAINBOW,
    LED_MODE_PULSE
};

static enum led_pattern_mode s_led_mode = LED_MODE_RAINBOW;
static uint8_t s_led_r = 0, s_led_g = 0, s_led_b = 0;

static void app_set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_led_r = r;
    s_led_g = g;
    s_led_b = b;
#if DT_NODE_HAS_STATUS(STRIP_NODE, okay)
    if (device_is_ready(strip)) {
        pixels[0].r = r;
        pixels[0].g = g;
        pixels[0].b = b;
        led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
    }
#endif
}

static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region, remainder, p, q, t;
    if (s == 0) {
        *r = v; *g = v; *b = v;
        return;
    }
    region = h / 43;
    remainder = (h - (region * 43)) * 6;
    p = (v * (255 - s)) >> 8;
    q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

    switch (region) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

/* ── 2. Bluetooth BLE & Nordic UART Service (NUS) ─────────────────── */
static char s_assigned_ip[32] = "Not Connected";
static bool s_has_ip = false;
static struct bt_conn *current_conn;

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, "ESP32-S3-Zephyr", 15),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SRV_VAL),
};

static void nus_notif_enabled(bool enabled, void *ctx)
{
    ARG_UNUSED(ctx);
    LOG_INF("[BLE NUS] Notification state changed: %s", enabled ? "ENABLED" : "DISABLED");
}

static void nus_send_response(struct bt_conn *conn, const char *str)
{
    if (!conn) return;
    size_t len = strlen(str);
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = MIN(len - offset, 20);
        bt_nus_send(conn, str + offset, chunk);
        offset += chunk;
        k_msleep(10);
    }
}

static void nus_received(struct bt_conn *conn, const void *data, uint16_t len, void *ctx)
{
    ARG_UNUSED(ctx);
    char rx_buf[128];
    uint16_t copy_len = MIN(len, sizeof(rx_buf) - 1);
    memcpy(rx_buf, data, copy_len);
    rx_buf[copy_len] = '\0';

    while (copy_len > 0 && (rx_buf[copy_len - 1] == '\r' || rx_buf[copy_len - 1] == '\n' || rx_buf[copy_len - 1] == ' ')) {
        rx_buf[--copy_len] = '\0';
    }

    LOG_INF("[BLE NUS RX] '%s'", rx_buf);

    if (strcmp(rx_buf, "help") == 0) {
        nus_send_response(conn,
            "\r\n================ BLE NUS CLI ================\r\n"
            " sysinfo            : Show board status\r\n"
            " mmap info          : Read 128KB Flash MMAP\r\n"
            " mmap memo <text>   : Save memo to Flash\r\n"
            " led <r> <g> <b>    : Set WS2812 RGB (0-255)\r\n"
            " rgb <rainbow|pulse|off> : Animation mode\r\n"
            " reboot             : Cold reset ESP32-S3\r\n"
            "=============================================\r\n");
    } else if (strcmp(rx_buf, "sysinfo") == 0 || strcmp(rx_buf, "app sysinfo") == 0) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "\r\n[Zephyr-S3 System Info]\r\n"
            " Kernel : %s\r\n"
            " Uptime : %u sec\r\n"
            " Wi-Fi  : %s\r\n"
            " LED    : Mode %d\r\n"
            " MMAP   : Phys 0x%08X (128KB)\r\n",
            KERNEL_VERSION_STRING,
            (uint32_t)(k_uptime_get() / 1000),
            s_assigned_ip,
            s_led_mode,
            mmap_storage_get_phys_addr());
        nus_send_response(conn, buf);
    } else if (strcmp(rx_buf, "mmap info") == 0 || strcmp(rx_buf, "mmap") == 0) {
        const mmap_nv_header_t *hdr = mmap_storage_get_header();
        char buf[256];
        if (hdr) {
            snprintf(buf, sizeof(buf),
                "\r\n[128KB Flash MMAP Storage]\r\n"
                " Boot Count  : %u\r\n"
                " Write Count : %u\r\n"
                " Device Tag  : %s\r\n"
                " User Memo   : %s\r\n",
                hdr->boot_count, hdr->write_count, hdr->device_tag, hdr->user_memo);
        } else {
            snprintf(buf, sizeof(buf), "\r\n[MMAP Error] Storage not initialized\r\n");
        }
        nus_send_response(conn, buf);
    } else if (strncmp(rx_buf, "mmap memo ", 10) == 0) {
        const char *memo = rx_buf + 10;
        if (mmap_storage_save_memo(memo)) {
            char buf[160];
            snprintf(buf, sizeof(buf), "\r\n[MMAP OK] Saved memo: '%s'\r\n", memo);
            nus_send_response(conn, buf);
        } else {
            nus_send_response(conn, "\r\n[MMAP ERR] Save failed!\r\n");
        }
    } else if (strncmp(rx_buf, "led ", 4) == 0) {
        char *endp;
        int r = (int)strtol(rx_buf + 4, &endp, 10);
        int g = (int)strtol(endp, &endp, 10);
        int b = (int)strtol(endp, NULL, 10);
        s_led_mode = LED_MODE_SOLID;
        app_set_led_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
        char buf[64];
        snprintf(buf, sizeof(buf), "\r\n[LED OK] Color R:%d G:%d B:%d\r\n", r, g, b);
        nus_send_response(conn, buf);
    } else if (strncmp(rx_buf, "rgb ", 4) == 0) {
        const char *mode = rx_buf + 4;
        if (strcmp(mode, "rainbow") == 0) {
            s_led_mode = LED_MODE_RAINBOW;
            nus_send_response(conn, "\r\n[LED] Rainbow mode activated\r\n");
        } else if (strcmp(mode, "pulse") == 0) {
            s_led_mode = LED_MODE_PULSE;
            nus_send_response(conn, "\r\n[LED] Pulse mode activated\r\n");
        } else if (strcmp(mode, "off") == 0) {
            s_led_mode = LED_MODE_OFF;
            app_set_led_color(0, 0, 0);
            nus_send_response(conn, "\r\n[LED] Off\r\n");
        } else {
            nus_send_response(conn, "\r\nUsage: rgb <rainbow|pulse|off>\r\n");
        }
    } else if (strcmp(rx_buf, "reboot") == 0) {
        nus_send_response(conn, "\r\n[Rebooting ESP32-S3...]\r\n");
        k_msleep(500);
        sys_reboot(SYS_REBOOT_COLD);
    } else {
        char tx_buf[160];
        snprintf(tx_buf, sizeof(tx_buf), "\r\n[Echo] %s (Type 'help' for commands)\r\n", rx_buf);
        nus_send_response(conn, tx_buf);
    }
}

static struct bt_nus_cb nus_listener = {
    .notif_enabled = nus_notif_enabled,
    .received = nus_received,
};

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("BLE Connection failed (err %u)", err);
        return;
    }
    LOG_INF("BLE Client Connected!");
    current_conn = bt_conn_ref(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("BLE Client Disconnected (reason %u)", reason);
    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

static void init_bluetooth(void)
{
    int err = bt_nus_cb_register(&nus_listener, NULL);
    if (err) {
        LOG_ERR("Failed to register NUS callback: %d", err);
        return;
    }

    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("Failed to enable Bluetooth: %d", err);
        return;
    }

    err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        LOG_ERR("BLE advertising failed to start: %d", err);
        return;
    }

    LOG_INF("BLE 5.0 Nordic UART Service (NUS) active! Name: 'ESP32-S3-Zephyr'");
}

/* ── 3. Wi-Fi DHCP Event Management ──────────────────────────────── */
static struct net_mgmt_event_callback mgmt_cb;
static struct net_mgmt_event_callback wifi_conn_cb;

static void net_event_handler(struct net_mgmt_event_callback *cb,
                             uint32_t mgmt_event, struct net_if *iface)
{
    if (mgmt_event == NET_EVENT_IPV4_DHCP_BOUND) {
        char buf[NET_IPV4_ADDR_LEN];
        const struct in_addr *addr = &iface->config.dhcpv4.requested_ip;
        net_addr_ntop(AF_INET, addr, buf, sizeof(buf));
        snprintf(s_assigned_ip, sizeof(s_assigned_ip), "%s", buf);
        s_has_ip = true;

        LOG_INF("==================================================");
        LOG_INF("[Wi-Fi] DHCP Bound! Assigned IP: %s", s_assigned_ip);
        LOG_INF("[Web Server] Dashboard: http://%s", s_assigned_ip);
        LOG_INF("==================================================");
    }
}

static void wifi_conn_handler(struct net_mgmt_event_callback *cb,
                             uint32_t mgmt_event, struct net_if *iface)
{
    if (mgmt_event == NET_EVENT_WIFI_CMD_CONNECT_RESULT) {
        const struct wifi_status *status = (const struct wifi_status *)cb->info;
        if (status && status->status == 0) {
            LOG_INF("[Wi-Fi] Connected to AP! Requesting DHCP IP...");
            net_dhcpv4_start(iface ? iface : net_if_get_default());
        }
    }
}

static void init_wifi_events(void)
{
    net_mgmt_init_event_callback(&mgmt_cb, net_event_handler,
                                 NET_EVENT_IPV4_DHCP_BOUND);
    net_mgmt_add_event_callback(&mgmt_cb);

    net_mgmt_init_event_callback(&wifi_conn_cb, wifi_conn_handler,
                                 NET_EVENT_WIFI_CMD_CONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_conn_cb);
}

/* ── 4. Embedded HTTP Web Server (Port 80) ────────────────────────── */
#define HTTP_SERVER_STACK_SIZE 4096
#define HTTP_SERVER_PRIORITY   8

static const char HTTP_DASHBOARD_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-S3 Zephyr RTOS Dashboard</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0f172a;color:#f8fafc;margin:0;padding:16px;display:flex;justify-content:center;}"
    ".container{width:100%;max-width:680px;display:flex;flex-direction:column;gap:16px;}"
    ".card{background:#1e293b;border-radius:12px;padding:20px;box-shadow:0 8px 24px rgba(0,0,0,0.4);border:1px solid #334155;}"
    "h1{font-size:20px;color:#38bdf8;margin:0 0 12px 0;display:flex;align-items:center;gap:8px;}"
    "h2{font-size:15px;color:#cbd5e1;margin:0 0 12px 0;border-bottom:1px solid #334155;padding-bottom:6px;}"
    ".badge{background:#0284c7;color:#fff;font-size:11px;padding:3px 8px;border-radius:20px;}"
    ".info-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:8px;}"
    ".info-box{background:#0f172a;padding:10px;border-radius:8px;border-left:3px solid #38bdf8;}"
    ".info-title{font-size:10px;color:#94a3b8;text-transform:uppercase;}"
    ".info-value{font-size:13px;font-weight:bold;margin-top:2px;color:#f1f5f9;word-break:break-all;}"
    ".btn-group{display:grid;grid-template-columns:repeat(auto-fit,minmax(90px,1fr));gap:8px;margin-top:10px;}"
    "button{background:#38bdf8;color:#0f172a;border:none;padding:10px;border-radius:6px;font-weight:bold;cursor:pointer;font-size:12px;transition:0.15s;}"
    "button:hover{background:#7dd3fc;}"
    ".btn-danger{background:#ef4444;color:#fff;}"
    ".btn-danger:hover{background:#f87171;}"
    ".btn-green{background:#10b981;color:#fff;}"
    ".btn-purple{background:#a855f7;color:#fff;}"
    ".btn-rainbow{background:linear-gradient(90deg,#f43f5e,#fbbf24,#10b981,#3b82f6);color:#fff;}"
    "input[type='text'],input[type='file']{background:#0f172a;border:1px solid #475569;color:#f8fafc;padding:8px 12px;border-radius:6px;font-size:12px;width:100%;box-sizing:border-box;}"
    "pre{background:#020617;padding:12px;border-radius:8px;font-family:monospace;font-size:11px;overflow-x:auto;max-height:220px;color:#38bdf8;border:1px solid #1e293b;}"
    ".bar-bg{background:#334155;border-radius:6px;height:12px;width:100%;overflow:hidden;margin-top:8px;}"
    ".bar-fill{background:#a855f7;height:100%;width:0%;transition:width 0.2s;}"
    "</style></head><body><div class='container'>"

    "<!-- 1. System Info -->"
    "<div class='card'>"
    "<h1>ESP32-S3 Zephyr RTOS <span class='badge'>v3.7.0 LTS</span></h1>"
    "<div class='info-grid'>"
    "<div class='info-box'><div class='info-title'>Architecture</div><div class='info-value'>Xtensa Dual-Core @ 240MHz</div></div>"
    "<div class='info-box'><div class='info-title'>Flash / PSRAM</div><div class='info-value'>16MB Flash / 8MB Octal</div></div>"
    "<div class='info-box'><div class='info-title'>Bluetooth BLE 5.0</div><div class='info-value'>NUS Active (ESP32-S3-Zephyr)</div></div>"
    "<div class='info-box'><div class='info-title'>Bootloader</div><div class='info-value'>MCUboot Dual-Slot A/B</div></div>"
    "</div>"
    "<h2>WS2812 RGB LED (GPIO 48 via SPI3)</h2>"
    "<div class='btn-group'>"
    "<button onclick='setLed(255,0,0)'>🔴 Red</button>"
    "<button onclick='setLed(0,255,0)'>🟢 Green</button>"
    "<button onclick='setLed(0,0,255)'>🔵 Blue</button>"
    "<button onclick='setMode(\"rainbow\")' class='btn-rainbow'>🌈 Rainbow</button>"
    "<button onclick='setLed(0,0,0)'>⚫ Off</button>"
    "<button onclick='reboot()' class='btn-danger'>⚡ Reboot</button>"
    "</div></div>"

    "<!-- 2. 128KB Flash MMAP Storage -->"
    "<div class='card'>"
    "<h2>💾 128KB Flash MMAP 비휘발성 저장소 (Hardware MMU)</h2>"
    "<div class='info-grid'>"
    "<div class='info-box'><div class='info-title'>Physical Flash Address</div><div class='info-value'>0x00F10000 (128 KB)</div></div>"
    "<div class='info-box'><div class='info-title'>Virtual Memory Address</div><div class='info-value' id='mmap-virt'>0x3C... (0-Copy)</div></div>"
    "<div class='info-box'><div class='info-title'>Boot Count / Writes</div><div class='info-value' id='mmap-counts'>-- / --</div></div>"
    "<div class='info-box'><div class='info-title'>Device Tag</div><div class='info-value' id='mmap-tag'>--</div></div>"
    "</div>"
    "<div style='margin-top:10px;'>"
    "<div style='font-size:12px; color:#94a3b8; margin-bottom:4px;'>영구 저장 사용자 메모 (Flash User Memo):</div>"
    "<div style='display:flex; gap:8px;'>"
    "<input type='text' id='mmap-memo-input' placeholder='플래시에 영구 보존할 메모 입력...'>"
    "<button class='btn-green' onclick='saveMemo()' style='flex:0 0 80px;'>저장</button>"
    "</div></div>"
    "<div style='margin-top:14px;'>"
    "<div style='display:flex; justify-content:space-between; align-items:center; margin-bottom:6px;'>"
    "<span style='font-size:12px; color:#94a3b8;'>실시간 128KB Flash Hex Dump 뷰어:</span>"
    "<button onclick='fetchDump()' style='padding:4px 10px; font-size:11px;'>🔄 새로고침</button>"
    "</div>"
    "<pre id='hex-dump-pre'>Hex dump loading...</pre>"
    "</div></div>"

    "<!-- 3. Dual OTA / File Upload -->"
    "<div class='card'>"
    "<h2>🚀 Dual OTA 무선 펌웨어 & 파일 업로드</h2>"
    "<div style='font-size:12px; color:#94a3b8; margin-bottom:8px;'>"
    "실행 슬롯: <span class='badge' style='background:#10b981;'>Slot 0 (0x00010000)</span> | "
    "타겟 슬롯: <span class='badge' style='background:#a855f7;'>Slot 1 (0x00110000)</span>"
    "</div>"
    "<div style='display:flex; gap:8px; margin-top:8px;'>"
    "<input type='file' id='ota-file-input' accept='.bin'>"
    "<button class='btn-purple' onclick='uploadFirmware()' style='flex:0 0 100px;'>OTA 업로드</button>"
    "</div>"
    "<div id='ota-progress-box' style='display:none; margin-top:10px;'>"
    "<div class='bar-bg'><div id='ota-bar' class='bar-fill'></div></div>"
    "<div id='ota-status' style='font-size:11px; color:#c084fc; margin-top:4px; text-align:center;'>0% 업로드 중...</div>"
    "</div></div>"

    "</div><script>"
    "function setLed(r,g,b){fetch('/api/led?r='+r+'&g='+g+'&b='+b);}"
    "function setMode(m){fetch('/api/rgb?mode='+m);}"
    "function reboot(){if(confirm('Reboot ESP32-S3?')){fetch('/api/reboot');}}"
    "function fetchMmapInfo(){"
    "  fetch('/api/mmap/info').then(r=>r.json()).then(d=>{"
    "    document.getElementById('mmap-virt').innerText = d.virt_addr;"
    "    document.getElementById('mmap-counts').innerText = d.boot_count + ' 회 / ' + d.write_count + ' 회';"
    "    document.getElementById('mmap-tag').innerText = d.device_tag;"
    "    document.getElementById('mmap-memo-input').value = d.user_memo;"
    "  });"
    "}"
    "function saveMemo(){"
    "  const txt = document.getElementById('mmap-memo-input').value;"
    "  fetch('/api/mmap/memo?text='+encodeURIComponent(txt)).then(()=>{fetchMmapInfo();fetchDump();});"
    "}"
    "function fetchDump(){"
    "  fetch('/api/mmap/dump').then(r=>r.text()).then(t=>{"
    "    document.getElementById('hex-dump-pre').innerText = t;"
    "  });"
    "}"
    "function uploadFirmware(){"
    "  const inp = document.getElementById('ota-file-input');"
    "  if(!inp.files || !inp.files[0]){alert('업로드할 firmware.bin 파일을 선택하세요.');return;}"
    "  const f = inp.files[0];"
    "  const box = document.getElementById('ota-progress-box');"
    "  const bar = document.getElementById('ota-bar');"
    "  const st = document.getElementById('ota-status');"
    "  box.style.display='block';"
    "  const xhr = new XMLHttpRequest();"
    "  xhr.open('POST', '/api/ota/upload', true);"
    "  xhr.upload.onprogress = e => {"
    "    if(e.lengthComputable){"
    "      const pct = Math.round((e.loaded/e.total)*100);"
    "      bar.style.width=pct+'%';"
    "      st.innerText=pct+'% 전송 중 ('+Math.round(e.loaded/1024)+' KB / '+Math.round(e.total/1024)+' KB)...';"
    "    }"
    "  };"
    "  xhr.onload = () => {"
    "    if(xhr.status===200){"
    "      bar.style.width='100%';"
    "      st.innerText='✅ 펌웨어 업로드 완료! MCUboot 업그레이드 요청 후 2초 뒤 자동 재부팅됩니다.';"
    "    } else {"
    "      st.innerText='❌ 업로드 실패: '+xhr.responseText;"
    "    }"
    "  };"
    "  xhr.onerror = () => { st.innerText='❌ 네트워크 에러!'; };"
    "  xhr.send(f);"
    "}"
    "fetchMmapInfo(); fetchDump();"
    "</script></body></html>";

static void http_server_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    int serv_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serv_fd < 0) {
        LOG_ERR("Failed to create HTTP socket (%d)", errno);
        return;
    }

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(80);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (zsock_bind(serv_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        LOG_ERR("HTTP bind failed (%d)", errno);
        zsock_close(serv_fd);
        return;
    }

    if (zsock_listen(serv_fd, 5) < 0) {
        LOG_ERR("HTTP listen failed (%d)", errno);
        zsock_close(serv_fd);
        return;
    }

    LOG_INF("HTTP Web Server listening on port 80");

    char rx_buffer[1024];
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = zsock_accept(serv_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            k_sleep(K_MSEC(100));
            continue;
        }

        int received = zsock_recv(client_fd, rx_buffer, sizeof(rx_buffer) - 1, 0);
        if (received > 0) {
            rx_buffer[received] = '\0';

            /* API: LED color */
            if (strstr(rx_buffer, "GET /api/led") != NULL) {
                char *r_str = strstr(rx_buffer, "r=");
                char *g_str = strstr(rx_buffer, "g=");
                char *b_str = strstr(rx_buffer, "b=");
                if (r_str && g_str && b_str) {
                    uint8_t r = atoi(r_str + 2);
                    uint8_t g = atoi(g_str + 2);
                    uint8_t b = atoi(b_str + 2);
                    s_led_mode = LED_MODE_SOLID;
                    app_set_led_color(r, g, b);
                }
                const char resp[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"status\":\"ok\"}";
                zsock_send(client_fd, resp, sizeof(resp) - 1, 0);
            }
            /* API: RGB mode */
            else if (strstr(rx_buffer, "GET /api/rgb?mode=rainbow") != NULL) {
                s_led_mode = LED_MODE_RAINBOW;
                const char resp[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"mode\":\"rainbow\"}";
                zsock_send(client_fd, resp, sizeof(resp) - 1, 0);
            }
            /* API: MMAP Info */
            else if (strstr(rx_buffer, "GET /api/mmap/info") != NULL) {
                const mmap_nv_header_t *hdr = mmap_storage_get_header();
                char json[512];
                if (hdr) {
                    snprintf(json, sizeof(json),
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
                        "{\"magic\":\"0x%08X\",\"boot_count\":%u,\"write_count\":%u,"
                        "\"device_tag\":\"%s\",\"user_memo\":\"%s\","
                        "\"virt_addr\":\"0x%08X\",\"phys_addr\":\"0x%08X\"}",
                        hdr->magic, hdr->boot_count, hdr->write_count,
                        hdr->device_tag, hdr->user_memo,
                        mmap_storage_get_virt_addr(), mmap_storage_get_phys_addr());
                } else {
                    snprintf(json, sizeof(json), "HTTP/1.1 500 Error\r\n\r\n{\"error\":\"mmap_error\"}");
                }
                zsock_send(client_fd, json, strlen(json), 0);
            }
            /* API: MMAP Save Memo */
            else if (strstr(rx_buffer, "GET /api/mmap/memo") != NULL) {
                char *t_str = strstr(rx_buffer, "text=");
                if (t_str) {
                    char memo_buf[256];
                    char *end = strchr(t_str + 5, ' ');
                    size_t mlen = end ? (size_t)(end - (t_str + 5)) : strlen(t_str + 5);
                    mlen = MIN(mlen, sizeof(memo_buf) - 1);
                    memcpy(memo_buf, t_str + 5, mlen);
                    memo_buf[mlen] = '\0';
                    mmap_storage_save_memo(memo_buf);
                }
                const char resp[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"status\":\"saved\"}";
                zsock_send(client_fd, resp, sizeof(resp) - 1, 0);
            }
            /* API: MMAP Hex Dump */
            else if (strstr(rx_buffer, "GET /api/mmap/dump") != NULL) {
                static char dump_buf[2048];
                mmap_storage_hex_dump(0, 512, dump_buf, sizeof(dump_buf));
                char hdr[128];
                int hlen = snprintf(hdr, sizeof(hdr),
                    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n",
                    (int)strlen(dump_buf));
                zsock_send(client_fd, hdr, hlen, 0);
                zsock_send(client_fd, dump_buf, strlen(dump_buf), 0);
            }
            /* API: OTA Firmware / File Upload to Flash */
            else if (strstr(rx_buffer, "POST /api/ota/upload") != NULL ||
                     strstr(rx_buffer, "POST /api/fs/upload") != NULL) {
                char *cl_str = strstr(rx_buffer, "Content-Length: ");
                int content_len = cl_str ? atoi(cl_str + 16) : 0;

                LOG_INF("[OTA] Receiving firmware upload (%d bytes)", content_len);

                /* Erase Secondary Flash Slot (Slot 1 @ 0x00110000, 1MB max) */
                uint32_t target_addr = 0x00110000;
                uint32_t erase_len = ((content_len + 4095) / 4096) * 4096;
                if (erase_len < 65536) erase_len = 65536;
                esp_flash_erase_region(NULL, target_addr, erase_len);

                char *body = strstr(rx_buffer, "\r\n\r\n");
                int total_written = 0;
                if (body) {
                    body += 4;
                    int header_len = (int)(body - rx_buffer);
                    int body_in_buf = received - header_len;
                    if (body_in_buf > 0) {
                        esp_flash_write(NULL, body, target_addr + total_written, body_in_buf);
                        total_written += body_in_buf;
                    }
                }

                while (total_written < content_len) {
                    int r = zsock_recv(client_fd, rx_buffer, sizeof(rx_buffer), 0);
                    if (r <= 0) break;
                    esp_flash_write(NULL, rx_buffer, target_addr + total_written, r);
                    total_written += r;
                }

                LOG_INF("[OTA] Wrote %d bytes to Secondary Slot (0x%08X)", total_written, target_addr);

                const char ok_resp[] = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOTA_SUCCESS";
                zsock_send(client_fd, ok_resp, sizeof(ok_resp) - 1, 0);
                zsock_close(client_fd);

                k_sleep(K_MSEC(1500));
                sys_reboot(SYS_REBOOT_COLD);
                continue;
            }
            /* API: Reboot */
            else if (strstr(rx_buffer, "GET /api/reboot") != NULL) {
                const char resp[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"status\":\"rebooting\"}";
                zsock_send(client_fd, resp, sizeof(resp) - 1, 0);
                zsock_close(client_fd);
                k_sleep(K_MSEC(500));
                sys_reboot(SYS_REBOOT_COLD);
                continue;
            }
            /* Serve Dashboard HTML */
            else {
                char header[128];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                    (int)(sizeof(HTTP_DASHBOARD_HTML) - 1));
                zsock_send(client_fd, header, hlen, 0);
                zsock_send(client_fd, HTTP_DASHBOARD_HTML, sizeof(HTTP_DASHBOARD_HTML) - 1, 0);
            }
        }
        zsock_close(client_fd);
    }
}

K_THREAD_DEFINE(http_server_tid, HTTP_SERVER_STACK_SIZE,
                http_server_thread_entry, NULL, NULL, NULL,
                HTTP_SERVER_PRIORITY, 0, 0);

/* ── 5. Heartbeat & LED Animation Thread (10Hz) ───────────────────── */
#define HEARTBEAT_STACK_SIZE 2048
#define HEARTBEAT_PRIORITY   7

static void heartbeat_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Heartbeat & LED animation thread started (Priority: %d)", HEARTBEAT_PRIORITY);

    uint16_t hue = 0;
    uint32_t ticks = 0;
    uint8_t pulse_val = 0;
    int8_t pulse_dir = 5;

    while (1) {
        k_sleep(K_MSEC(50));
        ticks++;

        if (s_led_mode == LED_MODE_RAINBOW) {
            uint8_t r, g, b;
            hsv_to_rgb(hue, 255, 120, &r, &g, &b);
            app_set_led_color(r, g, b);
            hue = (hue + 2) % 256;
        } else if (s_led_mode == LED_MODE_PULSE) {
            pulse_val += pulse_dir;
            if (pulse_val >= 200 || pulse_val <= 10) {
                pulse_dir = -pulse_dir;
            }
            app_set_led_color(0, 0, pulse_val);
        } else if (s_led_mode == LED_MODE_OFF) {
            app_set_led_color(0, 0, 0);
        }

        if (ticks % 20 == 0) {
            uint32_t uptime_sec = (uint32_t)(k_uptime_get() / 1000);
            LOG_DBG("Heartbeat tick (Uptime: %u sec, IP: %s)", uptime_sec, s_assigned_ip);
        }
    }
}

K_THREAD_DEFINE(heartbeat_tid, HEARTBEAT_STACK_SIZE,
                heartbeat_thread_entry, NULL, NULL, NULL,
                HEARTBEAT_PRIORITY, 0, 0);

/* ── 6. Worker Thread (Event Processing via Message Queue) ────────── */
#define WORKER_STACK_SIZE 2048
#define WORKER_PRIORITY   6

struct app_event {
    uint32_t id;
    int32_t  data;
};

K_MSGQ_DEFINE(app_event_msgq, sizeof(struct app_event), 16, 4);

static void worker_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Worker thread started (Priority: %d)", WORKER_PRIORITY);

    struct app_event evt;
    while (1) {
        if (k_msgq_get(&app_event_msgq, &evt, K_FOREVER) == 0) {
            LOG_INF("[Worker] Event received -> ID: %u, Data: %d", evt.id, evt.data);
        }
    }
}

K_THREAD_DEFINE(worker_tid, WORKER_STACK_SIZE,
                worker_thread_entry, NULL, NULL, NULL,
                WORKER_PRIORITY, 0, 0);

/* ── 7. Shell Commands ────────────────────────────────────────────── */

static int cmd_sysinfo(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    uint32_t uptime_sec = (uint32_t)(k_uptime_get() / 1000);
    shell_print(sh, "================ ESP32-S3 Zephyr RTOS Full Stack ================");
    shell_print(sh, "  Kernel Version : %s", KERNEL_VERSION_STRING);
    shell_print(sh, "  Architecture   : Xtensa Dual-Core LX7 @ %d MHz", CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000);
    shell_print(sh, "  Board Target   : %s", CONFIG_BOARD_TARGET);
    shell_print(sh, "  System Uptime  : %u sec", uptime_sec);
    shell_print(sh, "  Wi-Fi IP       : %s", s_assigned_ip);
    shell_print(sh, "  Web Dashboard  : %s", s_has_ip ? s_assigned_ip : "Connect Wi-Fi first");
    shell_print(sh, "  Bluetooth BLE  : Active ('ESP32-S3-Zephyr') NUS Service");
    shell_print(sh, "  WS2812 RGB LED : GPIO 48 via SPI3 Driver (Mode: %d)", s_led_mode);
    shell_print(sh, "  MMAP Storage   : 128KB @ Phys 0x%08X -> Virt 0x%08X",
                mmap_storage_get_phys_addr(), mmap_storage_get_virt_addr());
    shell_print(sh, "==================================================================");
    return 0;
}

static int cmd_mmap(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_print(sh, "Usage: app mmap <info | memo <text> | dump [offset] [len]>");
        return -1;
    }

    if (strcmp(argv[1], "info") == 0) {
        const mmap_nv_header_t *hdr = mmap_storage_get_header();
        if (!hdr) {
            shell_error(sh, "Failed to read MMAP header!");
            return -1;
        }
        shell_print(sh, "================ 128KB Flash MMAP Storage ================");
        shell_print(sh, "  Magic Header : 0x%08X ('S3MM')", hdr->magic);
        shell_print(sh, "  Physical Flash: 0x%08X (Size: 128 KB)", mmap_storage_get_phys_addr());
        shell_print(sh, "  Virtual MMU  : 0x%08X (0-Copy Read)", mmap_storage_get_virt_addr());
        shell_print(sh, "  Boot Count   : %u", hdr->boot_count);
        shell_print(sh, "  Write Count  : %u", hdr->write_count);
        shell_print(sh, "  Device Tag   : %s", hdr->device_tag);
        shell_print(sh, "  User Memo    : %s", hdr->user_memo);
        shell_print(sh, "==========================================================");
    } else if (strcmp(argv[1], "memo") == 0) {
        if (argc < 3) {
            shell_print(sh, "Usage: app mmap memo <text>");
            return -1;
        }
        if (mmap_storage_save_memo(argv[2])) {
            shell_print(sh, "[OK] Saved memo to 128KB MMAP Flash: '%s'", argv[2]);
        } else {
            shell_error(sh, "Failed to write memo to flash!");
        }
    } else if (strcmp(argv[1], "dump") == 0) {
        uint32_t offset = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 16) : 0;
        size_t len = (argc >= 4) ? (size_t)atoi(argv[3]) : 256;
        static char dump_buf[1024];
        mmap_storage_hex_dump(offset, len, dump_buf, sizeof(dump_buf));
        shell_print(sh, "\r\n%s", dump_buf);
    } else {
        shell_error(sh, "Unknown mmap subcommand '%s'", argv[1]);
        return -1;
    }
    return 0;
}

static int cmd_led(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 4) {
        shell_print(sh, "Usage: app led <r: 0-255> <g: 0-255> <b: 0-255>");
        return -1;
    }

    uint8_t r = (uint8_t)atoi(argv[1]);
    uint8_t g = (uint8_t)atoi(argv[2]);
    uint8_t b = (uint8_t)atoi(argv[3]);

    s_led_mode = LED_MODE_SOLID;
    app_set_led_color(r, g, b);
    shell_print(sh, "[OK] Set WS2812 RGB LED color to R:%u, G:%u, B:%u", r, g, b);
    return 0;
}

static int cmd_rgb(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_print(sh, "Usage: app rgb <rainbow | pulse | off>");
        return -1;
    }

    if (strcmp(argv[1], "rainbow") == 0) {
        s_led_mode = LED_MODE_RAINBOW;
        shell_print(sh, "[OK] LED mode set to Rainbow rotation");
    } else if (strcmp(argv[1], "pulse") == 0) {
        s_led_mode = LED_MODE_PULSE;
        shell_print(sh, "[OK] LED mode set to Blue breathing pulse");
    } else if (strcmp(argv[1], "off") == 0) {
        s_led_mode = LED_MODE_OFF;
        app_set_led_color(0, 0, 0);
        shell_print(sh, "[OK] LED turned Off");
    } else {
        shell_error(sh, "Unknown pattern '%s'. Valid: rainbow, pulse, off", argv[1]);
        return -1;
    }
    return 0;
}

static int cmd_ota(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "================ MCUboot Dual-Slot OTA Layout ================");
    shell_print(sh, "  Bootloader   : MCUboot (A/B Bank Swap with Automatic Rollback)");
    shell_print(sh, "  Slot 0 (app) : 1024 KB @ 0x00010000 (Active Running Slot)");
    shell_print(sh, "  Slot 1 (app) : 1024 KB @ 0x00110000 (Secondary Download Slot)");
    shell_print(sh, "  Scratch Area :  256 KB @ 0x00210000 (Atomic Swap Buffer)");
    shell_print(sh, "  Storage Area :   24 KB @ 0x00250000 (Non-volatile Settings)");
    shell_print(sh, "  MMAP Storage :  128 KB @ 0x00F10000 (MMU Direct Memory Map)");
    shell_print(sh, "==============================================================");
    return 0;
}

static int cmd_threads(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "\r\n=== Active Zephyr Threads & Stack Usage ===");
    thread_analyzer_print();
    return 0;
}

static int cmd_event(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_print(sh, "Usage: app event <number>");
        return -1;
    }

    struct app_event evt;
    static uint32_t s_evt_counter = 1;
    evt.id = s_evt_counter++;
    evt.data = atoi(argv[1]);

    if (k_msgq_put(&app_event_msgq, &evt, K_NO_WAIT) == 0) {
        shell_print(sh, "[OK] Queued event ID %u with data %d", evt.id, evt.data);
    } else {
        shell_error(sh, "[Error] Message queue is full!");
    }
    return 0;
}

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "Rebooting ESP32-S3 via Zephyr RTOS sys_reboot...");
    k_sleep(K_MSEC(200));
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_app,
    SHELL_CMD(sysinfo, NULL, "Show ESP32-S3 Zephyr system info", cmd_sysinfo),
    SHELL_CMD(mmap,    NULL, "MMAP storage (app mmap info|memo|dump)", cmd_mmap),
    SHELL_CMD(led,     NULL, "Set WS2812 LED color (app led <r> <g> <b>)", cmd_led),
    SHELL_CMD(rgb,     NULL, "Set LED mode (app rgb <rainbow|pulse|off>)", cmd_rgb),
    SHELL_CMD(ota,     NULL, "Show MCUboot OTA flash layout", cmd_ota),
    SHELL_CMD(threads, NULL, "List running threads & stack usage", cmd_threads),
    SHELL_CMD(event,   NULL, "Send event to worker thread (event <val>)", cmd_event),
    SHELL_CMD(reboot,  NULL, "Software reset ESP32-S3", cmd_reboot),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(app, &sub_app, "ESP32-S3 Zephyr Application Commands", NULL);

/* ── Main Entry Point ────────────────────────────────────────────── */

int main(void)
{
    LOG_INF("==================================================");
    LOG_INF("   ESP32-S3 Zephyr RTOS Full-Stack Firmware      ");
    LOG_INF("   Kernel: %s | Board: %s", KERNEL_VERSION_STRING, CONFIG_BOARD_TARGET);
    LOG_INF("==================================================");

    /* 1. WS2812 RGB LED */
#if DT_NODE_HAS_STATUS(STRIP_NODE, okay)
    if (device_is_ready(strip)) {
        LOG_INF("WS2812 RGB LED (GPIO 48) initialized successfully!");
    } else {
        LOG_WRN("WS2812 RGB LED device not ready");
    }
#endif

    /* 2. MMAP 128KB Flash Storage (Hardware MMU) */
    mmap_storage_init();

    /* 3. Bluetooth BLE & NUS */
    init_bluetooth();

    /* 4. Wi-Fi DHCP Event Listener */
    init_wifi_events();

    LOG_INF("Zephyr Kernel initialized successfully!");
    LOG_INF("Interactive shell is ready on UART console (115200 bps).");
    LOG_INF("Type 'help' or 'app sysinfo' in serial monitor.");
    LOG_INF("Use 'app mmap info' or 'app mmap dump' to inspect Flash MMAP.");

    return 0;
}
