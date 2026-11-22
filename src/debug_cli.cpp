/**
 * @file debug_cli.cpp
 * @brief Dual-Interface (USB Serial + BLE UART) CLI Shell for ESP32-S3
 *        Supports standard Nordic UART Service (NUS)
 */

#include "debug_cli.h"
#include "mmap_storage.h"
#include "fs_manager.h"
#include <esp_ota_ops.h>
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

/* ── PSRAM Test Buffer Management ────────────────────────────── */
#define MAX_PSRAM_BLOCKS 8
static void* g_psram_blocks[MAX_PSRAM_BLOCKS] = {nullptr};
static size_t g_psram_block_sizes[MAX_PSRAM_BLOCKS] = {0};
static int g_psram_block_count = 0;

bool PSRAM_AllocMB(int mb)
{
    if (mb <= 0 || g_psram_block_count >= MAX_PSRAM_BLOCKS) return false;
    size_t bytes = (size_t)mb * 1024 * 1024;
    void *ptr = ps_malloc(bytes);
    if (ptr == nullptr) {
        return false;
    }
    // 메모리 채우기 및 무결성 검증 패턴 기록
    memset(ptr, 0xAA, bytes);
    g_psram_blocks[g_psram_block_count] = ptr;
    g_psram_block_sizes[g_psram_block_count] = bytes;
    g_psram_block_count++;
    return true;
}

void PSRAM_FreeAll(void)
{
    for (int i = 0; i < g_psram_block_count; i++) {
        if (g_psram_blocks[i] != nullptr) {
            free(g_psram_blocks[i]);
            g_psram_blocks[i] = nullptr;
            g_psram_block_sizes[i] = 0;
        }
    }
    g_psram_block_count = 0;
}

size_t PSRAM_GetAllocatedBytes(void)
{
    size_t total = 0;
    for (int i = 0; i < g_psram_block_count; i++) {
        total += g_psram_block_sizes[i];
    }
    return total;
}

