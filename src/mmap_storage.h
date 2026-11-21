/**
 * @file mmap_storage.h
 * @brief 128KB Flash Non-Volatile Memory via Hardware MMU (mmap)
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>

#define MMAP_STORAGE_SIZE     (128 * 1024) // 128 KBytes = 131,072 Bytes
#define MMAP_MAGIC_HEADER     0x53334D4D   // "S3MM" (ESP32-S3 MMAP)

// ── 128KB 플래시 메모리 매핑 구조체 ────────────────────────────
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;           // 0x53334D4D
    uint32_t boot_count;      // 비휘발성 부팅 카운터
    char     device_tag[32];  // 기기 식별 태그
    char     user_memo[256];  // 사용자 비휘발성 저장 메모
    uint32_t write_count;     // 플래시 쓰기 횟수
    uint8_t  reserved[176];   // 512바이트 헤더 정렬용 여유 공간
} mmap_nv_header_t;
#pragma pack(pop)

// ── Public API ───────────────────────────────────────────────
bool     MMAP_Init(void);
const mmap_nv_header_t* MMAP_GetHeader(void);
const uint8_t*          MMAP_GetRawPointer(void);
uint32_t                MMAP_GetVirtualAddress(void);
uint32_t                MMAP_GetPhysicalAddress(void);

bool     MMAP_SaveMemo(const char *memo);
bool     MMAP_SetDeviceTag(const char *tag);
void     MMAP_IncrementBootCount(void);
void     MMAP_HexDump(uint32_t offset, size_t length, char *out_buf, size_t buf_size);

