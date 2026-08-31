// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "gdoc.h"
#include "../wifi_net/wifi_net.h"
#include "../config/config.h"  // URL + refresh interval persisted in esp32.json
#include "../logging/logging.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <atomic>

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

// Snapshot of the previously fetched revision, plus the diff accumulated against
// it. prevValid stays false until the first successful fetch has been snapshotted,
// so a fresh boot doesn't report the whole document as "new".
static String prevLines[MAX_DOC_LINES];

// Staged handoff between the fetch task and the loop task -- see the note in
// claude_usage.cpp. gdoc is the module that makes this necessary rather than
// merely tidy: gdocLine() returns a String's interior pointer, so the fetch must
// never rewrite lines[] while the renderer holds one.
static String stagedLines[MAX_DOC_LINES];
static int stagedCount = 0;
static String stagedTitle;
static bool stagedHaveTitle = false;
static bool stagedOk = false;
static bool stagedLinesValid = false;  // false after a failed fetch: keep the old text
static time_t stagedAsOf = 0;
static std::atomic<uint32_t> pending{0};
static int prevCount = 0;
static bool prevValid = false;

static const int MAX_DIFF_LINES = 24;  // bounded: the popup can't show more anyway
static String diffLines[MAX_DIFF_LINES];
static int diffCount = 0;
static bool diffTrunc = false;

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
static const int GDOC_INTERVAL_DEFAULT = 60;  // 1 hour

int gdocIntervalMin() {
  long m = configGetInt(GDOC_INTERVAL_KEY, GDOC_INTERVAL_DEFAULT);
  return m < 1 ? 1 : (int)m;
}
bool gdocSetIntervalMin(int minutes) {
  if (minutes < 1) minutes = 1;
  configSetInt(GDOC_INTERVAL_KEY, minutes);
  return configSave();
}

// Append one "<line number> <text>" entry to the pending diff, skipping duplicates
// (a line deleted and re-added later would otherwise pile up). The number is the
// line's 1-based position in the new revision, matching the Notes box.
static void diffAdd(int lineNo, const String &line) {
  String entry = String(lineNo) + " " + line;
  for (int i = 0; i < diffCount; i++)
    if (diffLines[i] == entry) return;
  if (diffCount < MAX_DIFF_LINES) diffLines[diffCount++] = entry;
  else diffTrunc = true;
}

// Diff the freshly parsed lines against the previous snapshot and accumulate the
// changes, then make the new revision the snapshot. A line-level LCS decides which
// lines are new (so an insert shifts the rest without reporting them), and only
// the resulting lines *of the new revision* are kept -- an edit shows as its new
// text, and a plain deletion shows nothing. Entries come out in line order because
// the walk visits the new revision front to back. Blank lines take part in the
// matching but are never emitted. MAX_DOC_LINES is small, so the (n+1)x(m+1) table
// is trivial.
static uint8_t lcs[MAX_DOC_LINES + 1][MAX_DOC_LINES + 1];

static void diffAgainstPrev() {
  if (prevValid) {
    int before = diffCount;
    for (int i = prevCount; i >= 0; i--)
      for (int j = lineCount; j >= 0; j--) {
        if (i == prevCount || j == lineCount) lcs[i][j] = 0;
        else if (prevLines[i] == lines[j]) lcs[i][j] = lcs[i + 1][j + 1] + 1;
        else lcs[i][j] = lcs[i + 1][j] > lcs[i][j + 1] ? lcs[i + 1][j] : lcs[i][j + 1];
      }
    int i = 0, j = 0;
    while (i < prevCount || j < lineCount) {
      if (i < prevCount && j < lineCount && prevLines[i] == lines[j]) {
        i++;  // unchanged line, present in both
        j++;
      } else if (i < prevCount && (j == lineCount || lcs[i + 1][j] >= lcs[i][j + 1])) {
        i++;  // dropped from the old revision -- nothing to show
      } else {
        if (lines[j].length()) diffAdd(j + 1, lines[j]);
        j++;
      }
    }
    if (diffCount != before)
      logInfo("Doc changed: %d new line(s), %d pending", diffCount - before, diffCount);
  }
  for (int i = 0; i < lineCount; i++) prevLines[i] = lines[i];
  for (int i = lineCount; i < prevCount; i++) prevLines[i] = "";  // release dropped lines
  prevCount = lineCount;
  prevValid = true;
}

int gdocDiffCount() {
  return diffCount;
}
const char *gdocDiffLine(int i) {
  if (i < 0 || i >= diffCount) return "";
  return diffLines[i].c_str();
}
bool gdocDiffTruncated() {
  return diffTrunc;
}
void gdocDiffClear() {
  // No lock: diffLines[] is written only by gdocCommit() and cleared here, both
  // on the loop task.
  for (int i = 0; i < diffCount; i++) diffLines[i] = "";
  diffCount = 0;
  diffTrunc = false;
}

void gdocFetch() {
  // Do not overwrite the single staged slot until the loop task has consumed it.
  if (pending.load(std::memory_order_acquire)) return;

  if (!wifiConnected() || docUrl.length() == 0) return;
  stagedHaveTitle = false;
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
    stagedOk = false;
    stagedLinesValid = false;  // keep whatever text is already on screen
    pending.store(1, std::memory_order_release);
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
      stagedTitle = sanitize(t);
      stagedHaveTitle = true;
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
  // Parse into the staging slot, never straight into lines[]: the loop task may
  // be rendering the doc view right now, and gdocLine() hands it c_str() pointers
  // into lines[]. Only gdocCommit(), on that same task, touches the live text.
  stagedCount = 0;
  int start = 0;
  while (start <= (int)payload.length() && stagedCount < MAX_DOC_LINES) {
    int nl = payload.indexOf('\n', start);
    String s = sanitize(nl < 0 ? payload.substring(start) : payload.substring(start, nl));
    bool prevBlank = (stagedCount > 0 && stagedLines[stagedCount - 1].length() == 0);
    if (s.length() > 0 || !prevBlank) stagedLines[stagedCount++] = s;  // skip a 2nd+ consecutive blank
    if (nl < 0) break;
    start = nl + 1;
  }
  while (stagedCount > 0 && stagedLines[stagedCount - 1].length() == 0) stagedCount--;  // drop trailing blanks

  stagedAsOf = time(nullptr);
  stagedOk = true;
  stagedLinesValid = true;
  pending.store(1, std::memory_order_release);
  logInfo("Doc fetched: %d lines", stagedCount);
}

// Promote the staged revision and diff it against the previous one. Runs on the
// loop task, the only reader of lines[]/diffLines[], so the swap needs no lock
// and the renderer never sees a half-replaced document.
bool gdocCommit() {
  if (!pending.load(std::memory_order_acquire)) return false;
  if (stagedLinesValid) {
    for (int i = 0; i < stagedCount; i++) lines[i] = stagedLines[i];
    for (int i = stagedCount; i < lineCount; i++) lines[i] = "";  // release dropped lines
    lineCount = stagedCount;
    if (stagedHaveTitle) title = stagedTitle;
    diffAgainstPrev();  // collect what changed vs. the last revision (drives the popup)
    asOf = stagedAsOf;
  }
  ok = stagedOk;
  pending.store(0, std::memory_order_release);
  return true;
}

// Convenience for the synchronous boot path and the web handlers, which run on
// the loop task and want the result straight away.
void gdocUpdate() {
  gdocFetch();
  gdocCommit();
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
