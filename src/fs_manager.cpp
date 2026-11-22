/**
 * @file fs_manager.cpp
 * @brief 7MB LittleFS File System Manager implementation
 */

#include "fs_manager.h"

static bool g_fs_mounted = false;

bool FS_Init(void)
{
    if (g_fs_mounted) return true;

    // true: 마운트 실패 시 자동 포맷
    if (!LittleFS.begin(true)) {
        Serial.println(F("[LittleFS] Mount Failed!"));
        return false;
    }

    g_fs_mounted = true;
    Serial.printf("[LittleFS] Mounted successfully! Total: %lu bytes, Used: %lu bytes\r\n",
                  (unsigned long)LittleFS.totalBytes(),
                  (unsigned long)LittleFS.usedBytes());

    // 기본 파일이 없으면 샘플 파일 생성
    if (!LittleFS.exists("/config.json")) {
        FS_CreateSampleFiles();
    }

    return true;
}

size_t FS_GetTotalBytes(void)
{
    if (!g_fs_mounted) return 0;
    return LittleFS.totalBytes();
}

size_t FS_GetUsedBytes(void)
{
    if (!g_fs_mounted) return 0;
    return LittleFS.usedBytes();
}

String FS_GetFileListJSON(void)
{
    if (!g_fs_mounted) return "[]";

    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) {
        return "[]";
    }

    String json = "[";
    bool first = true;

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            if (!first) json += ",";
            first = false;
            json += "{\"name\":\"";
            String fname = file.name();
            if (fname.startsWith("/")) fname = fname.substring(1);
            json += fname;
            json += "\",\"size\":";
            json += String(file.size());
            json += "}";
        }
        file = root.openNextFile();
    }
    json += "]";
    return json;
}

bool FS_Format(void)
{
    if (!g_fs_mounted) return false;
    Serial.println(F("[LittleFS] Formatting filesystem..."));
    bool res = LittleFS.format();
    if (res) {
        Serial.println(F("[LittleFS] Format completed successfully!"));
        FS_CreateSampleFiles();
    } else {
        Serial.println(F("[LittleFS] Format failed!"));
    }
    return res;
}

bool FS_FileExists(const char *path)
{
    if (!g_fs_mounted) return false;
    return LittleFS.exists(path);
}

bool FS_DeleteFile(const char *path)
{
    if (!g_fs_mounted) return false;
    return LittleFS.remove(path);
}

bool FS_WriteFile(const char *path, const char *data)
{
    if (!g_fs_mounted) return false;
    File f = LittleFS.open(path, "w");
    if (!f) return false;
    f.print(data);
    f.close();
    return true;
}

String FS_ReadFile(const char *path)
{
    if (!g_fs_mounted) return "";
    File f = LittleFS.open(path, "r");
    if (!f) return "";
    String content = f.readString();
    f.close();
    return content;
}

void FS_CreateSampleFiles(void)
{
    Serial.println(F("[LittleFS] Generating default sample files..."));

    // 1. config.json
    const char *cfg = "{\r\n"
                      "  \"device\": \"ESP32-S3-EVKit-Verification\",\r\n"
                      "  \"hardware\": \"ESP32-S3-WROOM-1 (N16R8)\",\r\n"
                      "  \"flash_total_mb\": 16,\r\n"
                      "  \"psram_total_mb\": 8,\r\n"
                      "  \"filesystem\": \"LittleFS 7MB\",\r\n"
                      "  \"ota_support\": true,\r\n"
                      "  \"version\": \"1.0.0\"\r\n"
                      "}\r\n";
    FS_WriteFile("/config.json", cfg);

    // 2. sensor_log.csv
    const char *csv = "Timestamp,Uptime_sec,Chip_Temp_C,Heap_Free_KB,PSRAM_Free_MB\r\n"
                      "2026-11-22 11:30:00,10,42.5,285,7.92\r\n"
                      "2026-11-22 11:30:05,15,42.7,284,7.92\r\n"
                      "2026-11-22 11:30:10,20,42.6,284,7.92\r\n"
                      "2026-11-22 11:30:15,25,42.8,283,7.92\r\n"
                      "2026-11-22 11:30:20,30,42.5,283,7.92\r\n";
    FS_WriteFile("/sensor_log.csv", csv);

    // 3. readme.txt
    const char *txt = "=== ESP32-S3 LittleFS 7MB Storage ===\r\n"
                      "This file system is stored directly in the on-board 16MB Flash memory.\r\n"
                      "You can download, view, delete, or upload .txt, .json, and .csv files\r\n"
                      "wirelessly using the Web Dashboard (http://192.168.4.1) or the CLI shell.\r\n";
    FS_WriteFile("/readme.txt", txt);

    Serial.println(F("[LittleFS] Sample files created: /config.json, /sensor_log.csv, /readme.txt"));
}

