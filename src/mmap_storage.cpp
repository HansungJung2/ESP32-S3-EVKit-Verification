/**
 * @file mmap_storage.cpp
 * @brief 128KB Flash MMAP implementation for ESP32-S3
 */

#include "mmap_storage.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "spi_flash_mmap.h"
#include <string.h>

static const esp_partition_t *g_part = nullptr;
static const void *g_mapped_ptr = nullptr;
static spi_flash_mmap_handle_t g_mmap_handle = 0;
static bool g_initialized = false;

bool MMAP_Init(void)
{
    if (g_initialized) return true;

    // 파티션 테이블에서 "mmap_store" 파티션 검색
    g_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "mmap_store");
    if (g_part == nullptr) {
        Serial.println(F("[MMAP] Error: Partition 'mmap_store' not found!"));
        return false;
    }

    // 128KB 영역을 CPU 가상 주소 공간에 하드웨어 MMU 매핑
    esp_err_t err = esp_partition_mmap(
        g_part,
        0,                      // offset
        MMAP_STORAGE_SIZE,      // 128KB
        ESP_PARTITION_MMAP_DATA,// Data 읽기용 매핑
        &g_mapped_ptr,          // 매핑된 CPU 가상 주소 포인터 반환
        &g_mmap_handle
    );

    if (err != ESP_OK || g_mapped_ptr == nullptr) {
        Serial.printf("[MMAP] Error: esp_partition_mmap failed (err: 0x%X)\r\n", err);
        return false;
    }

    g_initialized = true;

    const mmap_nv_header_t *hdr = (const mmap_nv_header_t *)g_mapped_ptr;
    Serial.printf("[MMAP] Initialized! Physical: 0x%08lX -> Virtual: %p (Size: 128KB)\r\n",
                  (unsigned long)g_part->address, g_mapped_ptr);

    // 최초 사용 시(헤더 매직 넘버가 없을 때) 초기화
    if (hdr->magic != MMAP_MAGIC_HEADER) {
        Serial.println(F("[MMAP] Unformatted Flash detected! Formatting first 4KB sector..."));
        mmap_nv_header_t init_hdr;
        memset(&init_hdr, 0, sizeof(init_hdr));
        init_hdr.magic = MMAP_MAGIC_HEADER;
        init_hdr.boot_count = 1;
        init_hdr.write_count = 1;
        strncpy(init_hdr.device_tag, "ESP32-S3-N16R8-DEFAULT", sizeof(init_hdr.device_tag) - 1);
        strncpy(init_hdr.user_memo, "Hello from 128KB Flash MMAP Storage!", sizeof(init_hdr.user_memo) - 1);

        // 4KB 섹터 Erase 후 쓰기
        esp_partition_erase_range(g_part, 0, 4096);
        esp_partition_write(g_part, 0, &init_hdr, sizeof(init_hdr));
    } else {
        MMAP_IncrementBootCount();
    }

    return true;
}

const mmap_nv_header_t* MMAP_GetHeader(void)
{
    if (!g_initialized && !MMAP_Init()) return nullptr;
    return (const mmap_nv_header_t *)g_mapped_ptr;
}

const uint8_t* MMAP_GetRawPointer(void)
{
    if (!g_initialized && !MMAP_Init()) return nullptr;
    return (const uint8_t *)g_mapped_ptr;
}

uint32_t MMAP_GetVirtualAddress(void)
{
    return (uint32_t)g_mapped_ptr;
}

uint32_t MMAP_GetPhysicalAddress(void)
{
    return g_part ? g_part->address : 0;
}

void MMAP_IncrementBootCount(void)
{
    if (!g_initialized && !MMAP_Init()) return;
    const mmap_nv_header_t *hdr = (const mmap_nv_header_t *)g_mapped_ptr;

    mmap_nv_header_t copy;
    memcpy(&copy, hdr, sizeof(copy));
    copy.boot_count++;

    // 4KB 섹터 안전 갱신
    esp_partition_erase_range(g_part, 0, 4096);
    esp_partition_write(g_part, 0, &copy, sizeof(copy));
}

bool MMAP_SaveMemo(const char *memo)
{
    if (!g_initialized && !MMAP_Init()) return false;
    if (memo == nullptr) return false;

    const mmap_nv_header_t *hdr = (const mmap_nv_header_t *)g_mapped_ptr;
    mmap_nv_header_t copy;
    memcpy(&copy, hdr, sizeof(copy));

    memset(copy.user_memo, 0, sizeof(copy.user_memo));
    strncpy(copy.user_memo, memo, sizeof(copy.user_memo) - 1);
    copy.write_count++;

    esp_err_t err = esp_partition_erase_range(g_part, 0, 4096);
    if (err != ESP_OK) return false;

    err = esp_partition_write(g_part, 0, &copy, sizeof(copy));
    return (err == ESP_OK);
}

bool MMAP_SetDeviceTag(const char *tag)
{
    if (!g_initialized && !MMAP_Init()) return false;
    if (tag == nullptr) return false;

    const mmap_nv_header_t *hdr = (const mmap_nv_header_t *)g_mapped_ptr;
    mmap_nv_header_t copy;
    memcpy(&copy, hdr, sizeof(copy));

    memset(copy.device_tag, 0, sizeof(copy.device_tag));
    strncpy(copy.device_tag, tag, sizeof(copy.device_tag) - 1);
    copy.write_count++;

    esp_err_t err = esp_partition_erase_range(g_part, 0, 4096);
    if (err != ESP_OK) return false;

    err = esp_partition_write(g_part, 0, &copy, sizeof(copy));
    return (err == ESP_OK);
}

void MMAP_HexDump(uint32_t offset, size_t length, char *out_buf, size_t buf_size)
{
    if (!g_initialized && !MMAP_Init()) {
        snprintf(out_buf, buf_size, "MMAP not initialized.");
        return;
    }
    if (offset + length > MMAP_STORAGE_SIZE) {
        length = MMAP_STORAGE_SIZE - offset;
    }

    const uint8_t *ptr = (const uint8_t *)g_mapped_ptr + offset;
    size_t written = 0;

    for (size_t i = 0; i < length && written < buf_size - 64; i += 16) {
        written += snprintf(out_buf + written, buf_size - written,
                            "%04lX: ", (unsigned long)(offset + i));

        // Hex bytes
        for (size_t j = 0; j < 16; j++) {
            if (i + j < length) {
                written += snprintf(out_buf + written, buf_size - written, "%02X ", ptr[i + j]);
            } else {
                written += snprintf(out_buf + written, buf_size - written, "   ");
            }
        }
        written += snprintf(out_buf + written, buf_size - written, " |");

        // ASCII chars
        for (size_t j = 0; j < 16 && (i + j) < length; j++) {
            char c = ptr[i + j];
            written += snprintf(out_buf + written, buf_size - written, "%c", (c >= 32 && c <= 126) ? c : '.');
        }
        written += snprintf(out_buf + written, buf_size - written, "|\r\n");
    }
}

