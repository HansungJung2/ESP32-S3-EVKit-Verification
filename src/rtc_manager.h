/**
 * @file rtc_manager.h
 * @brief ESP32-S3 Internal RTC Hardware Clock & Time Sync Manager
 */

#pragma once

#include <Arduino.h>
#include <sys/time.h>
#include <time.h>

void   RTC_Init(void);
void   RTC_SetEpoch(time_t epoch, int tz_offset_sec = 9 * 3600);
bool   RTC_SetManualTime(int year, int mon, int mday, int hour, int min, int sec);
bool   RTC_SyncNTP(void);
bool   RTC_IsSynced(void);
String RTC_GetTimeString(void);
time_t RTC_GetEpoch(void);

