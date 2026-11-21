/**
 * @file debug_cli.h
 * @brief Dual-Interface (USB Serial + BLE UART) CLI Shell for ESP32-S3
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>

/* ── CLI Configuration ─────────────────────────────────────── */
#define CLI_MAX_COMMANDS   32
#define CLI_MAX_CMD_LEN    128
#define CLI_MAX_ARGS       8
#define CLI_RX_RING_SIZE   512
#define CLI_PROMPT         "ESP32> "

#define BLE_DEVICE_NAME    "ESP32-S3-CLI"

/* ── Command Function Pointer ──────────────────────────────── */
typedef int (*cli_cmd_func_t)(int argc, char **argv);

/* ── Command Table Entry ────────────────────────────────────── */
typedef struct {
    const char      *name;
    cli_cmd_func_t   func;
    const char      *help;
} cli_command_t;

/* ── Public API ─────────────────────────────────────────────── */
void CLI_Init(void);
void CLI_Process(void);
bool CLI_RegisterCommand(const char *name, cli_cmd_func_t func, const char *help);
void CLI_PutRxChar(char ch);
void CLI_Print(const char *str);
void CLI_Printf(const char *format, ...);
bool CLI_IsBLEConnected(void);
String CLI_ExecuteWebCommand(const char *line);

/* ── PSRAM Test API ─────────────────────────────────────────── */
bool PSRAM_AllocMB(int mb);
void PSRAM_FreeAll(void);
size_t PSRAM_GetAllocatedBytes(void);
