#pragma once
#include <Arduino.h>

// microSD backend for the ESP32-S3-RLCD-4.2. The slot is wired for SDMMC in
// 1-bit mode (CLK=38, CMD=21, D0=39 — from Waveshare's 06_SD_Card BSP) and is
// mounted at /sdcard. Provides whole-file text read/write plus an explicit
// reformat. All ops are no-ops (return false / "") when no card is mounted, so
// callers can use the card opportunistically without guarding every call.
bool sdBegin();    // mount; formats a raw/unreadable card so it becomes usable. true if mounted
bool sdMounted();  // true once a card is mounted
bool sdFormat();   // wipe the card to a fresh FAT filesystem (requires a mounted card)

bool sdWriteText(const char *name, const String &text);  // overwrite /sdcard/<name>; true on success
String sdReadText(const char *name);                     // whole file as a String ("" if missing/unmounted)

// Absolute path "/sdcard/<name>" for callers that stream a file directly with
// fopen()/stat() (e.g. large cached assets). "" when no card is mounted.
String sdPath(const char *name);
