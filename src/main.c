/*
 * Copyright (c) 2026
 * Zephyr RTOS Full-Stack Verification Firmware for ESP32-S3 (N16R8)
 *
 * Implements:
 * 1. WS2812B RGB LED (GPIO 48 via SPI3 Driver & DeviceTree)
 * 2. Wi-Fi 4 (802.11 b/g/n) with Zephyr L2 Shell (wifi scan, wifi connect)
 * 3. BLE 5.0 Nordic UART Service (NUS) Wireless CLI
 * 4. Embedded HTTP Web Server & REST API on Port 80 (BSD Sockets)
 * 5. Flash Map & MCUboot Dual-Slot OTA Diagnostics
 * 6. Multi-threaded Kernel (Heartbeat, Worker, Web Server, LED animation)
 * 7. Interactive Serial Shell CLI (app sysinfo, app led, app rgb, app ota, etc.)
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

/* Networking & Sockets */
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/socket.h>

/* Bluetooth BLE & NUS */
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/services/nus.h>

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

/* HSV to RGB conversion for smooth rainbow effect */
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

static void nus_received(struct bt_conn *conn, const void *data, uint16_t len, void *ctx)
{
    ARG_UNUSED(ctx);
    char rx_buf[128];
    uint16_t copy_len = MIN(len, sizeof(rx_buf) - 1);
    memcpy(rx_buf, data, copy_len);
    rx_buf[copy_len] = '\0';

    LOG_INF("[BLE NUS RX] %s", rx_buf);

    /* Echo response with Zephyr signature */
    char tx_buf[160];
    int tx_len = snprintf(tx_buf, sizeof(tx_buf), "[Zephyr-S3] Echo: %s\r\n", rx_buf);
    bt_nus_send(conn, tx_buf, tx_len);
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

    err = bt_le_adv_start(BT_LE_ADV_CONN_NAME, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        LOG_ERR("BLE advertising failed to start: %d", err);
        return;
    }

    LOG_INF("BLE 5.0 Nordic UART Service (NUS) active! Name: 'ESP32-S3-Zephyr'");
}

/* ── 3. Wi-Fi DHCP Event Management ──────────────────────────────── */
static struct net_mgmt_event_callback mgmt_cb;
static char s_assigned_ip[32] = "Not Connected";
static bool s_has_ip = false;

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

static void init_wifi_events(void)
{
    net_mgmt_init_event_callback(&mgmt_cb, net_event_handler,
                                 NET_EVENT_IPV4_DHCP_BOUND);
    net_mgmt_add_event_callback(&mgmt_cb);
}

/* ── 4. Embedded HTTP Web Server (Port 80) ────────────────────────── */
#define HTTP_SERVER_STACK_SIZE 4096
#define HTTP_SERVER_PRIORITY   8

static const char HTTP_DASHBOARD_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-S3 Zephyr RTOS</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0f172a;color:#f8fafc;margin:0;padding:20px;display:flex;justify-content:center;}"
    ".card{background:#1e293b;border-radius:12px;padding:24px;width:100%;max-width:540px;box-shadow:0 10px 25px rgba(0,0,0,0.5);}"
    "h1{font-size:22px;color:#38bdf8;margin-top:0;display:flex;align-items:center;gap:10px;}"
    ".badge{background:#0284c7;color:#fff;font-size:12px;padding:3px 8px;border-radius:20px;}"
    ".info-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:16px 0;}"
    ".info-box{background:#0f172a;padding:12px;border-radius:8px;border-left:4px solid #38bdf8;}"
    ".info-title{font-size:11px;color:#94a3b8;text-transform:uppercase;}"
    ".info-value{font-size:15px;font-weight:bold;margin-top:4px;color:#f1f5f9;}"
    ".btn-group{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:16px;}"
    "button{background:#38bdf8;color:#0f172a;border:none;padding:12px;border-radius:8px;font-weight:bold;cursor:pointer;transition:0.2s;}"
    "button:hover{background:#7dd3fc;}"
    ".btn-danger{background:#ef4444;color:#fff;}"
    ".btn-danger:hover{background:#f87171;}"
    ".btn-rainbow{background:linear-gradient(90deg,#f43f5e,#fbbf24,#10b981,#3b82f6);color:#fff;}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>ESP32-S3 Zephyr RTOS <span class='badge'>v3.7.0</span></h1>"
    "<div class='info-grid'>"
    "<div class='info-box'><div class='info-title'>Architecture</div><div class='info-value'>Xtensa Dual-Core LX7</div></div>"
    "<div class='info-box'><div class='info-title'>CPU Frequency</div><div class='info-value'>240 MHz</div></div>"
    "<div class='info-box'><div class='info-title'>BLE 5.0 NUS</div><div class='info-value'>Active (ESP32-S3-Zephyr)</div></div>"
    "<div class='info-box'><div class='info-title'>OTA Bootloader</div><div class='info-value'>MCUboot Dual-Slot</div></div>"
    "</div>"
    "<h3>WS2812 RGB LED Control (GPIO 48)</h3>"
    "<div class='btn-group'>"
    "<button onclick='setLed(255,0,0)'>🔴 Red</button>"
    "<button onclick='setLed(0,255,0)'>🟢 Green</button>"
    "<button onclick='setLed(0,0,255)'>🔵 Blue</button>"
    "<button onclick='setMode(\"rainbow\")' class='btn-rainbow'>🌈 Rainbow</button>"
    "<button onclick='setLed(0,0,0)'>⚫ Off</button>"
    "<button onclick='reboot()' class='btn-danger'>⚡ Reboot</button>"
    "</div></div>"
    "<script>"
    "function setLed(r,g,b){fetch('/api/led?r='+r+'&g='+g+'&b='+b);}"
    "function setMode(m){fetch('/api/rgb?mode='+m);}"
    "function reboot(){if(confirm('Reboot ESP32-S3?')){fetch('/api/reboot');}}"
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

    char rx_buffer[512];
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
            } else if (strstr(rx_buffer, "GET /api/rgb?mode=rainbow") != NULL) {
                s_led_mode = LED_MODE_RAINBOW;
                const char resp[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"mode\":\"rainbow\"}";
                zsock_send(client_fd, resp, sizeof(resp) - 1, 0);
            } else if (strstr(rx_buffer, "GET /api/reboot") != NULL) {
                const char resp[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"status\":\"rebooting\"}";
                zsock_send(client_fd, resp, sizeof(resp) - 1, 0);
                zsock_close(client_fd);
                k_sleep(K_MSEC(500));
                sys_reboot(SYS_REBOOT_COLD);
            } else {
                /* Serve Dashboard HTML */
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
    shell_print(sh, "==================================================================");
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

    /* 2. Bluetooth BLE & NUS */
    init_bluetooth();

    /* 3. Wi-Fi DHCP Event Listener */
    init_wifi_events();

    LOG_INF("Zephyr Kernel initialized successfully!");
    LOG_INF("Interactive shell is ready on UART console (115200 bps).");
    LOG_INF("Type 'help' or 'app sysinfo' in serial monitor.");
    LOG_INF("Use 'wifi scan' and 'wifi connect <SSID> -p <PW>' to connect Wi-Fi.");

    return 0;
}
