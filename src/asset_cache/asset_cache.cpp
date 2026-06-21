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

// Cached copies live under /sdcard/assets/; the web routes keep their short
// "/bootstrap.css" form (the route is unrelated to the on-card path).
static const CachedAsset ASSETS[] = {
  {"/bootstrap.css", "assets/bootstrap.css",
   "https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css", "text/css"},
  {"/bootstrap.js", "assets/bootstrap.js",
   "https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/js/bootstrap.bundle.min.js", "application/javascript"},
  {"/sortable.js", "assets/sortable.js",
   "https://cdn.jsdelivr.net/npm/sortablejs@1.15.6/Sortable.min.js", "application/javascript"},
  {"/chart.js", "assets/chart.js",
   "https://cdn.jsdelivr.net/npm/chart.js@4.4.3/dist/chart.umd.min.js", "application/javascript"},
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

// A cached asset is only usable if its first byte looks like text (JS/CSS), not
// a gzip stream (0x1f) or an HTML/XML error page ('<') — guards against a copy
// that got saved compressed or as an error body.
static bool fileLooksValid(const String &p) {
  FILE *f = fopen(p.c_str(), "rb");
  if (!f) return false;
  int c = fgetc(f);
  fclose(f);
  return c != EOF && c != 0x1f && c != '<';
}

bool assetIsCached(const CachedAsset &a) {
  String p = sdPath(a.file);
  return p.length() && fileLooksValid(p);
}

// True if the asset is absent, corrupt, or older than the TTL (when time known).
static bool needsRefresh(const CachedAsset &a) {
  String p = sdPath(a.file);
  if (p.length() == 0) return false;     // no SD card -> nothing to cache
  if (!fileLooksValid(p)) return true;   // missing/empty/corrupt -> re-fetch
  struct stat st;
  if (stat(p.c_str(), &st) != 0) return true;
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
  http.setTimeout(8000);  // per-read socket timeout
  http.addHeader("Accept-Encoding", "identity");  // never let the CDN gzip the body
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
  // Bounded loop: never block boot forever if the connection stalls.
  unsigned long lastData = millis(), startMs = millis();
  while (http.connected() && (len < 0 || total < (size_t)len)) {
    size_t avail = stream->available();
    if (avail) {
      int n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
      if (n <= 0) break;
      fwrite(buf, 1, n, f);
      total += n;
      lastData = millis();
    } else {
      if (millis() - lastData > 8000) break;    // 8 s with no new data -> give up
      if (millis() - startMs > 45000) break;     // 45 s overall cap
      delay(5);
    }
  }
  fclose(f);
  http.end();

  if (total == 0 || (len > 0 && total != (size_t)len)) {
    logWarn("Asset %s: incomplete (%u/%d)", a.file, (unsigned)total, len);
    remove(tmp.c_str());
    return false;
  }
  if (!fileLooksValid(tmp)) {  // gzip/HTML body slipped through -> don't cache it
    logWarn("Asset %s: content not valid text, discarding", a.file);
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
  String dir = sdPath("assets");
  if (dir.length()) mkdir(dir.c_str(), 0777);  // ensure /sdcard/assets exists (harmless if it does)
  for (int i = 0; i < ASSET_COUNT; i++) {
    if (needsRefresh(ASSETS[i])) downloadAsset(ASSETS[i]);
  }
}
