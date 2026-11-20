/**
 * @file debug_cli.cpp
 * @brief Dual-Interface (USB Serial + BLE UART) CLI Shell for ESP32-S3
 *        Supports standard Nordic UART Service (NUS)
 */

#include "debug_cli.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <esp_system.h>
#include <WiFi.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

/* ── Nordic UART Service (NUS) Standard UUIDs ───────────────── */
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

/* ── External LED control functions ────────────────────────── */
extern void LED_SetColor(uint8_t r, uint8_t g, uint8_t b);
extern void LED_SetMode(uint8_t mode);
extern void LED_SetBrightness(uint8_t bright);
extern uint8_t LED_GetMode(void);

/* ── Command Table ──────────────────────────────────────────── */
static cli_command_t cmd_table[CLI_MAX_COMMANDS];
static uint8_t cmd_count = 0;

/* ── Ring Buffer (USB + BLE 통합 큐) ────────────────────────── */
static volatile char     rx_ring[CLI_RX_RING_SIZE];
static volatile uint16_t rx_head = 0;
static volatile uint16_t rx_tail = 0;

/* ── Line Buffer ─────────────────────────────────────────────── */
static char    rx_line_buf[CLI_MAX_CMD_LEN];
static uint8_t rx_line_index = 0;

/* ── BLE Server & Characteristics ───────────────────────────── */
static BLEServer* pServer = nullptr;
static BLECharacteristic* pTxCharacteristic = nullptr;
static bool deviceConnected = false;
static bool oldDeviceConnected = false;

/* ── Output Capture for Web CLI ─────────────────────────────── */
static bool g_capture_output = false;
static String g_captured_text = "";

bool CLI_IsBLEConnected(void)
{
    return deviceConnected;
}

void CLI_PutRxChar(char ch)
{
    uint16_t next_head = (rx_head + 1) % CLI_RX_RING_SIZE;
    if (next_head != rx_tail) {
        rx_ring[rx_head] = ch;
        rx_head = next_head;
    }
}

void CLI_Print(const char *str)
{
    if (g_capture_output) {
        g_captured_text += str;
    }
    Serial.print(str);
    if (deviceConnected && pTxCharacteristic != nullptr) {
        // BLE MTU 크기 고려하여 청크 단위 전송
        size_t len = strlen(str);
        size_t sent = 0;
        while (sent < len) {
            size_t chunk = (len - sent > 20) ? 20 : (len - sent);
            pTxCharacteristic->setValue((uint8_t*)(str + sent), chunk);
            pTxCharacteristic->notify();
            sent += chunk;
            delay(5);
        }
    }
}

void CLI_Printf(const char *format, ...)
{
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    CLI_Print(buf);
}

/* ── BLE Callbacks ──────────────────────────────────────────── */
class ServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
        deviceConnected = true;
    }

    void onDisconnect(BLEServer* pServer) override {
        deviceConnected = false;
    }
};

class RxCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) override {
        String rxValue = pCharacteristic->getValue();
        if (rxValue.length() > 0) {
            for (size_t i = 0; i < rxValue.length(); i++) {
                CLI_PutRxChar(rxValue[i]);
            }
        }
    }
};

/* ════════════════════════════════════════════════════════════════
 *  Built-in Command Handlers
 * ════════════════════════════════════════════════════════════════ */

static int cli_cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    CLI_Print("\r\n================ AVAILABLE CLI COMMANDS ================\r\n");
    for (uint8_t i = 0; i < cmd_count; i++) {
        CLI_Printf("  %-12s : %s\r\n", cmd_table[i].name, cmd_table[i].help);
    }
    CLI_Print("========================================================\r\n");
    return 0;
}

