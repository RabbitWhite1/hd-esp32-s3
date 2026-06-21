// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "gdoc.h"
#include "../wifi_net/wifi_net.h"
#include "../config/config.h"  // URL + refresh interval persisted in esp32.json
#include "../logging/logging.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// Link-shared Google Doc, fetched as plain text. The "export?format=txt" endpoint
// 307-redirects to a signed googleusercontent.com host, so redirect-following must
// be enabled. The doc must stay shared as "anyone with the link can view". No URL
// is baked into the firmware — it is set at runtime via the web UI and persisted
// in the shared config (esp32.json, key gdoc_url); empty until then, so
// gdocUpdate() is a no-op.
static String docUrl = "";

static const int MAX_DOC_LINES = 12;
static const int MAX_LINE_LEN = 96;  // bytes kept per line (UTF-8: ~32 Chinese or 96 ASCII)

static String lines[MAX_DOC_LINES];
static int lineCount = 0;
static String title = "";  // document title, parsed from the export filename
static bool ok = false;
static time_t asOf = 0;  // wall-clock time of the last successful fetch

// Keep printable ASCII and whole UTF-8 multi-byte sequences (so Chinese survives
// for the GB2312-font Notes box); drop control bytes. Truncates on a character
// boundary at MAX_LINE_LEN bytes so a multi-byte glyph is never split.
static String sanitize(const String &raw) {
  String out;
  out.reserve(raw.length());
  size_t i = 0;
  while (i < raw.length()) {
    uint8_t c = (uint8_t)raw[i];
    if (c < 0x20 || c == 0x7f) { i++; continue; }  // skip control bytes (incl. \r \t)
    int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;  // UTF-8 length
    if ((int)out.length() + len > MAX_LINE_LEN) break;  // stop at a char boundary
    for (int k = 0; k < len && i < raw.length(); k++) out += (char)raw[i++];
  }
  out.trim();
  return out;
}

// Reduce a normal Google Docs link to the canonical base URL (no /edit, no query,
// no export suffix); pass-through anything that isn't a recognizable docs URL.
// The "/export?format=txt" is appended only at fetch time (see gdocUpdate).
static String normalizeDocUrl(const String &in) {
  int d = in.indexOf("/d/");
  if (d < 0) return in;
  int idStart = d + 3;
  int idEnd = in.indexOf('/', idStart);
  int q = in.indexOf('?', idStart);
  if (q >= 0 && (idEnd < 0 || q < idEnd)) idEnd = q;  // stop at a query if it comes first
  String id = (idEnd < 0) ? in.substring(idStart) : in.substring(idStart, idEnd);
  if (id.length() == 0) return in;
  return "https://docs.google.com/document/d/" + id;
}

void gdocSetUrl(const String &url) {
  if (url.length() == 0) return;  // empty -> keep current
  docUrl = normalizeDocUrl(url);
}
const String &gdocUrl() {
  return docUrl;
}
static const char *GDOC_URL_KEY = "gdoc_url";

bool gdocSaveUrl() {
  configSet(GDOC_URL_KEY, docUrl);
  bool ok = configSave();
  if (ok) logInfo("gdoc URL saved to config");
  else logError("gdoc URL save failed (SD card?)");
  return ok;
}
void gdocLoadUrl() {
  String u = configGet(GDOC_URL_KEY);
  u.trim();
  if (u.length()) {
    gdocSetUrl(u);
    logInfo("gdoc URL loaded from config");
  }
}

static const char *GDOC_INTERVAL_KEY = "gdoc_refresh_min";
static const int GDOC_INTERVAL_DEFAULT = 240;  // 4 hours

int gdocIntervalMin() {
  long m = configGetInt(GDOC_INTERVAL_KEY, GDOC_INTERVAL_DEFAULT);
  return m < 1 ? 1 : (int)m;
}
bool gdocSetIntervalMin(int minutes) {
  if (minutes < 1) minutes = 1;
  configSetInt(GDOC_INTERVAL_KEY, minutes);
  return configSave();
}

void gdocUpdate() {
  if (!wifiConnected() || docUrl.length() == 0) return;
  WiFiClientSecure client;
  client.setInsecure();  // skip cert validation (same approach as the weather fetch)

  String fetchUrl = docUrl + "/export?format=txt";  // export suffix added only for the fetch
  HTTPClient http;
  if (!http.begin(client, fetchUrl)) return;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // 307 -> googleusercontent.com
  // Browser-like UA reduces the chance of being bounced by anti-bot filtering.
  http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
  // Keep the export's Content-Disposition so we can read the doc title off it.
  const char *headerKeys[] = {"Content-Disposition"};
  http.collectHeaders(headerKeys, 1);

  int code = http.GET();
  if (code != 200) {
    logError("Doc fetch HTTP %d", code);
    http.end();
    ok = false;
    return;
  }
  // The title rides along as the download filename, e.g.
  //   Content-Disposition: attachment; filename="hd.txt"
  String cd = http.header("Content-Disposition");
  int fn = cd.indexOf("filename=\"");
  if (fn >= 0) {
    int s = fn + 10;
    int e = cd.indexOf('"', s);
    if (e > s) {
      String t = cd.substring(s, e);
      if (t.endsWith(".txt")) t = t.substring(0, t.length() - 4);
      title = sanitize(t);
    }
  }
  String payload = http.getString();
  http.end();

  // Strip a leading UTF-8 BOM (EF BB BF) the txt export prepends.
  if (payload.length() >= 3 && (uint8_t)payload[0] == 0xEF &&
      (uint8_t)payload[1] == 0xBB && (uint8_t)payload[2] == 0xBF)
    payload.remove(0, 3);

  // Split on newlines, keeping blank lines (they separate paragraphs) but
  // collapsing a run of consecutive blanks into a single blank — the Google txt
  // export emits two blank lines per paragraph gap. Trailing blanks are dropped.
  lineCount = 0;
  int start = 0;
  while (start <= (int)payload.length() && lineCount < MAX_DOC_LINES) {
    int nl = payload.indexOf('\n', start);
    String s = sanitize(nl < 0 ? payload.substring(start) : payload.substring(start, nl));
    bool prevBlank = (lineCount > 0 && lines[lineCount - 1].length() == 0);
    if (s.length() > 0 || !prevBlank) lines[lineCount++] = s;  // skip a 2nd+ consecutive blank
    if (nl < 0) break;
    start = nl + 1;
  }
  while (lineCount > 0 && lines[lineCount - 1].length() == 0) lineCount--;  // drop trailing blanks
  ok = true;
  asOf = time(nullptr);
  logInfo("Doc fetched: %d lines", lineCount);
}

bool gdocOk() {
  return ok;
}
int gdocLineCount() {
  return lineCount;
}
const char *gdocLine(int i) {
  if (i < 0 || i >= lineCount) return "";
  return lines[i].c_str();
}
const char *gdocTitle() {
  return title.c_str();
}
time_t gdocAsOf() {
  return asOf;
}
