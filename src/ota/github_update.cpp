// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "github_update.h"
#include "ota.h"
#include "../wifi_net/wifi_net.h"
#include "../config/config.h"
#include "../logging/logging.h"
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <mbedtls/sha256.h>

static const char *GH_REPO_KEY = "gh_repo";
static const char *GH_TOKEN_KEY = "gh_token";
static const char *GH_REPO_DEFAULT = "RabbitWhite1/hd-esp32-s3";
static const char *BIN_NAME = "hd-esp32-s3.ino.bin";

static const int MAX_RELEASES = 10;
struct Release {
  String tag;
  uint32_t binId = 0;   // asset ids, not URLs: the asset endpoint is the one path
  uint32_t shaId = 0;   // that works for public and private repos alike
  uint32_t binSize = 0;
};
static Release rels[MAX_RELEASES];
static int relCount = 0;
static time_t listedAt = 0;
static unsigned long listedMs = 0;  // millis(), so staleness works before NTP lands
static String lastError = "";

static String repoName() {
  String r = configGet(GH_REPO_KEY);
  r.trim();
  return r.length() ? r : String(GH_REPO_DEFAULT);
}

const char *ghRepo() {
  static String r;
  r = repoName();
  return r.c_str();
}
int ghCount() {
  return relCount;
}
const char *ghTag(int i) {
  return (i >= 0 && i < relCount) ? rels[i].tag.c_str() : "";
}
uint32_t ghSize(int i) {
  return (i >= 0 && i < relCount) ? rels[i].binSize : 0;
}
time_t ghListedAt() {
  return listedAt;
}
const char *ghError() {
  return lastError.c_str();
}

// GitHub rejects requests without a User-Agent and wants the versioned Accept.
// The token is optional: a public repo serves both the listing and the assets
// anonymously, so it is attached only when one is configured.
static void addGhHeaders(HTTPClient &http, const char *accept) {
  http.addHeader("Accept", accept);
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  http.setUserAgent("hd-esp32-s3");
  String tok = configGet(GH_TOKEN_KEY);
  tok.trim();
  if (tok.length()) http.addHeader("Authorization", String("Bearer ") + tok);
}

bool ghRefreshIfStale(unsigned long maxAgeSec) {
  bool fresh = listedMs && (millis() - listedMs < maxAgeSec * 1000UL);
  if (fresh && relCount > 0) return true;
  if (!wifiConnected()) return relCount > 0;  // offline: keep showing what we had
  ghRefresh();  // a failure leaves the previous list in place
  return relCount > 0;
}

bool ghRefresh() {
  if (!wifiConnected()) {
    lastError = "Wi-Fi not connected";
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();  // the image is verified by SHA-256, not by the chain
  HTTPClient http;
  String url = "https://api.github.com/repos/" + repoName() +
               "/releases?per_page=" + String(MAX_RELEASES);
  if (!http.begin(client, url)) {
    lastError = "connection failed";
    return false;
  }
  addGhHeaders(http, "application/vnd.github+json");
  int code = http.GET();
  if (code != 200) {
    lastError = "GitHub HTTP " + String(code) +
                (code == 404 ? " (private repo? set gh_token)" : "");
    logError("Release list failed: %s", lastError.c_str());
    http.end();
    return false;
  }

  // Release notes run to kilobytes each; a filter keeps only what is needed so
  // the listing parses in a few KB instead of the whole payload.
  JsonDocument filter;
  filter[0]["tag_name"] = true;
  filter[0]["assets"][0]["name"] = true;
  filter[0]["assets"][0]["id"] = true;
  filter[0]["assets"][0]["size"] = true;
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    lastError = String("parse error: ") + err.c_str();
    return false;
  }

  listedMs = millis();  // stamp the attempt so a failing repo isn't retried per render
  relCount = 0;
  int published = doc.as<JsonArray>().size();
  String shaName = String(BIN_NAME) + ".sha256";
  for (JsonObject rel : doc.as<JsonArray>()) {
    if (relCount >= MAX_RELEASES) break;
    Release r;
    const char *t = rel["tag_name"];
    if (!t || !*t) continue;
    r.tag = t;
    for (JsonObject a : rel["assets"].as<JsonArray>()) {
      const char *n = a["name"];
      if (!n) continue;
      if (shaName == n) r.shaId = a["id"] | 0;
      else if (String(BIN_NAME) == n) {
        r.binId = a["id"] | 0;
        r.binSize = a["size"] | 0;
      }
    }
    if (r.binId) rels[relCount++] = r;  // a release with no image is not installable
  }
  listedAt = time(nullptr);
  listedMs = millis();
  if (relCount == 0) {
    // Distinguish "nothing published yet" from "published, but the CI asset is
    // missing" -- the fixes are entirely different (cut a tag vs. check the run).
    lastError = published ? (String("releases exist but none carries ") + BIN_NAME)
                          : String("no releases published yet (push a v* tag)");
    return false;
  }
  lastError = "";
  logInfo("Listed %d installable release(s) from %s", relCount, repoName().c_str());
  return true;
}

// Where an asset's bytes actually live. The asset endpoint answers with a 302 to
// a signed storage URL, and that host rejects the request if the Authorization
// header follows it -- so the redirect is walked by hand and the second hop is
// made clean.
struct AssetSource {
  String url;
  bool needAuth = false;  // true only if GitHub served the bytes inline (no redirect)
};