static int cli_cmd_sysinfo(int argc, char **argv)
{
    (void)argc; (void)argv;
    CLI_Print("\r\n=== ESP32-S3-WROOM-1 (N16R8) System Status ===\r\n");
    CLI_Printf("  MCU Target : ESP32-S3 (Xtensa LX7 Dual Core)\r\n");
    CLI_Printf("  Core Clock : %lu MHz\r\n", (unsigned long)getCpuFrequencyMhz());
    CLI_Printf("  Flash      : 16MB (SPI)\r\n");
    CLI_Printf("  PSRAM      : %lu bytes (8MB OPI)\r\n", (unsigned long)ESP.getPsramSize());
    CLI_Printf("  Free Heap  : %lu bytes\r\n", (unsigned long)ESP.getFreeHeap());
    CLI_Printf("  SDK Ver    : %s\r\n", ESP.getSdkVersion());
    CLI_Printf("  MAC Addr   : %s\r\n", WiFi.macAddress().c_str());
    CLI_Printf("  SoftAP IP  : %s (SSID: ESP32-S3-WIFI)\r\n", WiFi.softAPIP().toString().c_str());
    if (WiFi.status() == WL_CONNECTED) {
        CLI_Printf("  Station IP : %s (SSID: %s)\r\n", WiFi.localIP().toString().c_str(), WiFi.SSID().c_str());
    } else {
        CLI_Print("  Station    : Disconnected\r\n");
    }
    CLI_Printf("  BLE Name   : %s (%s)\r\n", BLE_DEVICE_NAME, deviceConnected ? "CONNECTED" : "ADVERTISING");
    CLI_Printf("  Uptime     : %lu ms\r\n", (unsigned long)millis());
    CLI_Printf("  RGB LED    : GPIO48 (WS2812B)\r\n");
    CLI_Print("===============================================\r\n");
    return 0;
}

static int cli_cmd_top(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint32_t totalHeap = ESP.getHeapSize();
    uint32_t freeHeap  = ESP.getFreeHeap();
    uint32_t usedHeap  = totalHeap - freeHeap;
    float heapUsage = totalHeap ? ((float)usedHeap / totalHeap) * 100.0f : 0.0f;

    uint32_t totalPsram = ESP.getPsramSize();
    uint32_t freePsram  = ESP.getFreePsram();
    uint32_t usedPsram  = totalPsram - freePsram;
    float psramUsage = totalPsram ? ((float)usedPsram / totalPsram) * 100.0f : 0.0f;

    float tempC = temperatureRead();

    CLI_Print("\r\n================ ESP32-S3 SYSTEM STATS ================\r\n");
    CLI_Printf("  CPU Frequency : %lu MHz\r\n", (unsigned long)getCpuFrequencyMhz());
    CLI_Printf("  Chip Temp     : %.1f °C\r\n", tempC);
    CLI_Printf("  Internal Heap : %lu / %lu bytes (%.1f%% used)\r\n", (unsigned long)usedHeap, (unsigned long)totalHeap, heapUsage);
    CLI_Printf("  8MB OPI PSRAM : %lu / %lu bytes (%.1f%% used)\r\n", (unsigned long)usedPsram, (unsigned long)totalPsram, psramUsage);
    if (WiFi.status() == WL_CONNECTED) {
        CLI_Printf("  Wi-Fi RSSI    : %d dBm (SSID: %s)\r\n", WiFi.RSSI(), WiFi.SSID().c_str());
    }
    CLI_Printf("  Uptime        : %lu sec\r\n", (unsigned long)(millis() / 1000));
    CLI_Print("=======================================================\r\n");
    return 0;
}

