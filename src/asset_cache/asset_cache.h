// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include <Arduino.h>

// Offline cache for the web UI's third-party static assets (Bootstrap CSS/JS).
// Each asset is downloaded once from its CDN and stored on the SD card; it is
// re-fetched only when missing or older than the TTL (1 week). The web server
// serves the cached copy from SD, so the panel stays styled with no internet —
// and falls back to the CDN URL only when nothing is cached yet.
struct CachedAsset {
  const char *route;        // path the web server serves it at, e.g. "/bootstrap.css"
  const char *file;         // filename on the SD card
  const char *url;          // upstream CDN source (also the offline fallback link)
  const char *contentType;  // MIME type to serve with
};

void assetsEnsureFresh();  // download any missing/stale asset when Wi-Fi is up (call after timeBegin)

int assetCount();
const CachedAsset *assetAt(int i);
const CachedAsset *assetByRoute(const char *route);  // nullptr if no such route
bool assetIsCached(const CachedAsset &a);            // true if a non-empty SD copy exists
