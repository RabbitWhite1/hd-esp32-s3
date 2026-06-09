// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "web_ui.h"
#include "logging.h"
#include <WebServer.h>

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
    "<title>hd to-do</title>"
    "<style>body{font-family:sans-serif;font-size:1.1em;margin:1em}"
    "ul{padding:0}li{list-style:none;margin:.5em 0}"
    "input[type=text]{font-size:1em;width:70%}"
    "button{font-size:1em;margin:.4em .4em 0 0}</style>"
    "</head><body><h2>To-do</h2>"
    "<form action='/save' method='POST'><ul>";
  for (int i = 0; i < todoCount; i++) {
    html += "<li><input type='checkbox' name='done";
    html += i;
    html += "'";
    if (todos[i].done) html += " checked";
    html += "> <input type='text' name='item";
    html += i;
    html += "' value='";
    html += htmlEscape(todos[i].text);
    html += "'></li>";
  }
  html += "</ul>";
  // "+" adds a blank row (a server round-trip that also preserves current edits).
  if (todoCount < MAX_TODOS)
    html += "<button type='submit' name='action' value='add'>+</button>";
  html += "<button type='submit' name='action' value='save'>Save</button>"
          "</form></body></html>";
  server.send(200, "text/html", html);
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
  } else {
    // Save: drop blank-text items so the LCD list stays clean.
    int w = 0;
    for (int i = 0; i < todoCount; i++) {
      if (todos[i].text.length() > 0) {
        if (w != i) todos[w] = todos[i];
        w++;
      }
    }
    todoCount = w;
    logInfo("To-do saved (%d items)", todoCount);
  }
  server.sendHeader("Location", "/");
  server.send(303);  // See Other -> browser re-GETs "/"
}

void webBegin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
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
