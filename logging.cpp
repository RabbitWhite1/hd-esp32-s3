// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "logging.h"
#include <Arduino.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>

static const char *levelTag(LogLevel level) {
  switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO: return "INFO";
    case LOG_WARN: return "WARN";
    case LOG_ERROR: return "ERROR";
  }
  return "?";
}

static void logEmit(LogLevel level, const char *fmt, va_list args) {
  if (level < LOG_LEVEL) return;

  // local-time stamp; reads ~1970 until time_sync has run NTP.
  char ts[16];
  time_t now = time(nullptr);
  struct tm tm_now;
  localtime_r(&now, &tm_now);
  strftime(ts, sizeof(ts), "%y%m%d-%H%M%S", &tm_now);

  char msg[200];
  vsnprintf(msg, sizeof(msg), fmt, args);

  Serial.printf("%s [%s] %s\n", ts, levelTag(level), msg);
}

void logDebug(const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  logEmit(LOG_DEBUG, fmt, a);
  va_end(a);
}
void logInfo(const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  logEmit(LOG_INFO, fmt, a);
  va_end(a);
}
void logWarn(const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  logEmit(LOG_WARN, fmt, a);
  va_end(a);
}
void logError(const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  logEmit(LOG_ERROR, fmt, a);
  va_end(a);
}
