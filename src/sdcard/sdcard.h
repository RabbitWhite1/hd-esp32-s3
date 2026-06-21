// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>

// microSD backend for the ESP32-S3-RLCD-4.2. The slot is wired for SDMMC in
// 1-bit mode (CLK=38, CMD=21, D0=39 — from Waveshare's 06_SD_Card BSP) and is
// mounted at /sdcard. Provides whole-file text read/write plus an explicit
// reformat. All ops are no-ops (return false / "") when no card is mounted, so
// callers can use the card opportunistically without guarding every call.
bool sdBegin();    // mount; formats a raw/unreadable card so it becomes usable. true if mounted (logs result)
bool sdMounted();  // true once a card is mounted
bool sdFormat();   // wipe the card to a fresh FAT filesystem (requires a mounted card)

// Hot-plug support: poll sdCardPresent() to notice a card pulled at runtime, then
// sdUnmount() it; sdRemount() re-tries the mount (quiet on failure) for re-insert.
bool sdCardPresent();  // true if the mounted card still responds (CMD13); false if gone/unmounted
void sdUnmount();      // unmount + drop the handle (call after the card is pulled)
bool sdRemount();      // attempt to mount again, without logging a failure; true if now mounted

bool sdWriteText(const char *name, const String &text);   // overwrite /sdcard/<name>; true on success
bool sdAppendText(const char *name, const String &text);  // append to /sdcard/<name> (created if absent); true on success
String sdReadText(const char *name);                      // whole file as a String ("" if missing/unmounted)

// Absolute path "/sdcard/<name>" for callers that stream a file directly with
// fopen()/stat() (e.g. large cached assets). "" when no card is mounted.
String sdPath(const char *name);
