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

// Open a release asset for reading. The asset endpoint answers with a 302 to a
// signed storage URL, and that host rejects the request if the Authorization
// header follows it -- so the redirect is walked here rather than by HTTPClient,
// and the second hop is made clean.
static bool openAsset(uint32_t id, WiFiClientSecure &c1, HTTPClient &h1,
                      WiFiClientSecure &c2, HTTPClient &h2, HTTPClient *&out) {
  String url = "https://api.github.com/repos/" + repoName() + "/releases/assets/" + String(id);
  c1.setInsecure();
  if (!h1.begin(c1, url)) {
    lastError = "connection failed";
    return false;
  }
  addGhHeaders(h1, "application/octet-stream");
  h1.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const char *loc[] = {"Location"};
  h1.collectHeaders(loc, 1);
  int code = h1.GET();
  if (code == 200) {  // served inline, no redirect to follow
    out = &h1;
    return true;
  }
  if (code != 302 && code != 307) {
    lastError = "asset HTTP " + String(code);
    h1.end();
    return false;
  }
  String next = h1.header("Location");
  h1.end();
  if (!next.length()) {
    lastError = "redirect carried no Location";
    return false;
  }
  c2.setInsecure();
  if (!h2.begin(c2, next)) {
    lastError = "redirect connection failed";
    return false;
  }
  h2.setUserAgent("hd-esp32-s3");  // no Authorization: the signed URL carries its own
  int code2 = h2.GET();
  if (code2 != 200) {
    lastError = "asset redirect HTTP " + String(code2);
    h2.end();
    return false;
  }
  out = &h2;
  return true;
}

// The .sha256 asset holds "<64 hex>  <filename>"; keep the digest.
static bool fetchExpectedSha(uint32_t id, String &sha) {
  WiFiClientSecure c1, c2;
  HTTPClient h1, h2, *r = nullptr;
  if (!openAsset(id, c1, h1, c2, h2, r)) return false;
  String body = r->getString();
  r->end();
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

  WiFiClientSecure c1, c2;
  HTTPClient h1, h2, *r = nullptr;
  if (!openAsset(rels[idx].binId, c1, h1, c2, h2, r)) return false;
  int total = r->getSize();
  if (total <= 0) total = (int)rels[idx].binSize;
  if (total <= 0) {
    lastError = "unknown image size";
    r->end();
    return false;
  }
  if (!Update.begin(total)) {
    lastError = "no room in the idle slot";
    r->end();
    return false;
  }

  logInfo("Installing %s (%d bytes) from %s", tag.c_str(), total, repoName().c_str());
  String status = String("Installing ") + tag;
  otaReport(true, 0, status.c_str());

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);  // 0 = SHA-256, not SHA-224

  WiFiClient *stream = r->getStreamPtr();
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
  r->end();

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
