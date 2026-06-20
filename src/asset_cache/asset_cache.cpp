// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "asset_cache.h"
#include "../wifi_net/wifi_net.h"
#include "../sdcard/sdcard.h"
#include "../logging/logging.h"
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <sys/stat.h>
#include <time.h>

static const long ASSET_TTL_SEC = 7L * 24 * 3600;  // 1 week

static const CachedAsset ASSETS[] = {
  {"/bootstrap.css", "bootstrap.css",
   "https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css", "text/css"},
  {"/bootstrap.js", "bootstrap.js",
   "https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/js/bootstrap.bundle.min.js", "application/javascript"},
  {"/sortable.js", "sortable.js",
   "https://cdn.jsdelivr.net/npm/sortablejs@1.15.6/Sortable.min.js", "application/javascript"},
};
static const int ASSET_COUNT = sizeof(ASSETS) / sizeof(ASSETS[0]);

int assetCount() {
  return ASSET_COUNT;
}
const CachedAsset *assetAt(int i) {
  if (i < 0 || i >= ASSET_COUNT) return nullptr;
  return &ASSETS[i];
}
const CachedAsset *assetByRoute(const char *route) {
  for (int i = 0; i < ASSET_COUNT; i++)
    if (strcmp(ASSETS[i].route, route) == 0) return &ASSETS[i];
  return nullptr;
}

bool assetIsCached(const CachedAsset &a) {
  String p = sdPath(a.file);
  if (p.length() == 0) return false;
  struct stat st;
  return stat(p.c_str(), &st) == 0 && st.st_size > 0;
}

// True if the asset is absent or older than the TTL (and we know the time).
static bool needsRefresh(const CachedAsset &a) {
  String p = sdPath(a.file);
  if (p.length() == 0) return false;  // no SD card -> nothing to cache
  struct stat st;
  if (stat(p.c_str(), &st) != 0 || st.st_size == 0) return true;  // missing/empty
  time_t now = time(nullptr);
  if (now < 1700000000) return false;  // clock not synced -> don't churn the cache
  return (now - st.st_mtime) > ASSET_TTL_SEC;
}

// Download one asset to a temp file, then atomically swap it in so a failed or
// partial fetch never replaces a good cached copy.
static bool downloadAsset(const CachedAsset &a) {
  String dst = sdPath(a.file);
  if (dst.length() == 0) return false;
  String tmp = dst + ".tmp";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, a.url)) return false;
  int code = http.GET();  // redirect-following is on by default
  if (code != 200) {
    logError("Asset %s: HTTP %d", a.file, code);
    http.end();
    return false;
  }
  int len = http.getSize();  // -1 if unknown (chunked)
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) {
    logError("Asset %s: temp open failed", a.file);
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t total = 0;
  while (http.connected() && (len < 0 || total < (size_t)len)) {
    size_t avail = stream->available();
    if (avail) {
      int n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
      if (n <= 0) break;
      fwrite(buf, 1, n, f);
      total += n;
    } else {
      delay(1);
    }
  }
  fclose(f);
  http.end();

  if (total == 0 || (len > 0 && total != (size_t)len)) {
    logWarn("Asset %s: incomplete (%u/%d)", a.file, (unsigned)total, len);
    remove(tmp.c_str());
    return false;
  }
  remove(dst.c_str());
  if (rename(tmp.c_str(), dst.c_str()) != 0) {
    logError("Asset %s: rename failed", a.file);
    remove(tmp.c_str());
    return false;
  }
  logInfo("Asset cached: %s (%u bytes)", a.file, (unsigned)total);
  return true;
}

void assetsEnsureFresh() {
  if (!sdMounted()) {
    logWarn("Asset cache: no SD card, web UI will use the CDN");
    return;
  }
  if (!wifiConnected()) {
    logInfo("Asset cache: offline, serving whatever is already cached");
    return;
  }
  for (int i = 0; i < ASSET_COUNT; i++) {
    if (needsRefresh(ASSETS[i])) downloadAsset(ASSETS[i]);
  }
}
