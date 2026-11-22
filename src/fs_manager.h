/**
 * @file fs_manager.h
 * @brief 7MB LittleFS File System Manager for ESP32-S3
 */

#pragma once

#include <Arduino.h>
#include <LittleFS.h>

bool   FS_Init(void);
size_t FS_GetTotalBytes(void);
size_t FS_GetUsedBytes(void);
String FS_GetFileListJSON(void);
bool   FS_Format(void);
bool   FS_FileExists(const char *path);
bool   FS_DeleteFile(const char *path);
bool   FS_WriteFile(const char *path, const char *data);
String FS_ReadFile(const char *path);
void   FS_CreateSampleFiles(void);