// Resolve the asset to a URL, tearing this connection down before returning.
// Two live TLS sessions is enough to exhaust the heap on this part -- that shows
// up as a bare "connection refused" (-1) on the second one -- so the resolve step
// keeps its client in this scope and the download opens its own afterwards.
static bool resolveAsset(uint32_t id, AssetSource &src) {
  String api = "https://api.github.com/repos/" + repoName() + "/releases/assets/" + String(id);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, api)) {
    lastError = "connection failed";
    return false;
  }
  addGhHeaders(http, "application/octet-stream");
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const char *loc[] = {"Location"};
  http.collectHeaders(loc, 1);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    src.url = api;  // served inline; re-request it with the same headers
    src.needAuth = true;
    ok = true;
  } else if (code == 302 || code == 307) {
    src.url = http.header("Location");
    src.needAuth = false;
    ok = src.url.length() > 0;
    if (!ok) lastError = "redirect carried no Location";
  } else {
    lastError = "asset HTTP " + String(code);
  }
  http.end();
  return ok;
}

// Open the resolved URL for reading. The caller owns the client/http pair, so
// only one TLS session is ever live at a time.
static bool openAsset(const AssetSource &src, WiFiClientSecure &client, HTTPClient &http) {
  client.setInsecure();
  if (!http.begin(client, src.url)) {
    lastError = "connection failed";
    return false;
  }
  if (src.needAuth) addGhHeaders(http, "application/octet-stream");
  else http.setUserAgent("hd-esp32-s3");  // the signed URL carries its own auth
  int code = http.GET();
  if (code != 200) {
    lastError = "asset HTTP " + String(code) + " (heap " + String((unsigned)ESP.getFreeHeap()) + ")";
    http.end();
    return false;
  }
  return true;
}

// The .sha256 asset holds "<64 hex>  <filename>"; keep the digest.
static bool fetchExpectedSha(uint32_t id, String &sha) {
  AssetSource src;
  if (!resolveAsset(id, src)) return false;
  WiFiClientSecure client;
  HTTPClient http;
  if (!openAsset(src, client, http)) return false;
  String body = http.getString();
  http.end();
  body.trim();
  int sp = body.indexOf(' ');
  sha = (sp > 0) ? body.substring(0, sp) : body;
  sha.toLowerCase();
  return sha.length() == 64;
}

bool ghInstall(const String &tag) {
  lastError = "";
  if (!wifiConnected()) {
    lastError = "Wi-Fi not connected";
    return false;
  }
  int idx = -1;
  for (int i = 0; i < relCount; i++)
    if (rels[i].tag == tag) idx = i;
  if (idx < 0) {
    lastError = "unknown release (refresh the list)";
    return false;
  }

  // With no checksum there is nothing to verify the image against, and an
  // unverified flash is the failure mode most worth avoiding here.
  String expected;
  if (!rels[idx].shaId || !fetchExpectedSha(rels[idx].shaId, expected)) {
    if (!lastError.length()) lastError = "release has no usable .sha256";
    return false;
  }

  AssetSource src;
  if (!resolveAsset(rels[idx].binId, src)) return false;
  logInfo("Downloading image (free heap %u)", (unsigned)ESP.getFreeHeap());
  WiFiClientSecure client;
  HTTPClient http;
  if (!openAsset(src, client, http)) return false;
  int total = http.getSize();
  if (total <= 0) total = (int)rels[idx].binSize;
  if (total <= 0) {
    lastError = "unknown image size";
    http.end();
    return false;
  }
  if (!Update.begin(total)) {
    lastError = "no room in the idle slot";
    http.end();
    return false;
  }

  logInfo("Installing %s (%d bytes) from %s", tag.c_str(), total, repoName().c_str());
  String status = String("Installing ") + tag;
  otaReport(true, 0, status.c_str());

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);  // 0 = SHA-256, not SHA-224

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  int written = 0, lastPct = -1;
  unsigned long lastData = millis();
  while (written < total) {
    int avail = stream->available();
    if (avail <= 0) {
      if (!stream->connected() || millis() - lastData > 15000) break;
      delay(1);
      continue;
    }
    int want = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
    int n = stream->readBytes(buf, want);
    if (n <= 0) continue;
    lastData = millis();
    if ((int)Update.write(buf, n) != n) {
      lastError = "flash write failed";
      break;
    }
    mbedtls_sha256_update(&sha, buf, n);
    written += n;
    int pct = (int)((int64_t)written * 100 / total);
    if (pct >= lastPct + 5) {
      lastPct = pct;
      otaReport(true, pct, status.c_str());
    }
  }
  http.end();

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);

  if (written != total && !lastError.length())
    lastError = "download truncated (" + String(written) + "/" + String(total) + ")";
  else if (written == total && expected != hex)
    lastError = "checksum mismatch -- image rejected";

  if (lastError.length()) {
    Update.abort();  // nothing committed, so the running slot stays untouched
    logError("Install failed: %s", lastError.c_str());
    otaReport(false, 0, lastError.c_str());
    return false;
  }
  if (!Update.end(true)) {
    lastError = "finalize failed";
    otaReport(false, 0, lastError.c_str());
    return false;
  }

  logInfo("Installed %s -> rebooting", tag.c_str());
  otaReport(true, 100, "Rebooting");
  delay(400);  // let the final redraw land before the reset
  ESP.restart();
  return true;
}