static int cli_cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        CLI_Print("Usage:\r\n");
        CLI_Print("  wifi status\r\n");
        CLI_Print("  wifi scan\r\n");
        CLI_Print("  wifi connect <SSID> <PASSWORD>\r\n");
        return -1;
    }

    if (strcmp(argv[1], "status") == 0) {
        CLI_Printf("[WIFI] SoftAP IP: %s\r\n", WiFi.softAPIP().toString().c_str());
        if (WiFi.status() == WL_CONNECTED) {
            CLI_Printf("[WIFI] Station Connected to '%s', IP: %s\r\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        } else {
            CLI_Print("[WIFI] Station Not Connected\r\n");
        }
    } else if (strcmp(argv[1], "scan") == 0) {
        CLI_Print("[WIFI] Scanning networks...\r\n");
        int n = WiFi.scanNetworks();
        if (n == 0) {
            CLI_Print("  No networks found.\r\n");
        } else {
            CLI_Printf("  Found %d networks:\r\n", n);
            for (int i = 0; i < n; ++i) {
                CLI_Printf("  [%2d] %-20s (RSSI: %2d dBm)\r\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
            }
        }
    } else if (strcmp(argv[1], "connect") == 0) {
        if (argc < 4) {
            CLI_Print("Usage: wifi connect <SSID> <PASSWORD>\r\n");
            return -1;
        }
        CLI_Printf("[WIFI] Connecting to '%s'...\r\n", argv[2]);
        WiFi.begin(argv[2], argv[3]);
        int timeout = 20;
        while (WiFi.status() != WL_CONNECTED && timeout > 0) {
            delay(500);
            CLI_Print(".");
            timeout--;
        }
        if (WiFi.status() == WL_CONNECTED) {
            CLI_Printf("\r\n[WIFI] Connected! Web Dashboard IP: http://%s\r\n", WiFi.localIP().toString().c_str());
        } else {
            CLI_Print("\r\n[WIFI] Connection failed!\r\n");
        }
    } else {
        CLI_Printf("Unknown option: '%s'\r\n", argv[1]);
        return -1;
    }
    return 0;
}

static int cli_cmd_led(int argc, char **argv)
{
    if (argc < 4) {
        CLI_Print("Usage: led <r> <g> <b>  (each 0-255)\r\n");
        CLI_Print("  ex)  led 255 0 0      -> Red\r\n");
        CLI_Print("  ex)  led 0 255 0      -> Green\r\n");
        CLI_Print("  ex)  led 0 0 255      -> Blue\r\n");
        return -1;
    }
    uint8_t r = (uint8_t)constrain(atoi(argv[1]), 0, 255);
    uint8_t g = (uint8_t)constrain(atoi(argv[2]), 0, 255);
    uint8_t b = (uint8_t)constrain(atoi(argv[3]), 0, 255);
    LED_SetColor(r, g, b);
    CLI_Printf("[LED] Color set -> R:%d G:%d B:%d\r\n", r, g, b);
    return 0;
}

static int cli_cmd_rgb(int argc, char **argv)
{
    if (argc < 2) {
        CLI_Print("Usage: rgb <rainbow|pulse|off>\r\n");
        return -1;
    }
    if (strcmp(argv[1], "rainbow") == 0) {
        LED_SetMode(0);
        CLI_Print("[LED] Mode -> Rainbow Cycle\r\n");
    } else if (strcmp(argv[1], "pulse") == 0) {
        LED_SetMode(1);
        CLI_Print("[LED] Mode -> Color Pulse\r\n");
    } else if (strcmp(argv[1], "off") == 0) {
        LED_SetMode(2);
        CLI_Print("[LED] Mode -> OFF\r\n");
    } else {
        CLI_Printf("Unknown mode: '%s' (Use: rainbow / pulse / off)\r\n", argv[1]);
        return -1;
    }
    return 0;
}

static int cli_cmd_bright(int argc, char **argv)
{
    if (argc < 2) {
        CLI_Print("Usage: bright <0-255>\r\n");
        return -1;
    }
    uint8_t val = (uint8_t)constrain(atoi(argv[1]), 0, 255);
    LED_SetBrightness(val);
    CLI_Printf("[LED] Brightness set -> %d\r\n", val);
    return 0;
}

static int cli_cmd_hello(int argc, char **argv)
{
    int count = 1;
    if (argc >= 2) count = constrain(atoi(argv[1]), 1, 20);
    for (int i = 0; i < count; i++) {
        CLI_Printf("Hello, World! - ESP32-S3  [#%d]\r\n", i + 1);
    }
    return 0;
}

static int cli_cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    CLI_Print("[SYS] Rebooting in 500ms...\r\n");
    Serial.flush();
    delay(500);
    esp_restart();
    return 0;
}

/* ════════════════════════════════════════════════════════════════
 *  CLI Core
 * ════════════════════════════════════════════════════════════════ */

bool CLI_RegisterCommand(const char *name, cli_cmd_func_t func, const char *help)
{
    if (cmd_count >= CLI_MAX_COMMANDS || name == NULL || func == NULL)
        return false;
    cmd_table[cmd_count].name = name;
    cmd_table[cmd_count].func = func;
    cmd_table[cmd_count].help = (help != NULL) ? help : "";
    cmd_count++;
    return true;
}

static void CLI_ExecuteLine(char *line)
{
    char *argv[CLI_MAX_ARGS];
    int   argc = 0;

    char *token = strtok(line, " \t\r\n");
    while (token != NULL && argc < CLI_MAX_ARGS) {
        argv[argc++] = token;
        token = strtok(NULL, " \t\r\n");
    }

    if (argc == 0) {
        CLI_Print(CLI_PROMPT);
        return;
    }

    bool found = false;
    for (uint8_t i = 0; i < cmd_count; i++) {
        if (strcmp(argv[0], cmd_table[i].name) == 0) {
            found = true;
            int res = cmd_table[i].func(argc, argv);
            if (res != 0)
                CLI_Printf("[CLI] '%s' returned error: %d\r\n", argv[0], res);
            break;
        }
    }

    if (!found)
        CLI_Printf("Unknown command: '%s'. Type 'help' for list.\r\n", argv[0]);

    CLI_Print(CLI_PROMPT);
}

