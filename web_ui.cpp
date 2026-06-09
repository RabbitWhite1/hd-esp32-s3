// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "web_ui.h"
#include "logging.h"
#include <WebServer.h>

static WebServer server(80);
static String userMessage = "";

// Escape the few characters that would otherwise break the HTML we embed the
// message into (it is reflected back into both an attribute value and the body).
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
  String safe = htmlEscape(userMessage);
  String html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>hd display</title></head><body>"
    "<h2>Send a message to the display</h2>"
    "<form action='/set' method='POST'>"
    "<input name='msg' maxlength='64' autofocus "
    "style='font-size:1.2em;width:80%' value='";
  html += safe;
  html += "'><p><button type='submit' style='font-size:1.2em'>Send</button></p>"
          "</form><p>Currently showing: <b>";
  html += safe;
  html += "</b></p></body></html>";
  server.send(200, "text/html", html);
}

static void handleSet() {
  if (server.hasArg("msg")) {
    userMessage = server.arg("msg");  // WebServer URL-decodes form args for us
    logInfo("Web message set: %s", userMessage.c_str());
  }
  server.sendHeader("Location", "/");
  server.send(303);  // See Other -> browser re-GETs "/"
}

void webBegin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/set", HTTP_POST, handleSet);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  logInfo("Web UI listening on port 80");
}

void webHandle() {
  server.handleClient();
}

const String &webMessage() {
  return userMessage;
}
