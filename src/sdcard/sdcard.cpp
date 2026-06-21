// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "sdcard.h"
#include "../logging/logging.h"
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>

static const char *MOUNT_POINT = "/sdcard";
// SDMMC 1-bit pins for the ESP32-S3-RLCD-4.2 (Waveshare 06_SD_Card BSP defaults).
static const int SD_CLK = 38, SD_CMD = 21, SD_D0 = 39;

static sdmmc_card_t *card = nullptr;
static bool mounted = false;

bool sdBegin() {
  esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
  mcfg.format_if_mount_failed = true;  // make a raw/unreadable card usable
  mcfg.max_files = 5;
  mcfg.allocation_unit_size = 16 * 1024;

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.clk = (gpio_num_t)SD_CLK;
  slot.cmd = (gpio_num_t)SD_CMD;
  slot.d0 = (gpio_num_t)SD_D0;

  esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mcfg, &card);
  mounted = (err == ESP_OK && card != nullptr);
  if (mounted) logInfo("SD mounted at %s", MOUNT_POINT);
  else logWarn("SD mount failed (0x%x) - card inserted?", err);
  return mounted;
}

bool sdMounted() {
  return mounted;
}

bool sdFormat() {
  if (!mounted || !card) {
    logWarn("SD format skipped (not mounted)");
    return false;
  }
  logInfo("SD card formatting");
  esp_err_t err = esp_vfs_fat_sdcard_format(MOUNT_POINT, card);
  logInfo("SD card formatting done");
  if (err == ESP_OK) {
    logInfo("SD card formatted");
    return true;
  }
  logError("SD format failed (0x%x)", err);
  return false;
}

static String fullPath(const char *name) {
  String p = MOUNT_POINT;
  p += "/";
  p += name;
  return p;
}

String sdPath(const char *name) {
  if (!mounted) return String();
  return fullPath(name);
}

bool sdWriteText(const char *name, const String &text) {
  if (!mounted) return false;
  String p = fullPath(name);
  FILE *f = fopen(p.c_str(), "wb");
  if (!f) {
    logError("SD write open failed: %s", p.c_str());
    return false;
  }
  size_t n = text.length() ? fwrite(text.c_str(), 1, text.length(), f) : 0;
  fclose(f);
  return n == text.length();
}

bool sdAppendText(const char *name, const String &text) {
  if (!mounted) return false;
  String p = fullPath(name);
  FILE *f = fopen(p.c_str(), "ab");
  if (!f) {
    logError("SD append open failed: %s", p.c_str());
    return false;
  }
  size_t n = text.length() ? fwrite(text.c_str(), 1, text.length(), f) : 0;
  fclose(f);
  return n == text.length();
}

String sdReadText(const char *name) {
  if (!mounted) return String();
  String p = fullPath(name);
  FILE *f = fopen(p.c_str(), "rb");
  if (!f) return String();
  String out;
  char buf[129];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
    buf[n] = '\0';
    out += buf;
  }
  fclose(f);
  return out;
}
