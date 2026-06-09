// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>

// Time backend: NTP sync and dual-timezone (Pacific/Eastern) formatting.
void timeBegin();                                 // start SNTP and wait briefly for sync
bool timeFormatDateTime(char *buf, size_t len);   // false until the clock is set
