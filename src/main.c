/*
 * Copyright (c) 2026
 * Zephyr RTOS Verification Firmware for ESP32-S3 (N16R8)
 *
 * Demonstrates:
 * 1. Multi-threaded RTOS tasks (Heartbeat & Worker threads)
 * 2. Zephyr Kernel IPC (Message Queues, Semaphores)
 * 3. Interactive Serial Shell with custom CLI commands
 * 4. System diagnostics & Thread stack usage analyzer
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/shell/shell.h>
#include <zephyr/debug/thread_analyzer.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ── Thread 1: Heartbeat Thread (1Hz) ────────────────────────────── */
#define HEARTBEAT_STACK_SIZE 2048
#define HEARTBEAT_PRIORITY   7

static void heartbeat_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Heartbeat thread started (Priority: %d)", HEARTBEAT_PRIORITY);

    uint32_t count = 0;
    while (1) {
        k_sleep(K_MSEC(1000));
        count++;
        int64_t uptime_ms = k_uptime_get();
        LOG_DBG("Heartbeat #%u (Uptime: %lld ms)", count, uptime_ms);
    }
}

K_THREAD_DEFINE(heartbeat_tid, HEARTBEAT_STACK_SIZE,
                heartbeat_thread_entry, NULL, NULL, NULL,
                HEARTBEAT_PRIORITY, 0, 0);

/* ── Thread 2: Worker Thread (Event Processing via Message Queue) ── */
#define WORKER_STACK_SIZE 2048
#define WORKER_PRIORITY   6

struct app_event {
    uint32_t id;
    int32_t  data;
};

K_MSGQ_DEFINE(event_msgq, sizeof(struct app_event), 16, 4);

static void worker_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Worker thread started (Priority: %d)", WORKER_PRIORITY);

    struct app_event evt;
    while (1) {
        if (k_msgq_get(&event_msgq, &evt, K_FOREVER) == 0) {
            LOG_INF("[Worker] Event received -> ID: %u, Data: %d", evt.id, evt.data);
        }
    }
}

K_THREAD_DEFINE(worker_tid, WORKER_STACK_SIZE,
                worker_thread_entry, NULL, NULL, NULL,
                WORKER_PRIORITY, 0, 0);

/* ── Shell Commands ──────────────────────────────────────────────── */

static int cmd_sysinfo(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "================ ESP32-S3 Zephyr RTOS ================");
    shell_print(sh, "  Kernel Version : %s", KERNEL_VERSION_STRING);
    shell_print(sh, "  Architecture   : Xtensa (ESP32-S3 Dual-Core LX7)");
    shell_print(sh, "  Board Target   : %s", CONFIG_BOARD);
    shell_print(sh, "  CPU Frequency  : %d MHz", CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000);
    shell_print(sh, "  System Uptime  : %lld sec", k_uptime_get() / 1000);
    shell_print(sh, "  Cycles         : %u", k_cycle_get_32());
    shell_print(sh, "======================================================");
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

    if (k_msgq_put(&event_msgq, &evt, K_NO_WAIT) == 0) {
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
    LOG_INF("   ESP32-S3 Zephyr RTOS Verification Firmware    ");
    LOG_INF("   Kernel: %s | Board: %s", KERNEL_VERSION_STRING, CONFIG_BOARD);
    LOG_INF("==================================================");

    LOG_INF("Zephyr Kernel initialized successfully!");
    LOG_INF("Interactive shell is ready on UART console (115200 bps).");
    LOG_INF("Type 'help' or 'app sysinfo' in serial monitor.");

    return 0;
}