static int cli_cmd_psram(int argc, char **argv)
{
    if (argc < 2) {
        CLI_Print("Usage: psram <status|alloc|free|speed>\r\n");
        CLI_Print("  psram status       - Show 8MB PSRAM free/used\r\n");
        CLI_Print("  psram alloc <MB>   - Allocate N megabytes (e.g. psram alloc 2)\r\n");
        CLI_Print("  psram free         - Release all allocated test buffers\r\n");
        CLI_Print("  psram speed        - Run Read/Write throughput benchmark\r\n");
        return -1;
    }

    if (strcmp(argv[1], "status") == 0) {
        uint32_t total = ESP.getPsramSize();
        uint32_t freeP = ESP.getFreePsram();
        uint32_t used  = total - freeP;
        float pct = total ? ((float)used / total) * 100.0f : 0.0f;
        CLI_Printf("[PSRAM] Total: %lu bytes (%.2f MB)\r\n", (unsigned long)total, (float)total / (1024*1024));
        CLI_Printf("[PSRAM] Used : %lu bytes (%.2f MB, %.1f%%)\r\n", (unsigned long)used, (float)used / (1024*1024), pct);
        CLI_Printf("[PSRAM] Free : %lu bytes (%.2f MB)\r\n", (unsigned long)freeP, (float)freeP / (1024*1024));
        CLI_Printf("[PSRAM] Custom Test Allocated: %u MB (%d blocks)\r\n", (unsigned int)(PSRAM_GetAllocatedBytes() / (1024*1024)), g_psram_block_count);
    }
    else if (strcmp(argv[1], "alloc") == 0) {
        if (argc < 3) {
            CLI_Print("Usage: psram alloc <MB>  (1 to 6)\r\n");
            return -1;
        }
        int mb = atoi(argv[2]);
        if (mb <= 0 || mb > 6) {
            CLI_Print("Error: MB must be between 1 and 6\r\n");
            return -1;
        }
        CLI_Printf("[PSRAM] Allocating %d MB...\r\n", mb);
        if (PSRAM_AllocMB(mb)) {
            CLI_Printf("[PSRAM] Successfully allocated %d MB in Octal PSRAM!\r\n", mb);
            CLI_Printf("[PSRAM] Current Total Test Buffer: %u MB\r\n", (unsigned int)(PSRAM_GetAllocatedBytes() / (1024*1024)));
        } else {
            CLI_Print("[PSRAM] Error: Memory allocation failed!\r\n");
            return -1;
        }
    }
    else if (strcmp(argv[1], "free") == 0) {
        PSRAM_FreeAll();
        CLI_Print("[PSRAM] All test buffers released!\r\n");
    }
    else if (strcmp(argv[1], "speed") == 0) {
        CLI_Print("[PSRAM] Benchmark: Allocating 1MB for Read/Write test...\r\n");
        const size_t benchSize = 1024 * 1024; // 1MB
        uint32_t *p = (uint32_t *)ps_malloc(benchSize);
        if (!p) {
            CLI_Print("[PSRAM] Alloc failed for benchmark!\r\n");
            return -1;
        }
        uint32_t words = benchSize / sizeof(uint32_t);

        // Write test
        uint32_t t0 = micros();
        for (uint32_t i = 0; i < words; i++) {
            p[i] = 0xA5A50000 + i;
        }
        uint32_t writeUs = micros() - t0;

        // Read & verify test
        t0 = micros();
        bool ok = true;
        for (uint32_t i = 0; i < words; i++) {
            if (p[i] != (0xA5A50000 + i)) {
                ok = false;
                break;
            }
        }
        uint32_t readUs = micros() - t0;
        free(p);

        float writeMBs = (float)benchSize / (writeUs ? writeUs : 1);
        float readMBs  = (float)benchSize / (readUs ? readUs : 1);

        CLI_Printf("[PSRAM] Write Speed: %.2f MB/s (%lu us)\r\n", writeMBs, (unsigned long)writeUs);
        CLI_Printf("[PSRAM] Read Speed : %.2f MB/s (%lu us)\r\n", readMBs, (unsigned long)readUs);
        CLI_Printf("[PSRAM] Integrity Check: %s\r\n", ok ? "PASSED (OK)" : "FAILED (DATA MISMATCH)");
    }
    else {
        CLI_Printf("Unknown option: '%s'\r\n", argv[1]);
        return -1;
    }
    return 0;
}
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

    uint32_t totalFlash = ESP.getFlashChipSize();
    uint32_t appFlash   = ESP.getSketchSize();
    float flashUsage = totalFlash ? ((float)appFlash / totalFlash) * 100.0f : 0.0f;

    float tempC = temperatureRead();

    CLI_Print("\r\n================ ESP32-S3 SYSTEM STATS ================\r\n");
    CLI_Printf("  CPU Frequency : %lu MHz\r\n", (unsigned long)getCpuFrequencyMhz());
    CLI_Printf("  Chip Temp     : %.1f °C\r\n", tempC);
    CLI_Printf("  16MB SPI Flash: %.2f / %.2f MB (%.1f%% used by app)\r\n", (float)appFlash / (1024*1024), (float)totalFlash / (1024*1024), flashUsage);
    CLI_Printf("  8MB OPI PSRAM : %lu / %lu bytes (%.1f%% used)\r\n", (unsigned long)usedPsram, (unsigned long)totalPsram, psramUsage);
    CLI_Printf("  Internal Heap : %lu / %lu bytes (%.1f%% used)\r\n", (unsigned long)usedHeap, (unsigned long)totalHeap, heapUsage);
    if (WiFi.status() == WL_CONNECTED) {
        CLI_Printf("  Wi-Fi RSSI    : %d dBm (SSID: %s)\r\n", WiFi.RSSI(), WiFi.SSID().c_str());
    }
    CLI_Printf("  Uptime        : %lu sec\r\n", (unsigned long)(millis() / 1000));
    CLI_Print("=======================================================\r\n");
    return 0;
}

