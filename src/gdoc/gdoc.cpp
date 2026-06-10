#include "gdoc.h"
#include "../wifi_net/wifi_net.h"
#include "../logging/logging.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// Link-shared Google Doc, fetched as plain text. The "export?format=txt" endpoint
// 307-redirects to a signed googleusercontent.com host, so redirect-following must
// be enabled. The doc must stay shared as "anyone with the link can view" for this
// to work without authentication. Replace the id to point at a different doc.
static const char *DOC_URL =
  "https://docs.google.com/document/d/"
  "1I6a2n9FEYPek4BmklUT_C4X7y0qPNivG4av5OZeoOy0/export?format=txt";

static const int MAX_DOC_LINES = 12;
static const int MAX_LINE_LEN = 48;  // chars kept per line (the display clips further)

static String lines[MAX_DOC_LINES];
static int lineCount = 0;
static String title = "";  // document title, parsed from the export filename
static bool ok = false;
static time_t asOf = 0;  // wall-clock time of the last successful fetch

// Keep only printable ASCII and trim the ends: U8g2's basic fonts can't render the
// UTF-8 multi-byte characters a Google Doc carries (smart quotes, emoji, etc.).
static String sanitize(const String &raw) {
  String out;
  out.reserve(raw.length());
  for (size_t i = 0; i < raw.length() && (int)out.length() < MAX_LINE_LEN; i++) {
    uint8_t c = (uint8_t)raw[i];
    if (c >= 0x20 && c < 0x7f) out += (char)c;  // drop control bytes + anything >= 0x80
  }
  out.trim();
  return out;
}

void gdocUpdate() {
  if (!wifiConnected()) return;
  WiFiClientSecure client;
  client.setInsecure();  // skip cert validation (same approach as the weather fetch)

  HTTPClient http;
  if (!http.begin(client, DOC_URL)) return;
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
  //   Content-Disposition: attachment; filename="h4d.txt"
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

  // Split on newlines, keeping blank lines too (they separate paragraphs in the
  // doc); only trailing blank lines are dropped below.
  lineCount = 0;
  int start = 0;
  while (start <= (int)payload.length() && lineCount < MAX_DOC_LINES) {
    int nl = payload.indexOf('\n', start);
    lines[lineCount++] = sanitize(nl < 0 ? payload.substring(start) : payload.substring(start, nl));
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
