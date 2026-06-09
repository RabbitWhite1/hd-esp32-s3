// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once

// Python-logging-style serial logger. Every line is prefixed with a
// "yymmdd-hhmmss" local-time stamp and a severity tag, e.g.:
//   240608-143022 [INFO] WiFi connected, IP: 192.168.1.50

// Severity levels, low to high (mirrors Python's DEBUG < INFO < WARNING < ERROR).
enum LogLevel { LOG_DEBUG = 0,
                LOG_INFO = 1,
                LOG_WARN = 2,
                LOG_ERROR = 3 };

// Minimum level that is emitted; messages below it are dropped.
// Default LOG_DEBUG = print everything. Override with -DLOG_LEVEL=LOG_WARN etc.
#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_DEBUG
#endif

// printf-style; the format string is checked at compile time.
void logDebug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logInfo(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logWarn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logError(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
