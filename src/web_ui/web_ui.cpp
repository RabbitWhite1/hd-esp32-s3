// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "web_ui.h"
#include "../claude_usage/claude_usage.h"  // configure org id + session key from the web form
#include "../sensors/sensors.h"            // live temp/humidity shown on the page
#include "../wifi_net/wifi_net.h"          // add/list saved Wi-Fi networks from the form
#include "../sdcard/sdcard.h"              // persist the to-do list to /sdcard/todo.md
#include "../gdoc/gdoc.h"                  // configure the Google Doc URL from the form
#include "../logging/logging.h"
#include <WebServer.h>

// Result of the most recent "add Wi-Fi" attempt, shown back on the page.
static String lastWifiMsg = "";

static const int MAX_TODOS = 8;

struct Todo {
  String text;
  bool done;
};

static WebServer server(80);
static Todo todos[MAX_TODOS];
static int todoCount = 0;

// Escape the few characters that would otherwise break the HTML we reflect the
// item text into (it lands in a text-input value attribute).
static String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

static void handleRoot() {
  String html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>hd panel</title>"
    "<style>body{font-family:sans-serif;font-size:1.1em;margin:1em}"
    "ul{padding:0}li{list-style:none;margin:.5em 0}"
    "input[type=text]{font-size:1em;width:70%}"
    "button{font-size:1em;margin:.4em .4em 0 0}</style>"
    "</head><body>";

  // Live temperature/humidity, read fresh on each page load.
  float tC = NAN, rh = NAN;
  bool haveSensor = sensorsPresent() && sensorsRead(&tC, &rh);
  html += "<p>Temperature: ";
  html += haveSensor ? (String(tC, 1) + " &deg;C") : String("--");
  html += " &nbsp; Humidity: ";
  html += haveSensor ? (String(rh, 1) + " %") : String("--");
  html += "</p>";

  html += "<h2>To-do</h2><form action='/save' method='POST'><ul>";
  for (int i = 0; i < todoCount; i++) {
    html += "<li><input type='checkbox' name='done";
    html += i;
    html += "'";
    if (todos[i].done) html += " checked";
    html += "> <input type='text' name='item";
    html += i;
    html += "' value='";
    html += htmlEscape(todos[i].text);
    html += "'> <button type='submit' name='action' value='del";
    html += i;
    html += "'>x</button></li>";
  }
  html += "</ul>";
  // "+" adds a blank row (a server round-trip that also preserves current edits).
  if (todoCount < MAX_TODOS)
    html += "<button type='submit' name='action' value='add'>+</button>";
  html += "<button type='submit' name='action' value='save'>Save</button></form>";

  // Google Doc URL shown in the Notes box (paste a normal Docs/sharing link; it's
  // reduced to the base doc URL and the txt export is fetched in code). Persisted
  // to /sdcard/gdoc_url.txt.
  html += "<hr><h2>Google Doc</h2><form action='/gdoc' method='POST'>"
          "<p>Doc URL:<br><input type='text' name='url' style='width:90%' value='";
  html += htmlEscape(gdocUrl());
  html += "'></p><button type='submit'>Save</button></form>";

  // Claude usage credentials (kept in RAM on the device, never in the firmware).
  // The stored session key is NEVER written into the page, so anyone on the LAN
  // can't read it from the source. Leave the field blank to keep the current key.
  html += "<hr><h2>Claude usage</h2><form action='/claude' method='POST'>"
          "<p>Org ID:<br><input type='text' name='org' value='";
  html += htmlEscape(claudeUsageOrgId());
  html += "'></p><p>Session key ";
  html += claudeUsageHasKey() ? "(set)" : "(not set)";
  html += ":<br><input type='text' name='key' style='width:70%' "
          "placeholder='leave blank to keep current'></p>"
          "<button type='submit'>Save</button></form>";

  // Wi-Fi: list the saved (known-good) networks and add a new one. A network is
  // only stored once the device has confirmed it can actually connect to it.
  html += "<hr><h2>Wi-Fi</h2>";
  if (lastWifiMsg.length()) {
    html += "<p><b>";
    html += htmlEscape(lastWifiMsg);
    html += "</b></p>";
  }
  html += "<p>Saved networks (top = tried first):</p><ol>";
  if (wifiNetCount() == 0) html += "<li>(none)</li>";
  for (int i = 0; i < wifiNetCount(); i++) {
    html += "<li>";
    // One inline form per row (before the name): raise/lower priority, or remove.
    html += "<form style='display:inline' action='/wifiedit' method='POST'>"
            "<input type='hidden' name='idx' value='";
    html += i;
    html += "'>"
            "<button name='act' value='up'>&uarr;</button>"
            "<button name='act' value='down'>&darr;</button>"
            "<button name='act' value='del'>Remove</button>"
            "</form> ";
    html += htmlEscape(wifiNetSSID(i));
    html += "</li>";
  }
  html += "</ol>";
  // Up/down only reorder in RAM; this button persists the order to the SD card.
  html += "<form action='/wifisave' method='POST'><button type='submit'>Save order</button></form>";
  html += "<form action='/wifi' method='POST'>"
          "<p>SSID:<br><input type='text' name='ssid'></p>"
          "<p>Password:<br><input type='text' name='pass'></p>"
          "<button type='submit'>Add (tests before saving)</button></form>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

static void handleWifi() {
  String s = server.hasArg("ssid") ? server.arg("ssid") : "";
  String p = server.hasArg("pass") ? server.arg("pass") : "";
  // Note: testing a new network drops the current link, so this HTTP response
  // may not reach the browser; reconnect via http://esp32.local/ afterwards.
  bool ok = wifiAddNetwork(s, p);
  lastWifiMsg = ok ? ("Connected & saved: " + s) : ("Could not connect to '" + s + "' - not saved");
  logInfo("WiFi add via web: %s -> %s", s.c_str(), ok ? "saved" : "rejected");
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleWifiEdit() {
  int idx = server.hasArg("idx") ? server.arg("idx").toInt() : -1;
  String act = server.hasArg("act") ? server.arg("act") : "";
  if (act == "del") {
    String s = wifiNetSSID(idx);  // capture the name before removal
    lastWifiMsg = wifiRemoveNetwork(s) ? ("Removed: " + s) : "Remove failed";
  } else if (act == "up" && wifiMoveNetwork(idx, -1)) {
    lastWifiMsg = "Moved up - click 'Save order' to keep it";
  } else if (act == "down" && wifiMoveNetwork(idx, 1)) {
    lastWifiMsg = "Moved down - click 'Save order' to keep it";
  }
  logInfo("WiFi edit via web: idx=%d act=%s", idx, act.c_str());
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleWifiSave() {
  wifiSaveNetworks();
  lastWifiMsg = "Priority order saved";
  logInfo("WiFi priority order saved via web");
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleClaude() {
  if (server.hasArg("org")) claudeUsageSetOrgId(server.arg("org"));
  if (server.hasArg("key")) claudeUsageSetSessionKey(server.arg("key"));  // empty -> keep current
  logInfo("Claude credentials updated via web UI");
  claudeUsageUpdate();  // refresh now so the result shows on the LCD immediately
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleGdoc() {
  if (server.hasArg("url")) {
    gdocSetUrl(server.arg("url"));  // normalizes a Docs link to the txt export
    gdocSaveUrl();                  // persist to /sdcard/gdoc_url.txt
    logInfo("gdoc URL updated via web UI");
    gdocUpdate();  // refresh the Notes box now
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

// Persist the to-do list to the SD card as a markdown checklist:
//   - [x] done item
//   - [ ] open item
static void todoSave() {
  String md;
  for (int i = 0; i < todoCount; i++) {
    md += todos[i].done ? "- [x] " : "- [ ] ";
    md += todos[i].text;
    md += '\n';
  }
  sdWriteText("todo.md", md);
}

// Load the to-do list back from /sdcard/todo.md (parses the markdown checklist).
static void todoLoad() {
  String data = sdReadText("todo.md");
  todoCount = 0;
  int start = 0;
  while (start < (int)data.length() && todoCount < MAX_TODOS) {
    int nl = data.indexOf('\n', start);
    String line = (nl < 0) ? data.substring(start) : data.substring(start, nl);
    int b = line.indexOf("- [");  // tolerate leading whitespace
    if (b >= 0 && (int)line.length() >= b + 5 && line[b + 4] == ']') {
      char mark = line[b + 3];
      String text = ((int)line.length() > b + 5) ? line.substring(b + 5) : String("");
      text.trim();
      todos[todoCount].done = (mark == 'x' || mark == 'X');
      todos[todoCount].text = text;  // empty items are kept
      todoCount++;
    }
    if (nl < 0) break;
    start = nl + 1;
  }
  logInfo("To-do loaded from SD (%d items)", todoCount);
}

// Rebuild the list from the submitted form fields (item0..itemN / done0..doneN).
static void rebuildFromArgs() {
  todoCount = 0;
  for (int i = 0; i < MAX_TODOS; i++) {
    String key = "item" + String(i);
    if (!server.hasArg(key)) continue;
    todos[todoCount].text = server.arg(key);  // WebServer URL-decodes form args
    todos[todoCount].done = server.hasArg("done" + String(i));
    todoCount++;
  }
}

static void handleSave() {
  rebuildFromArgs();
  String action = server.hasArg("action") ? server.arg("action") : "save";
  if (action == "add") {
    if (todoCount < MAX_TODOS) {
      todos[todoCount].text = "";
      todos[todoCount].done = false;
      todoCount++;
    }
  } else if (action.startsWith("del")) {
    // Remove the item at the given index (rebuildFromArgs kept the other edits).
    int idx = action.substring(3).toInt();
    if (idx >= 0 && idx < todoCount) {
      for (int j = idx; j < todoCount - 1; j++) todos[j] = todos[j + 1];
      todoCount--;
    }
    todoSave();  // removal is durable
    logInfo("To-do item %d removed (%d left)", idx, todoCount);
  } else {
    // Save the list as-is; empty items are kept.
    todoSave();  // persist to /sdcard/todo.md
    logInfo("To-do saved (%d items)", todoCount);
  }
  server.sendHeader("Location", "/");
  server.send(303);  // See Other -> browser re-GETs "/"
}

void webBegin() {
  todoLoad();  // restore the to-do list from SD (requires sdBegin() earlier in setup)
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/claude", HTTP_POST, handleClaude);
  server.on("/gdoc", HTTP_POST, handleGdoc);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/wifiedit", HTTP_POST, handleWifiEdit);
  server.on("/wifisave", HTTP_POST, handleWifiSave);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  logInfo("Web UI listening on port 80");
}

void webHandle() {
  server.handleClient();
}

int webTodoCount() {
  return todoCount;
}

const char *webTodoText(int i) {
  if (i < 0 || i >= todoCount) return "";
  return todos[i].text.c_str();
}

bool webTodoDone(int i) {
  if (i < 0 || i >= todoCount) return false;
  return todos[i].done;
}