static int cli_cmd_mmap(int argc, char **argv)
{
    if (argc < 2) {
        CLI_Print("Usage: mmap <status|write|tag|dump>\r\n");
        CLI_Print("  mmap status          - View MMAP pointer, boot count & memo\r\n");
        CLI_Print("  mmap write <text>    - Save non-volatile string to 128KB Flash\r\n");
        CLI_Print("  mmap tag <dev_name>  - Save device tag string\r\n");
        CLI_Print("  mmap dump [len]      - Hex dump of 128KB flash storage\r\n");
        return -1;
    }

    if (strcmp(argv[1], "status") == 0) {
        const mmap_nv_header_t *hdr = MMAP_GetHeader();
        if (!hdr) {
            CLI_Print("[MMAP] Error: Not initialized\r\n");
            return -1;
        }
        CLI_Print("\r\n================ 128KB FLASH MMAP STORAGE ================\r\n");
        CLI_Printf("  Physical Flash Addr : 0x%08lX (Offset in 16MB)\r\n", (unsigned long)MMAP_GetPhysicalAddress());
        CLI_Printf("  Virtual CPU Pointer : %p (MMU Mapped)\r\n", (void*)MMAP_GetVirtualAddress());
        CLI_Printf("  Partition Size      : %u KB (131,072 Bytes)\r\n", (unsigned int)(MMAP_STORAGE_SIZE / 1024));
        CLI_Printf("  Non-Volatile Boots  : %lu times\r\n", (unsigned long)hdr->boot_count);
        CLI_Printf("  Flash Write Count   : %lu times\r\n", (unsigned long)hdr->write_count);
        CLI_Printf("  Device Tag          : %s\r\n", hdr->device_tag);
        CLI_Printf("  Saved User Memo     : %s\r\n", hdr->user_memo);
        CLI_Print("==========================================================\r\n");
    }
    else if (strcmp(argv[1], "write") == 0) {
        if (argc < 3) {
            CLI_Print("Usage: mmap write <text to save in flash>\r\n");
            return -1;
        }
        // 공백 포함 전체 텍스트 조합
        char memo_buf[256] = {0};
        for (int i = 2; i < argc; i++) {
            strncat(memo_buf, argv[i], sizeof(memo_buf) - strlen(memo_buf) - 2);
            if (i < argc - 1) strncat(memo_buf, " ", sizeof(memo_buf) - strlen(memo_buf) - 1);
        }
        if (MMAP_SaveMemo(memo_buf)) {
            CLI_Printf("[MMAP] Successfully written to 128KB Flash!\r\n");
            CLI_Printf("[MMAP] Verified via mmap pointer: \"%s\"\r\n", MMAP_GetHeader()->user_memo);
        } else {
            CLI_Print("[MMAP] Error: Flash write failed!\r\n");
            return -1;
        }
    }
    else if (strcmp(argv[1], "tag") == 0) {
        if (argc < 3) {
            CLI_Print("Usage: mmap tag <device_name>\r\n");
            return -1;
        }
        if (MMAP_SetDeviceTag(argv[2])) {
            CLI_Printf("[MMAP] Device tag updated: %s\r\n", argv[2]);
        } else {
            CLI_Print("[MMAP] Error: Failed to update tag\r\n");
            return -1;
        }
    }
    else if (strcmp(argv[1], "dump") == 0) {
        size_t len = 64;
        if (argc >= 3) len = constrain(atoi(argv[2]), 16, 256);
        char dump_buf[1024];
        MMAP_HexDump(0, len, dump_buf, sizeof(dump_buf));
        CLI_Print("\r\n--- 128KB MMAP Storage Hex Dump ---\r\n");
        CLI_Print(dump_buf);
    }
    else {
        CLI_Printf("Unknown option: '%s'\r\n", argv[1]);
        return -1;
    }
    return 0;
}

