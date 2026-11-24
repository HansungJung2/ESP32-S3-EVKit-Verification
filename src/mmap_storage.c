/**
 * @file mmap_storage.c
 * @brief 128KB Flash MMAP implementation for ESP32-S3 under Zephyr RTOS
 */

#include "mmap_storage.h"
#include <esp_flash.h>
#include <spi_flash_mmap.h>
#include <string.h>
#include <stdio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(mmap_store, LOG_LEVEL_INF);

static const void *g_mapped_ptr = NULL;
static spi_flash_mmap_handle_t g_mmap_handle = 0;
static bool g_initialized = false;

bool mmap_storage_init(void)
{
    if (g_initialized) return true;

    /* Map 128KB physical flash at 0x00F10000 into CPU virtual address space via hardware MMU */
    esp_err_t err = spi_flash_mmap(
        MMAP_STORAGE_PHYS_ADDR,
        MMAP_STORAGE_SIZE,
        SPI_FLASH_MMAP_DATA,
        &g_mapped_ptr,
        &g_mmap_handle
    );

    if (err != ESP_OK || g_mapped_ptr == NULL) {
        LOG_ERR("spi_flash_mmap failed at 0x%08X (err: 0x%X)", MMAP_STORAGE_PHYS_ADDR, err);
        return false;
    }

    g_initialized = true;
    const mmap_nv_header_t *hdr = (const mmap_nv_header_t *)g_mapped_ptr;
    LOG_INF("MMU MMAP Initialized! Physical: 0x%08X -> Virtual: %p (Size: 128KB)",
            MMAP_STORAGE_PHYS_ADDR, g_mapped_ptr);

    if (hdr->magic != MMAP_MAGIC_HEADER) {
        LOG_WRN("Unformatted MMAP Flash detected! Formatting 4KB sector...");
        mmap_nv_header_t init_hdr;
        memset(&init_hdr, 0, sizeof(init_hdr));
        init_hdr.magic = MMAP_MAGIC_HEADER;
        init_hdr.boot_count = 1;
        init_hdr.write_count = 1;
        strncpy(init_hdr.device_tag, "ESP32-S3-ZEPHYR-N16R8", sizeof(init_hdr.device_tag) - 1);
        strncpy(init_hdr.user_memo, "Hello from Zephyr RTOS 128KB Flash MMAP!", sizeof(init_hdr.user_memo) - 1);

        esp_flash_erase_region(NULL, MMAP_STORAGE_PHYS_ADDR, 4096);
        esp_flash_write(NULL, &init_hdr, MMAP_STORAGE_PHYS_ADDR, sizeof(init_hdr));
    } else {
        mmap_storage_increment_boot_count();
    }
    return true;
}

const mmap_nv_header_t* mmap_storage_get_header(void)
{
    if (!g_initialized && !mmap_storage_init()) return NULL;
    return (const mmap_nv_header_t *)g_mapped_ptr;
}

const uint8_t* mmap_storage_get_raw_ptr(void)
{
    if (!g_initialized && !mmap_storage_init()) return NULL;
    return (const uint8_t *)g_mapped_ptr;
}

uint32_t mmap_storage_get_virt_addr(void)
{
    return (uint32_t)g_mapped_ptr;
}

uint32_t mmap_storage_get_phys_addr(void)
{
    return MMAP_STORAGE_PHYS_ADDR;
}

void mmap_storage_increment_boot_count(void)
{
    if (!g_initialized && !mmap_storage_init()) return;
    const mmap_nv_header_t *hdr = (const mmap_nv_header_t *)g_mapped_ptr;

    mmap_nv_header_t copy;
    memcpy(&copy, hdr, sizeof(copy));
    copy.boot_count++;

    esp_flash_erase_region(NULL, MMAP_STORAGE_PHYS_ADDR, 4096);
    esp_flash_write(NULL, &copy, MMAP_STORAGE_PHYS_ADDR, sizeof(copy));
}

bool mmap_storage_save_memo(const char *memo)
{
    if (!g_initialized && !mmap_storage_init()) return false;
    if (memo == NULL) return false;

    const mmap_nv_header_t *hdr = (const mmap_nv_header_t *)g_mapped_ptr;
    mmap_nv_header_t copy;
    memcpy(&copy, hdr, sizeof(copy));

    memset(copy.user_memo, 0, sizeof(copy.user_memo));
    strncpy(copy.user_memo, memo, sizeof(copy.user_memo) - 1);
    copy.write_count++;

    esp_err_t err = esp_flash_erase_region(NULL, MMAP_STORAGE_PHYS_ADDR, 4096);
    if (err != ESP_OK) return false;

    err = esp_flash_write(NULL, &copy, MMAP_STORAGE_PHYS_ADDR, sizeof(copy));
    return (err == ESP_OK);
}

bool mmap_storage_set_device_tag(const char *tag)
{
    if (!g_initialized && !mmap_storage_init()) return false;
    if (tag == NULL) return false;

    const mmap_nv_header_t *hdr = (const mmap_nv_header_t *)g_mapped_ptr;
    mmap_nv_header_t copy;
    memcpy(&copy, hdr, sizeof(copy));

    memset(copy.device_tag, 0, sizeof(copy.device_tag));
    strncpy(copy.device_tag, tag, sizeof(copy.device_tag) - 1);
    copy.write_count++;

    esp_err_t err = esp_flash_erase_region(NULL, MMAP_STORAGE_PHYS_ADDR, 4096);
    if (err != ESP_OK) return false;

    err = esp_flash_write(NULL, &copy, MMAP_STORAGE_PHYS_ADDR, sizeof(copy));
    return (err == ESP_OK);
}

void mmap_storage_hex_dump(uint32_t offset, size_t length, char *out_buf, size_t buf_size)
{
    if (!g_initialized && !mmap_storage_init()) {
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

        for (size_t j = 0; j < 16; j++) {
            if (i + j < length) {
                written += snprintf(out_buf + written, buf_size - written, "%02X ", ptr[i + j]);
            } else {
                written += snprintf(out_buf + written, buf_size - written, "   ");
            }
        }
        written += snprintf(out_buf + written, buf_size - written, " |");

        for (size_t j = 0; j < 16 && (i + j) < length; j++) {
            char c = ptr[i + j];
            written += snprintf(out_buf + written, buf_size - written, "%c", (c >= 32 && c <= 126) ? c : '.');
        }
        written += snprintf(out_buf + written, buf_size - written, "|\r\n");
    }
}
