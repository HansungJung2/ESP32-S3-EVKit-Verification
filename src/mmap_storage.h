/**
 * @file mmap_storage.h
 * @brief 128KB Flash Non-Volatile Memory via Hardware MMU (mmap) on Zephyr RTOS
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MMAP_STORAGE_PHYS_ADDR 0x00F10000
#define MMAP_STORAGE_SIZE      (128 * 1024) // 128 KBytes = 131,072 Bytes
#define MMAP_MAGIC_HEADER      0x53334D4D   // "S3MM" (ESP32-S3 MMAP)

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;           // 0x53334D4D
    uint32_t boot_count;      // Non-volatile boot counter
    char     device_tag[32];  // Device tag
    char     user_memo[256];  // User non-volatile memo
    uint32_t write_count;     // Flash write counter
    uint8_t  reserved[176];   // 512-byte aligned header
} mmap_nv_header_t;
#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif

bool                    mmap_storage_init(void);
const mmap_nv_header_t* mmap_storage_get_header(void);
const uint8_t*          mmap_storage_get_raw_ptr(void);
uint32_t                mmap_storage_get_virt_addr(void);
uint32_t                mmap_storage_get_phys_addr(void);

bool mmap_storage_save_memo(const char *memo);
bool mmap_storage_set_device_tag(const char *tag);
void mmap_storage_increment_boot_count(void);
void mmap_storage_hex_dump(uint32_t offset, size_t length, char *out_buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