static int cli_cmd_fs(int argc, char **argv)
{
    if (argc < 2) {
        CLI_Print("Usage:\r\n");
        CLI_Print("  fs list               - List files in LittleFS\r\n");
        CLI_Print("  fs cat <file>         - Display file content\r\n");
        CLI_Print("  fs rm <file>          - Delete file\r\n");
        CLI_Print("  fs write <file> <str> - Write string to file\r\n");
        CLI_Print("  fs sample             - Create default sample files\r\n");
        CLI_Print("  fs format             - Format 7MB LittleFS partition\r\n");
        return -1;
    }

    if (strcmp(argv[1], "list") == 0) {
        size_t total = FS_GetTotalBytes();
        size_t used = FS_GetUsedBytes();
        CLI_Printf("\r\n[LittleFS] Total: %lu bytes (%.2f MB), Used: %lu bytes (%.2f KB)\r\n",
                   (unsigned long)total, (float)total / (1024*1024),
                   (unsigned long)used, (float)used / 1024.0f);
        CLI_Print("--------------------------------------------------\r\n");
        CLI_Print(" File Name                       Size (Bytes)\r\n");
        CLI_Print("--------------------------------------------------\r\n");
        File root = LittleFS.open("/");
        if (!root || !root.isDirectory()) {
            CLI_Print("  (Failed to open root directory)\r\n");
            return -1;
        }
        File file = root.openNextFile();
        int count = 0;
        while (file) {
            if (!file.isDirectory()) {
                String fname = file.name();
                if (fname.startsWith("/")) fname = fname.substring(1);
                CLI_Printf("  %-30s %10lu B\r\n", fname.c_str(), (unsigned long)file.size());
                count++;
            }
            file = root.openNextFile();
        }
        if (count == 0) {
            CLI_Print("  (No files found)\r\n");
        }
        CLI_Print("--------------------------------------------------\r\n");
    } else if (strcmp(argv[1], "cat") == 0) {
        if (argc < 3) {
            CLI_Print("Usage: fs cat <filepath>\r\n");
            return -1;
        }
        String path = argv[2];
        if (!path.startsWith("/")) path = "/" + path;
        if (!FS_FileExists(path.c_str())) {
            CLI_Printf("File '%s' not found!\r\n", path.c_str());
            return -1;
        }
        String content = FS_ReadFile(path.c_str());
        CLI_Printf("\r\n--- [Content: %s (%d bytes)] ---\r\n", path.c_str(), content.length());
        CLI_Print(content.c_str());
        CLI_Print("\r\n--- [End of File] ---\r\n");
    } else if (strcmp(argv[1], "rm") == 0) {
        if (argc < 3) {
            CLI_Print("Usage: fs rm <filepath>\r\n");
            return -1;
        }
        String path = argv[2];
        if (!path.startsWith("/")) path = "/" + path;
        if (FS_DeleteFile(path.c_str())) {
            CLI_Printf("[LittleFS] File '%s' deleted successfully.\r\n", path.c_str());
        } else {
            CLI_Printf("[LittleFS] Failed to delete '%s' (not found or error).\r\n", path.c_str());
        }
    } else if (strcmp(argv[1], "write") == 0) {
        if (argc < 4) {
            CLI_Print("Usage: fs write <filepath> <text>\r\n");
            return -1;
        }
        String path = argv[2];
        if (!path.startsWith("/")) path = "/" + path;
        String data = "";
        for (int i = 3; i < argc; i++) {
            data += argv[i];
            if (i < argc - 1) data += " ";
        }
        data += "\r\n";
        if (FS_WriteFile(path.c_str(), data.c_str())) {
            CLI_Printf("[LittleFS] Successfully wrote %d bytes to '%s'\r\n", data.length(), path.c_str());
        } else {
            CLI_Printf("[LittleFS] Error writing to '%s'\r\n", path.c_str());
        }
    } else if (strcmp(argv[1], "sample") == 0) {
        FS_CreateSampleFiles();
        CLI_Print("[LittleFS] Default sample files created!\r\n");
    } else if (strcmp(argv[1], "format") == 0) {
        CLI_Print("[LittleFS] Formatting 7MB partition, please wait...\r\n");
        if (FS_Format()) {
            CLI_Print("[LittleFS] Format completed successfully!\r\n");
        } else {
            CLI_Print("[LittleFS] Format failed!\r\n");
        }
    } else {
        CLI_Printf("Unknown option: '%s'\r\n", argv[1]);
        return -1;
    }
    return 0;
}

static int cli_cmd_ota(int argc, char **argv)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *boot    = esp_ota_get_boot_partition();

    CLI_Print("\r\n=== ESP32-S3 Dual OTA Status ===\r\n");
    if (running) {
        CLI_Printf(" Running App Partition : %s (Offset: 0x%08X, Size: %lu KB)\r\n",
                   running->label, running->address, (unsigned long)(running->size / 1024));
    }
    if (next) {
        CLI_Printf(" Next Target Partition : %s (Offset: 0x%08X, Size: %lu KB)\r\n",
                   next->label, next->address, (unsigned long)(next->size / 1024));
    }
    if (boot) {
        CLI_Printf(" Boot Partition        : %s (Offset: 0x%08X)\r\n", boot->label, boot->address);
    }
    CLI_Printf(" Sketch Binary Size    : %lu bytes\r\n", (unsigned long)ESP.getSketchSize());
    CLI_Printf(" Free Sketch Space     : %lu bytes\r\n", (unsigned long)ESP.getFreeSketchSpace());
    CLI_Print(" Web OTA Upload URL    : http://192.168.4.1 (OTA Card)\r\n");
    CLI_Print("================================\r\n");
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
    CLI_RegisterCommand("psram",   cli_cmd_psram,   "8MB PSRAM test (status, alloc <MB>, free, speed)");
    CLI_RegisterCommand("mmap",    cli_cmd_mmap,    "128KB Flash MMAP NV-Storage (status, write, tag, dump)");
    CLI_RegisterCommand("fs",      cli_cmd_fs,      "7MB LittleFS (list, cat, rm, write, sample, format)");
    CLI_RegisterCommand("ota",     cli_cmd_ota,     "Dual OTA partition status and sketch size");
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