void CLI_Init(void)
{
    cmd_count      = 0;
    rx_head        = 0;
    rx_tail        = 0;
    rx_line_index  = 0;
    memset(rx_line_buf, 0, sizeof(rx_line_buf));

    /* Register built-in commands */
    CLI_RegisterCommand("help",    cli_cmd_help,    "List all available CLI commands");
    CLI_RegisterCommand("sysinfo", cli_cmd_sysinfo, "ESP32-S3 clock / memory / status");
    CLI_RegisterCommand("top",     cli_cmd_top,     "Display CPU / Heap / PSRAM / Temp stats");
    CLI_RegisterCommand("led",     cli_cmd_led,     "Set RGB color directly (Usage: led <r> <g> <b>)");
    CLI_RegisterCommand("rgb",     cli_cmd_rgb,     "Set LED mode (Usage: rgb rainbow|pulse|off)");
    CLI_RegisterCommand("bright",  cli_cmd_bright,  "Set LED brightness (Usage: bright 0-255)");
    CLI_RegisterCommand("wifi",    cli_cmd_wifi,    "Wi-Fi status, scan, connect");
    CLI_RegisterCommand("hello",   cli_cmd_hello,   "Print Hello World (Usage: hello [count])");
    CLI_RegisterCommand("reboot",  cli_cmd_reboot,  "Software reset the MCU");

    /* ── BLE Nordic UART Service Setup ── */
    BLEDevice::init(BLE_DEVICE_NAME);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);

    pTxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_TX,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_RX,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    pRxCharacteristic->setCallbacks(new RxCallbacks());

    pService->start();

    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();

    Serial.print(F("\r\n=======================================================\r\n"));
    Serial.printf("[CLI] ESP32-S3 Dual-Shell (USB + BLE NUS) Ready!\r\n");
    Serial.printf("[BLE] Device Name : %s\r\n", BLE_DEVICE_NAME);
    Serial.printf("[BLE] Service UUID: %s\r\n", SERVICE_UUID);
    Serial.print(F("[CLI] Type 'help' and press Enter to view commands.\r\n"));
    Serial.print(F("=======================================================\r\n"));
    Serial.print(F(CLI_PROMPT));
}

void CLI_Process(void)
{
    // BLE 재접속 관리
    if (!deviceConnected && oldDeviceConnected) {
        delay(500); // Bluetooth stack give time to get ready
        pServer->startAdvertising();
        oldDeviceConnected = deviceConnected;
    }
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
        CLI_Print("\r\n[BLE] Client Connected!\r\n");
        CLI_Print(CLI_PROMPT);
    }

    /* ── 1. USB Serial 입력 수신 ── */
    while (Serial.available()) {
        CLI_PutRxChar((char)Serial.read());
    }

    /* ── 2. 통합 큐(Ring buffer) 처리 ── */
    while (rx_tail != rx_head) {
        char ch = rx_ring[rx_tail];
        rx_tail = (rx_tail + 1) % CLI_RX_RING_SIZE;

        if (ch == '\r' || ch == '\n') {
            CLI_Print("\r\n");
            rx_line_buf[rx_line_index] = '\0';
            CLI_ExecuteLine(rx_line_buf);
            rx_line_index = 0;
            memset(rx_line_buf, 0, sizeof(rx_line_buf));
        }
        else if (ch == '\b' || ch == 0x7F) {   /* Backspace */
            if (rx_line_index > 0) {
                rx_line_index--;
                rx_line_buf[rx_line_index] = '\0';
                CLI_Print("\b \b");
            }
        }
        else if (rx_line_index < (CLI_MAX_CMD_LEN - 1)) {
            rx_line_buf[rx_line_index++] = ch;
            char echo[2] = {ch, '\0'};
            CLI_Print(echo); /* Echo back to both USB and BLE */
        }
    }
}

String CLI_ExecuteWebCommand(const char *cmd)
{
    if (cmd == NULL || strlen(cmd) == 0) return "";
    
    char temp_buf[CLI_MAX_CMD_LEN];
    strncpy(temp_buf, cmd, sizeof(temp_buf) - 1);
    temp_buf[sizeof(temp_buf) - 1] = '\0';

    g_captured_text = "";
    g_capture_output = true;
    
    CLI_ExecuteLine(temp_buf);
    
    g_capture_output = false;
    String result = g_captured_text;
    g_captured_text = "";
    return result;
}

