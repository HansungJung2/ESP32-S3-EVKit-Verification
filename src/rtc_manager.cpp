/**
 * @file rtc_manager.cpp
 * @brief ESP32-S3 Internal RTC Hardware Clock & Time Sync implementation
 */

#include "rtc_manager.h"
#include <WiFi.h>
#include <esp_sntp.h>

static bool g_rtc_synced = false;

void RTC_Init(void)
{
    // 기본 타임존: 한국 표준시 (KST = UTC+9)
    setenv("TZ", "KST-9", 1);
    tzset();

    time_t now = time(NULL);
    if (now > 1700000000) { // 2023년 이후면 유효한 시간으로 간주
        g_rtc_synced = true;
    }
    Serial.printf("[RTC] Hardware RTC Initialized. Current Time: %s (Synced: %s)\r\n",
                  RTC_GetTimeString().c_str(),
                  g_rtc_synced ? "YES" : "NO");
}

void RTC_SetEpoch(time_t epoch, int tz_offset_sec)
{
    struct timeval tv;
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    char tz_str[32];
    int tz_hour = tz_offset_sec / 3600;
    // POSIX TZ format: KST-9 (음수가 동쪽/빠른시간)
    snprintf(tz_str, sizeof(tz_str), "UTC%+d", -tz_hour);
    setenv("TZ", "KST-9", 1);
    tzset();

    g_rtc_synced = true;
    Serial.printf("[RTC] Synced to Epoch: %ld -> %s (KST)\r\n", (long)epoch, RTC_GetTimeString().c_str());
}

bool RTC_SetManualTime(int year, int mon, int mday, int hour, int min, int sec)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon  = mon - 1;
    t.tm_mday = mday;
    t.tm_hour = hour;
    t.tm_min  = min;
    t.tm_sec  = sec;

    time_t epoch = mktime(&t);
    if (epoch < 0) return false;

    RTC_SetEpoch(epoch);
    return true;
}

bool RTC_SyncNTP(void)
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[RTC] Cannot sync NTP: Wi-Fi Station not connected to Internet!"));
        return false;
    }

    Serial.println(F("[RTC] Requesting time from NTP servers (kr.pool.ntp.org)..."));
    configTime(9 * 3600, 0, "kr.pool.ntp.org", "time.google.com", "pool.ntp.org");

    // 최대 5초 대기
    int retry = 0;
    time_t now = 0;
    while (retry < 10) {
        delay(500);
        now = time(NULL);
        if (now > 1700000000) {
            g_rtc_synced = true;
            Serial.printf("[RTC] NTP Sync Success: %s\r\n", RTC_GetTimeString().c_str());
            return true;
        }
        retry++;
    }
    Serial.println(F("[RTC] NTP Sync Timeout!"));
    return false;
}

bool RTC_IsSynced(void)
{
    if (g_rtc_synced) return true;
    time_t now = time(NULL);
    if (now > 1700000000) {
        g_rtc_synced = true;
        return true;
    }
    return false;
}

String RTC_GetTimeString(void)
{
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buf);
}

time_t RTC_GetEpoch(void)
{
    return time(NULL);
}

