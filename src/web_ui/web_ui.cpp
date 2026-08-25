// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "web_ui.h"
#include "../claude_usage/claude_usage.h"  // configure org id + session key from the web form
#include "../codex_usage/codex_usage.h"    // receive the relayed Codex access token
#include "../sensors/sensors.h"            // live temp/humidity shown on the page
#include "../wifi_net/wifi_net.h"          // add/list saved Wi-Fi networks from the form
#include "../weather/weather.h"            // add/remove weather cities (geocoded) from the form
#include "../sdcard/sdcard.h"              // persist the to-do list to /sdcard/todo.md
#include "../asset_cache/asset_cache.h"    // serve Bootstrap from SD (offline-capable)
#include "../history/history.h"            // temp/humidity ring buffer for the NOW chart
#include "../gdoc/gdoc.h"                  // configure the Google Doc URL from the form
#include "../time_sync/time_sync.h"        // select primary/secondary time zones from the form
#include "../logging/logging.h"
#include "favicon.h"                       // embedded 32x32 ICO favicon
#include <WebServer.h>
#include <time.h>

static WebServer server(80);

// One-shot "flash" message shown as a Bootstrap toast on the next page load
// (success = green, failure = red). Set by the POST handlers; rendered and
// cleared by handleRoot so each message pops exactly once.
static String flashMsg = "";
static bool flashOk = true;
static void flash(bool ok, const String &msg) {
  flashOk = ok;
  flashMsg = msg;
}

// Escape a string for embedding inside a JSON double-quoted value.
static String jsonEscape(const String &in) {
  String o;
  o.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else {
          o += c;
        }
    }
  }
  return o;
}

// Finish a POST handler: a fetch()/AJAX caller (sends X-Requested-With) gets a
// small JSON result so the page can toast + refresh in place without reloading;
// a plain form post falls back to the classic flash + redirect-to-"/".
static void respond(bool ok, const String &msg) {
  if (server.hasHeader("X-Requested-With")) {
    String j = "{\"ok\":";
    j += ok ? "true" : "false";
    j += ",\"msg\":\"";
    j += jsonEscape(msg);
    j += "\"}";
    server.send(200, "application/json", j);
  } else {
    flash(ok, msg);
    server.sendHeader("Location", "/");
    server.send(303);
  }
}

static const int MAX_TODOS = 8;

struct Todo {
  String text;
  bool done;
};

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

// Open/close a Bootstrap "card" section with a titled body.
// Open a card. `right` is optional HTML placed at the top-right of the header
// (used for a compact "Save" button so it doesn't eat a row at the bottom).
static String cardOpen(const char *id, const char *title, const String &right = "") {
  String s = "<div class='card shadow-sm mb-3' id='";
  s += id;
  s += "'><div class='card-body'>"
       "<div class='d-flex justify-content-between align-items-center mb-3'>"
       "<h2 class='h5 card-title m-0'>";
  s += title;
  s += "</h2>";
  s += right;
  s += "</div>";
  return s;
}
static const char *cardClose = "</div></div>";

// A small Save button that submits the form with the given id (via the HTML
// `form` attribute), so it can live in the card header instead of inline.
static String saveBtn(const char *formId) {
  String s = "<button class='btn btn-sm btn-primary' form='";
  s += formId;
  s += "'>Save</button>";
  return s;
}

// Assets load from the CDN first (the browser gets a fresh copy); the cached SD
// copy at <route> is only a fallback for when the CDN is unreachable (offline).
static String assetCdn(const char *route) {
  const CachedAsset *a = assetByRoute(route);
  return a ? String(a->url) : String(route);
}
// <link> from the CDN; on load error, swap to the local cached copy.
static String cssLink(const char *route) {
  return "<link rel='stylesheet' href='" + assetCdn(route) +
         "' onerror=\"this.onerror=null;this.href='" + route + "'\">";
}
// <script> from the CDN; if its global didn't define, synchronously document.write
// the local cached copy as a fallback (runs during initial parse, preserving order).
static String jsScript(const char *route, const char *glob) {
  return "<script src='" + assetCdn(route) + "'></script>"
         "<script>window." + glob + "||document.write('<script src=\"" + route +
         "\"><\\/script>')</script>";
}

// Minimal, dependency-free page served at "/" while the device is in SoftAP
// setup mode (no saved network reachable, so no internet to load Bootstrap). A
// phone joins the open setup AP, picks a scanned network (or types one), enters
// the password, and submits to /wifi — which tests + saves it, then drops the AP.
static void handleSetup() {
  int n = wifiScan();  // blocks a couple seconds; fine for a one-off setup page
  String h =
    "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<link rel='icon' type='image/x-icon' href='/favicon.ico'>"
    "<title>Wi-Fi setup</title><style>"
    "body{font-family:system-ui,Arial,sans-serif;max-width:24rem;margin:2rem auto;padding:0 1rem;color:#222}"
    "h1{font-size:1.4rem}label{display:block;margin:1rem 0 .25rem;font-weight:600}"
    "input,button{width:100%;box-sizing:border-box;padding:.55rem;font-size:1rem;border:1px solid #bbb;border-radius:.375rem}"
    "button{margin-top:1.25rem;background:#0d6efd;color:#fff;border:0;font-weight:600}"
    ".muted{color:#666;font-size:.85rem}a{color:#0d6efd}"
    "</style></head><body>"
    "<h1>Wi-Fi setup</h1>"
    "<p class='muted'>Choose your network and enter its password. The device tests "
    "the connection and saves it only if it works, then leaves this hotspot.</p>"
    "<form method='POST' action='/wifi'>"
    "<label>Network</label>"
    "<input name='ssid' list='nets' placeholder='Network name' autocomplete='off'>"
    "<datalist id='nets'>";
  for (int i = 0; i < n; i++) {
    h += "<option value='";
    h += htmlEscape(wifiScanSSID(i));
    h += "'>";
  }
  h += "</datalist>"
       "<label>Password</label>"
       "<input name='pass' placeholder='Password'>"
       "<button type='submit'>Connect</button>"
       "</form>"
       "<p class='muted' style='margin-top:1.5rem'>";
  h += n;
  h += " network(s) found &middot; <a href='/'>rescan</a></p>"
       "</body></html>";
  server.send(200, "text/html", h);
}

static void handleRoot() {
  if (wifiInSetupMode()) {  // no saved network joined: show the minimal setup page
    handleSetup();
    return;
  }
  String html =
    "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>hd panel</title>"
    "<link rel='icon' type='image/x-icon' href='/favicon.ico'>";
  html += cssLink("/bootstrap.css");
  html +=
    "<style>html{scroll-behavior:smooth}.card{scroll-margin-top:7rem}"
    "@media(max-width:767px){.sidenav{position:static!important}}</style>"
    "</head><body class='bg-body-tertiary'>"
    "<div id='app' class='container pb-4' style='max-width:980px'>"
    // Sticky header: the title + the two top-level tabs (Dashboard / Configuration)
    // stay pinned at the top together while the page scrolls.
    "<div class='sticky-top bg-body-tertiary pt-3 mb-3'>"
    "<h1 class='h3 mb-2'>hd panel</h1>"
    "<ul class='nav nav-tabs' id='maintabs' role='tablist'>"
    "<li class='nav-item'><button class='nav-link active' type='button' role='tab' "
    "data-bs-toggle='tab' data-bs-target='#tab-dashboard'>Dashboard</button></li>"
    "<li class='nav-item'><button class='nav-link' type='button' role='tab' "
    "data-bs-toggle='tab' data-bs-target='#tab-config'>Configuration</button></li>"
    "</ul></div><div class='tab-content'>"
    // --- Dashboard tab: Now + To-do ---
    "<div class='tab-pane fade show active' id='tab-dashboard' role='tabpanel'>"
    "<div class='row g-4'>"
    "<div class='col-12 col-md-3'>"
    "<nav class='sidenav nav flex-column position-sticky' style='top:7rem'>"
    "<a class='nav-link' href='#now'>Now</a>"
    "<a class='nav-link' href='#todo'>To-do</a>"
    "</nav></div>"
    "<div class='col-12 col-md-9'>";

  // Live temperature/humidity, read fresh on each page load.
  float tC = NAN, rh = NAN;
  bool haveSensor = sensorsPresent() && sensorsRead(&tC, &rh);
  html += cardOpen("now", "Now");
  html += "<div class='d-flex gap-5'>"
          "<div><div class='text-muted small'>Temperature</div><div class='fs-4'>";
  html += haveSensor ? (String(tC, 1) + " &deg;C") : String("--");
  html += "</div></div><div><div class='text-muted small'>Humidity</div><div class='fs-4'>";
  html += haveSensor ? (String(rh, 1) + " %") : String("--");
  html += "</div></div></div>";
  // Trend chart (temperature + humidity), averaged over a selectable range +
  // granularity. JS fills the inputs (local time) and queries /history.
  html += "<div class='row g-2 align-items-end mt-2'>"
          "<div class='col-auto'><label class='form-label mb-0 small'>From</label>"
          "<input type='datetime-local' id='hfrom' class='form-control form-control-sm'></div>"
          "<div class='col-auto'><label class='form-label mb-0 small'>To</label>"
          "<input type='datetime-local' id='hto' class='form-control form-control-sm'></div>"
          "<div class='col-auto'><label class='form-label mb-0 small'>Bucket</label>"
          "<select id='hbucket' class='form-select form-select-sm'>"
          "<option value='minutely' selected>Minutely</option>"
          "<option value='hourly'>Hourly</option>"
          "<option value='daily'>Daily</option>"
          "<option value='weekly'>Weekly</option>"
          "<option value='monthly'>Monthly</option></select></div>"
          "<div class='col-auto'><button id='happly' class='btn btn-sm btn-primary'>Apply</button></div>"
          "</div>"
          "<div class='mt-3' style='height:200px'><canvas id='thchart_t'></canvas></div>"
          "<div class='mt-3' style='height:200px'><canvas id='thchart_h'></canvas></div>";
  html += cardClose;

  html += cardOpen("todo", "To-do",
                   "<button id='todosavebtn' class='btn btn-sm btn-primary' form='todoform' "
                   "name='action' value='save'>Save</button>");
  html += "<div class='text-muted small mb-2'>Drag to reorder; click Save to keep changes</div>"
          "<form id='todoform' action='/save' method='POST'>"
          "<input type='hidden' name='order' id='todoorderinput'>"
          "<div id='todolist'>";
  for (int i = 0; i < todoCount; i++) {
    html += "<div class='input-group mb-2' data-idx='";
    html += i;
    html += "'><span class='input-group-text drag-handle' style='cursor:grab' "
            "title='Drag to reorder'>&#x2630;</span><div class='input-group-text'>"
            "<input class='form-check-input mt-0' type='checkbox' name='done";
    html += i;
    html += "'";
    if (todos[i].done) html += " checked";
    html += "></div><input type='text' class='form-control' name='item";
    html += i;
    html += "' value='";
    html += htmlEscape(todos[i].text);
    html += "'><button class='btn btn-outline-danger' type='submit' name='action' value='del";
    html += i;
    html += "'>&times;</button></div>";
  }
  html += "</div>";
  // "+" adds a blank row (a server round-trip that also preserves current edits).
  if (todoCount < MAX_TODOS)
    html += "<button class='btn btn-outline-secondary' type='submit' name='action' value='add'>+ Add row</button>";
  html += "</form>";
  html += cardClose;

  // End of the Dashboard tab; open the Configuration tab (all the settings
  // cards) with its own sticky navigator.
  html += "</div></div></div>"  // /content col, /row, /#tab-dashboard
          "<div class='tab-pane fade' id='tab-config' role='tabpanel'>"
          "<div class='row g-4'>"
          "<div class='col-12 col-md-3'>"
          "<nav class='sidenav nav flex-column position-sticky' style='top:7rem'>"
          "<a class='nav-link' href='#gdoc'>Google Doc</a>"
          "<a class='nav-link' href='#tz'>Time zones</a>"
          "<a class='nav-link' href='#weather'>Weather cities</a>"
          "<a class='nav-link' href='#intervals'>Refresh intervals</a>"
          "<a class='nav-link' href='#claude'>Claude usage</a>"
          "<a class='nav-link' href='#codex'>Codex usage</a>"
          "<a class='nav-link' href='#wifi'>Wi-Fi</a>"
          "</nav></div>"
          "<div class='col-12 col-md-9'>";

  // Google Doc URL shown in the Notes box (paste a normal Docs/sharing link; it's
  // reduced to the base doc URL and the txt export is fetched in code). Persisted
  // in config (esp32.json).
  html += cardOpen("gdoc", "Google Doc", saveBtn("gdocform"));
  html += "<form id='gdocform' action='/gdoc' method='POST'>"
          "<label class='form-label'>Doc URL</label>"
          "<input type='text' class='form-control' name='url' value='";
  html += htmlEscape(gdocUrl());
  html += "'></form>";
  html += cardClose;

  // Time zones: two dropdowns (primary shown first, secondary in parentheses on
  // the LCD). Each option is an abbreviation + a famous city in that zone. The
  // selection is persisted in config (esp32.json).
  html += cardOpen("tz", "Time zones", saveBtn("tzform"));
  html += "<form id='tzform' action='/tz' method='POST'><div class='row g-3'>";
  for (int sel = 0; sel < 2; sel++) {
    int cur = sel == 0 ? timePrimaryZone() : timeSecondaryZone();
    html += "<div class='col-6'><label class='form-label'>";
    html += sel == 0 ? "Primary" : "Secondary";
    html += "</label><select class='form-select' name='";
    html += sel == 0 ? "primary" : "secondary";
    html += "'>";
    for (int z = 0; z < timeZoneCount(); z++) {
      html += "<option value='";
      html += z;
      html += "'";
      if (z == cur) html += " selected";
      html += ">";
      html += htmlEscape(timeZoneLabel(z));
      html += "</option>";
    }
    html += "</select></div>";
  }
  html += "</div></form>";
  html += cardClose;

  // Weather cities: add by name (geocoded to coordinates), remove, reorder by
  // priority, persisted in config (esp32.json). Only the top weatherShownMax()
  // are shown on the LCD + fetched. Reordering mirrors the Wi-Fi panel.
  html += cardOpen("weather", "Weather cities");
  html += "<div class='text-muted small mb-2'>Top ";
  html += weatherShownMax();
  html += " are shown on the device &mdash; drag to reorder</div>"
          "<ul class='list-group mb-2' id='citylist'>";
  if (weatherCityCount() == 0) html += "<li class='list-group-item text-muted'>(none)</li>";
  for (int i = 0; i < weatherCityCount(); i++) {
    html += "<li class='list-group-item d-flex justify-content-between align-items-center' data-idx='";
    html += i;
    html += "'><span><span class='drag-handle me-2' style='cursor:grab' title='Drag to reorder'>"
            "&#x2630;</span>";
    html += htmlEscape(weatherCityName(i));
    html += "</span>"
            "<form action='/weatheredit' method='POST' class='m-0 btn-group btn-group-sm'>"
            "<input type='hidden' name='idx' value='";
    html += i;
    html += "'>"
            "<button class='btn btn-outline-secondary' name='act' value='top' "
            "title='Move to top'>&#x2912;</button>"
            "<button class='btn btn-outline-danger' name='act' value='del'>Remove</button>"
            "</form></li>";
  }
  html += "</ul>";
  // Save order (drag/move-to-top change the browser/RAM only until this is clicked).
  html += "<form id='citysaveform' action='/weatherorder' method='POST' class='mb-3'>"
          "<input type='hidden' name='order' id='cityorderinput'>"
          "<button id='citysavebtn' class='btn btn-sm ";
  html += weatherOrderDirty() ? "btn-primary'" : "btn-outline-secondary' disabled";
  html += ">Save order</button></form>";
  html += "<hr>";  // separate the list/order section from adding a city
  if (weatherCityCount() < weatherMaxCities()) {
    html += "<form action='/weatheradd' method='POST' class='row g-2'>"
            "<div class='col'><input class='form-control' type='text' name='city' placeholder='e.g. Tokyo'></div>"
            "<div class='col-auto'><button class='btn btn-primary'>Add</button></div></form>"
            // Where to look up the exact name the geocoder recognizes.
            "<div class='form-text'>Not found? Look up valid names at "
            "<a href='https://open-meteo.com/en/docs/geocoding-api' target='_blank' rel='noopener'>"
            "Open-Meteo geocoding</a>.</div>";
  } else {
    html += "<p class='text-muted mb-0'>(max ";
    html += weatherMaxCities();
    html += " cities)</p>";
  }
  html += cardClose;

  // Auto-refresh intervals (minutes), persisted to esp32.json. Each input shows a
  // non-editable light-grey "min" suffix.
  html += cardOpen("intervals", "Refresh intervals", saveBtn("intervalsform"));
  html += "<form id='intervalsform' action='/intervals' method='POST' class='row g-3'>"
          "<div class='col-sm-4'><label class='form-label'>Claude usage</label>"
          "<div class='input-group'><input type='number' class='form-control' name='claude' min='1' value='";
  html += claudeUsageIntervalMin();
  html += "'><span class='input-group-text text-muted'>min</span></div></div>"
          "<div class='col-sm-4'><label class='form-label'>Codex usage</label>"
          "<div class='input-group'><input type='number' class='form-control' name='codex' min='1' value='";
  html += codexUsageIntervalMin();
  html += "'><span class='input-group-text text-muted'>min</span></div></div>"
          "<div class='col-sm-4'><label class='form-label'>Google Doc</label>"
          "<div class='input-group'><input type='number' class='form-control' name='gdoc' min='1' value='";
  html += gdocIntervalMin();
  html += "'><span class='input-group-text text-muted'>min</span></div></div></form>";
  html += cardClose;

  // Claude usage credentials (kept in RAM on the device, never in the firmware).
  // Two ways to set them, in tabs; the header Save button submits the active
  // tab's form (default: the cookie paste). The how-to sits outside the tabs so
  // it's shown for both.
  html += cardOpen("claude", "Claude usage",
                   "<button id='claudesave' class='btn btn-sm btn-primary' form='claudeform_cookie'>Save</button>");
  html += "<ul class='nav nav-tabs mb-3' role='tablist'>"
          "<li class='nav-item'><button class='nav-link active' type='button' role='tab' "
          "data-bs-toggle='tab' data-bs-target='#claude-cookie' data-form='claudeform_cookie'>Cookie</button></li>"
          "<li class='nav-item'><button class='nav-link' type='button' role='tab' "
          "data-bs-toggle='tab' data-bs-target='#claude-fields' data-form='claudeform_fields'>Org ID + key</button></li>"
          "</ul><div class='tab-content'>";

  // Cookie tab (default): paste the whole browser Cookie header; the device pulls
  // out sessionKey (and the org id from lastActiveOrg).
  html += "<div class='tab-pane fade show active' id='claude-cookie' role='tabpanel'>"
          "<form id='claudeform_cookie' action='/claude' method='POST'>"
          "<label class='form-label'>Paste full cookie</label>"
          "<textarea class='form-control' name='cookie' rows='4' "
          "placeholder='anthropic-device-id=...; sessionKey=sk-ant-...; lastActiveOrg=...; ...'></textarea>"
          "<div class='form-text'>The device extracts <code>sessionKey</code> (and the org id from "
          "<code>lastActiveOrg</code>).</div></form></div>";

  // Org ID + session key tab. The stored session key is NEVER written into the
  // page, so anyone on the LAN can't read it from the source; leave the key field
  // blank to keep the current one.
  html += "<div class='tab-pane fade' id='claude-fields' role='tabpanel'>"
          "<form id='claudeform_fields' action='/claude' method='POST'>"
          "<label class='form-label'>Org ID</label>"
          "<input type='text' class='form-control mb-3' name='org' value='";
  html += htmlEscape(claudeUsageOrgId());
  html += "'><label class='form-label'>Session key ";
  html += claudeUsageHasKey() ? "<span class='badge text-bg-success'>set</span>"
                              : "<span class='badge text-bg-secondary'>not set</span>";
  html += "</label><input type='text' class='form-control' name='key' "
          "placeholder='leave blank to keep current'></form></div>";

  html += "</div>";  // /tab-content

  // How-to (outside the tabs, shown for both methods).
  html += "<details class='mt-3'><summary class='text-primary' style='cursor:pointer'>"
          "How do I get the cookie?</summary>"
          "<ol class='form-text mb-0 mt-2'>"
          "<li>Open <a href='https://claude.ai' target='_blank' rel='noopener'>claude.ai</a> and sign in.</li>"
          "<li>Press <kbd>F12</kbd> to open the browser developer tools.</li>"
          "<li>Switch to the <strong>Network</strong> tab.</li>"
          "<li>In Claude, open <strong>Settings &rarr; Usage</strong> (so a usage request fires).</li>"
          "<li>In the Network tab, click the <code>usage</code> request.</li>"
          "<li>Under <strong>Headers &rarr; Request Headers</strong>: copy the whole <code>Cookie</code> value "
          "for the Cookie tab &mdash; or read off <code>sessionKey</code> and <code>lastActiveOrg</code> "
          "for the Org ID + key tab.</li>"
          "</ol></details>";
  html += cardClose;

  // Codex usage. Unlike the Claude card there is nothing to copy out of a browser:
  // the ChatGPT access token is minted by the `codex` CLI and expires in ~10 days,
  // so the machine running Codex relays it here (cron one-liner in the README) and
  // the device fetches its own usage with it. The paste box is the manual fallback.
  html += cardOpen("codex", "Codex usage", saveBtn("codexform"));
  html += "<p class='mb-2'>Access token ";
  if (!codexUsageHasToken()) {
    html += "<span class='badge text-bg-secondary'>not set</span>";
  } else if (codexTokenExpired()) {
    html += "<span class='badge text-bg-danger'>expired</span>";
  } else {
    html += "<span class='badge text-bg-success'>set</span>";
  }
  // Expiry is read from the token's own "exp" claim, so the page can say when the
  // relay needs to run again rather than waiting for a 401.
  time_t exp = codexTokenExpiry();
  if (exp > 0) {
    struct tm expTm;
    localtime_r(&exp, &expTm);
    char expStr[40];
    strftime(expStr, sizeof(expStr), "%Y-%m-%d %H:%M", &expTm);
    html += "<span class='text-muted small ms-2'>expires ";
    html += expStr;
    time_t now = time(nullptr);
    if (now > 1600000000 && exp > now) {
      html += " (in ";
      html += (int)((exp - now) / 86400);
      html += "d)";
    }
    html += "</span>";
  }
  html += "</p>";
  // Nothing can be fetched without a live token, so say what to do about it and
  // name the README section that sets the relay up (its cron is in the details
  // block below, so the fix is reachable without leaving the page).
  if (!codexUsageHasToken() || codexTokenExpired()) {
    html += "<div class='alert alert-warning py-2'>";
    html += codexUsageHasToken() ? "The access token has expired, so usage can't be fetched. "
                                 : "No access token yet, so usage can't be fetched. ";
    html += "Set up the hourly relay &mdash; see <strong>Codex usage relay</strong> in the "
            "README, or use <em>Keep it fresh automatically</em> below &mdash; or paste a "
            "token here now.</div>";
  }
  // The stored token is never written into the page, so it can't be read back off
  // the LAN; leave the box blank to keep the current one.
  html += "<form id='codexform' action='/codex' method='POST'>"
          "<label class='form-label'>Paste access token</label>"
          "<textarea class='form-control' name='token' rows='3' "
          "placeholder='eyJhbGciOi... (leave blank to keep current)'></textarea>"
          "<div class='form-text'>From <code>~/.codex/auth.json</code> &rarr; "
          "<code>tokens.access_token</code> on the machine you run Codex on.</div></form>";
  html += "<details class='mt-3'><summary class='text-primary' style='cursor:pointer'>"
          "Keep it fresh automatically</summary>"
          "<p class='form-text mb-2 mt-2'>Codex re-mints the token every time you use it. "
          "This hourly cron relays whatever the CLI last stored, so the device stays "
          "current without any manual step:</p>"
          "<pre class='form-text bg-body-secondary p-2 rounded' "
          "style='white-space:pre-wrap;word-break:break-all'><code>";
  // Single-quoted so $(...) is evaluated by cron at run time, not when installing;
  // no process substitution, since cron runs the line under /bin/sh.
  html += "(crontab -l 2>/dev/null; echo '0 * * * * curl -sf -X POST -d "
          "token=$(jq -r .tokens.access_token $HOME/.codex/auth.json) http://";
  html += wifiHostname();
  html += ".local/codextoken') | crontab -";
  html += "</code></pre>"
          "<p class='form-text mb-0'>Remove it again with "
          "<code>crontab -l | grep -v codextoken | crontab -</code>.</p></details>";
  html += cardClose;

  // Wi-Fi: list the saved (known-good) networks and add a new one. A network is
  // only stored once the device has confirmed it can actually connect to it.
  html += cardOpen("wifi", "Wi-Fi");
  html += "<div class='text-muted small mb-2'>Saved networks (top = tried first) "
          "&mdash; drag to reorder</div>"
          "<ul class='list-group mb-2' id='wifilist'>";
  if (wifiNetCount() == 0) html += "<li class='list-group-item text-muted'>(none)</li>";
  for (int i = 0; i < wifiNetCount(); i++) {
    html += "<li class='list-group-item d-flex justify-content-between align-items-center' data-idx='";
    html += i;
    html += "'><span><span class='drag-handle me-2' style='cursor:grab' title='Drag to reorder'>"
            "&#x2630;</span>";
    html += htmlEscape(wifiNetSSID(i));
    html += "</span>";
    // One inline form per row: raise/lower priority, or remove (touch/keyboard fallback).
    html += "<form action='/wifiedit' method='POST' class='m-0 btn-group btn-group-sm'>"
            "<input type='hidden' name='idx' value='";
    html += i;
    html += "'>"
            "<button class='btn btn-outline-secondary' name='act' value='top' "
            "title='Move to top'>&#x2912;</button>"
            "<button class='btn btn-outline-danger' name='act' value='del'>Remove</button>"
            "</form></li>";
  }
  html += "</ul>";
  // Dragging only rearranges the list in the browser; this button is what sends
  // the order to the device (apply + persist to SD). The hidden field is filled
  // with the current drag order at submit time. It lights up (primary) once the
  // order has unsaved changes (the per-row "move to top" also marks it dirty).
  html += "<form id='wifisaveform' action='/wifisave' method='POST' class='mb-3'>"
          "<input type='hidden' name='order' id='wifiorderinput'>"
          "<button id='wifisavebtn' class='btn btn-sm ";
  html += wifiOrderDirty() ? "btn-primary'" : "btn-outline-secondary' disabled";
  html += ">Save order</button></form>";
  html += "<hr>";  // separate the saved-list/order section from adding a network
  // SSID + password inputs and both add buttons on one row.
  // 'Add' stores without testing; 'Test then Add' verifies the join first
  // (which briefly drops the current link) and only saves on success.
  html += "<form action='/wifi' method='POST' class='row g-2 align-items-end'>"
          "<div class='col'><label class='form-label small mb-1'>SSID</label>"
          "<input type='text' class='form-control' name='ssid'></div>"
          "<div class='col'><label class='form-label small mb-1'>Password</label>"
          "<input type='text' class='form-control' name='pass'></div>"
          "<div class='col-auto'><button class='btn btn-outline-primary' name='mode' value='store'>Add</button></div>"
          "<div class='col-auto'><button class='btn btn-primary' name='mode' value='test'>Test then Add</button></div>"
          "</form>";
  html += cardClose;

  html += "</div></div></div>"  // /content col, /row, /#tab-config
          "</div>"              // /tab-content
          "</div>";             // /#app

  // Toast host. The JS path appends toasts here after each fetch(); the no-JS
  // fallback (plain form post -> redirect) renders a one-shot flash toast inside.
  html += "<div class='toast-container position-fixed top-0 end-0 p-3' id='toasts'>";
  if (flashMsg.length()) {
    html += "<div id='flash' class='toast align-items-center border-0 text-bg-";
    html += flashOk ? "success" : "danger";
    html += "' role='alert'><div class='d-flex'><div class='toast-body'>";
    html += htmlEscape(flashMsg);
    html += "</div><button type='button' class='btn-close btn-close-white me-2 m-auto' "
            "data-bs-dismiss='toast'></button></div></div>";
    flashMsg = "";  // one-shot
  }
  html += "</div>";

  // Busy overlay: the ESP32 handles each action slowly, so dim the page and show
  // a rotating spinner while a POST + in-place refresh is in flight.
  html += "<div id='busy' class='position-fixed top-0 start-0 w-100 h-100 d-none' "
          "style='background:rgba(0,0,0,.3);z-index:2000;'>"
          "<div class='position-absolute top-50 start-50 translate-middle text-center'>"
          "<div class='spinner-border text-light' style='width:3rem;height:3rem' role='status'></div>"
          "<div class='text-light mt-2 fw-semibold'>Processing&hellip;</div></div></div>";

  html += jsScript("/bootstrap.js", "bootstrap");
  html += jsScript("/sortable.js", "Sortable");
  html += jsScript("/chart.js", "Chart");

  // Submit every POST form via fetch() so saving never reloads the whole page:
  // show a toast from the JSON result, then swap just #app with fresh content.
  html += "<script>"
          "function busyOn(){document.getElementById('busy').classList.remove('d-none');}"
          "function busyOff(){document.getElementById('busy').classList.add('d-none');}"
          // Point the Claude card's Save button at whichever tab's form is active.
          // Delegated on document so it survives the #app swap after each save.
          "document.addEventListener('shown.bs.tab',function(ev){"
          "var fm=ev.target.getAttribute('data-form');if(!fm)return;"
          "var sb=document.getElementById('claudesave');if(sb)sb.setAttribute('form',fm);});"
          "function showToast(ok,msg){var c=document.getElementById('toasts');"
          "var d=document.createElement('div');"
          "d.className='toast align-items-center border-0 text-bg-'+(ok?'success':'danger');"
          "d.setAttribute('role','alert');"
          "d.innerHTML=\"<div class='d-flex'><div class='toast-body'></div>"
          "<button type='button' class='btn-close btn-close-white me-2 m-auto' data-bs-dismiss='toast'></button></div>\";"
          "d.querySelector('.toast-body').textContent=msg;"
          "c.appendChild(d);"
          "d.addEventListener('hidden.bs.toast',function(){d.remove();});"
          "new bootstrap.Toast(d,{delay:6000}).show();}"
          "async function reloadApp(){"
          // Remember which top-level tab is open so a save doesn't bounce the user
          // back to Dashboard when the fresh #app (always Dashboard-active) loads.
          "var am=document.querySelector('#maintabs .nav-link.active');"
          "var tgt=am?am.getAttribute('data-bs-target'):null;"
          "try{"
          "var r=await fetch('/',{headers:{'X-Requested-With':'fetch'}});"
          "var doc=new DOMParser().parseFromString(await r.text(),'text/html');"
          "var fresh=doc.getElementById('app');"
          "if(fresh)document.getElementById('app').replaceWith(fresh);}catch(e){}"
          "if(tgt&&window.bootstrap){var nb=document.querySelector('#maintabs .nav-link[data-bs-target=\"'+tgt+'\"]');"
          "if(nb)new bootstrap.Tab(nb).show();}"
          "initSortable();setupChart();}"
          // NOW trend chart. setupChart() seeds the date inputs (local time) +
          // wires controls; drawChart() queries /history for the averaged series.
          "function pad(n){return ('0'+n).slice(-2);}"
          "function fmtLocal(d){return d.getFullYear()+'-'+pad(d.getMonth()+1)+'-'+pad(d.getDate())"
          "+'T'+pad(d.getHours())+':'+pad(d.getMinutes());}"
          // Temperature y-axis: a fixed indoor band by default, stretched only on
          // the side where data actually goes past it (by 10% of that value).
          // Humidity is always pinned to 0-100. Tweak the band via these constants.
          "var TEMP_MIN_C=10,TEMP_MAX_C=35;"
          "function tempRange(a){var lo=TEMP_MIN_C,hi=TEMP_MAX_C,v=[];"
          "for(var i=0;i<a.length;i++){if(a[i]!=null&&!isNaN(a[i]))v.push(a[i]);}"
          "if(v.length){var mn=Math.min.apply(null,v),mx=Math.max.apply(null,v);"
          "if(mn<TEMP_MIN_C)lo=mn-0.1*Math.abs(mn);"
          "if(mx>TEMP_MAX_C)hi=mx+0.1*Math.abs(mx);}"
          "return {min:lo,max:hi};}"
          // Pick a 'nice' x-axis tick step (seconds) for the visible span, and
          // generate ticks aligned to local clock boundaries (00:00, 02:00, ...).
          "function stepFor(s){if(s<=4*3600)return 1800;if(s<=12*3600)return 3600;"
          "if(s<=28*3600)return 7200;if(s<=3*86400)return 21600;if(s<=8*86400)return 86400;"
          "if(s<=70*86400)return 604800;return 2592000;}"
          "function makeTicks(from,to){var st=stepFor(to-from);var d=new Date(from*1000);"
          "var mid=Math.floor(new Date(d.getFullYear(),d.getMonth(),d.getDate()).getTime()/1000);"
          "var v=mid+Math.ceil((from-mid)/st)*st,o=[];"
          "for(;v<=to;v+=st)o.push(v);return o;}"
          "function setupChart(){if(!document.getElementById('thchart_t'))return;"
          "var f=document.getElementById('hfrom'),t=document.getElementById('hto');"
          "if(f&&!f.value){var n=new Date();"
          "var s=new Date(n.getFullYear(),n.getMonth(),n.getDate());"  // today 00:00 local
          "f.value=fmtLocal(s);t.value=fmtLocal(new Date(s.getTime()+864e5));}"  // tomorrow 00:00

          "var ap=document.getElementById('happly');if(ap)ap.onclick=function(e){e.preventDefault();drawChart();};"
          "var bk=document.getElementById('hbucket');if(bk)bk.onchange=drawChart;"
          "drawChart();}"
          // Build one line chart (single series + own y-axis) sharing the time x-axis.
          "function buildChart(id,label,data,yr,color,unit,from,to,showDate){"
          "var cv=document.getElementById(id);if(!cv)return null;"
          "return new Chart(cv,{type:'line',data:{datasets:[{label:label,data:data,"
          "borderColor:color,backgroundColor:color,tension:.3,pointRadius:0}]},"
          "options:{responsive:true,maintainAspectRatio:false,animation:false,"
          "interaction:{intersect:false,mode:'index'},scales:{"
          "x:{type:'linear',min:from,max:to,"
          "afterBuildTicks:function(ax){ax.ticks=makeTicks(from,to).map(function(v){return {value:v};});},"
          "ticks:{autoSkip:false,callback:function(v){var d=new Date(v*1000);"
          "return showDate?(d.getMonth()+1)+'/'+d.getDate():pad(d.getHours())+':'+pad(d.getMinutes());}}},"
          "y:{title:{display:true,text:unit},min:yr.min,max:yr.max}}}});}"
          "async function drawChart(){if(typeof Chart==='undefined')return;"
          "if(!document.getElementById('thchart_t'))return;"
          "var f=Math.floor(new Date(document.getElementById('hfrom').value).getTime()/1000);"
          "var t=Math.floor(new Date(document.getElementById('hto').value).getTime()/1000);"
          "var b=document.getElementById('hbucket').value;if(!f||!t)return;"
          // Default to empty arrays so the axes still render when there's no data.
          "var h={t:[],temp:[],hum:[],bucket:60};"
          "try{var j=await (await fetch('/history?from='+f+'&to='+t+'&bucket='+b,"
          "{headers:{'X-Requested-With':'fetch'}})).json();if(j&&j.t)h=j;}catch(e){}"
          "var showDate=stepFor(t-f)>=86400;"
          "var tp=h.t.map(function(s,i){return {x:s,y:h.temp[i]};});"
          "var hm=h.t.map(function(s,i){return {x:s,y:h.hum[i]};});"
          "if(window._thc)window._thc.destroy();if(window._thh)window._thh.destroy();"
          "window._thc=buildChart('thchart_t','Temp \\u00b0C',tp,tempRange(h.temp),'#dc3545','\\u00b0C',f,t,showDate);"
          "window._thh=buildChart('thchart_h','Humidity %',hm,{min:0,max:100},'#0d6efd','%',f,t,showDate);}"
          // Make a reorderable list drag-sortable; dropping only rearranges the DOM
          // and lights the Save button. Used for To-do, Wi-Fi, and weather lists.
          "function makeSortable(listId,btnId){var el=document.getElementById(listId);"
          "if(!el||typeof Sortable==='undefined')return;"
          "Sortable.create(el,{handle:'.drag-handle',animation:150,onEnd:function(evt){"
          "if(evt.oldIndex===evt.newIndex)return;"  // dropped back in place -> nothing changed
          "var b=document.getElementById(btnId);"
          "if(b){b.classList.remove('btn-outline-secondary');b.classList.add('btn-primary');b.disabled=false;}}});}"
          "function initSortable(){makeSortable('todolist','todosavebtn');"
          "makeSortable('wifilist','wifisavebtn');makeSortable('citylist','citysavebtn');}"
          // Fill a Save-order form's hidden field with its list's current DOM order.
          "function fillOrder(listId,inputId){var wl=document.getElementById(listId);"
          "var oi=document.getElementById(inputId);"
          "if(wl&&oi)oi.value=[].map.call(wl.children,function(li){return li.getAttribute('data-idx');})"
          ".filter(function(x){return x!==null;}).join(',');}"
          "document.addEventListener('submit',async function(ev){"
          "var f=ev.target;if((f.method||'').toLowerCase()!=='post')return;"
          "ev.preventDefault();"
          // Save order: capture the current drag order (data-idx in DOM order) so
          // the device applies + persists exactly what's shown.
          "if(f.id==='wifisaveform')fillOrder('wifilist','wifiorderinput');"
          "if(f.id==='citysaveform')fillOrder('citylist','cityorderinput');"
          "if(f.id==='todoform')fillOrder('todolist','todoorderinput');"
          "var body=new URLSearchParams(new FormData(f));"
          "var s=ev.submitter;if(s&&s.name)body.append(s.name,s.value);"
          "busyOn();"
          "try{var r=await fetch(f.getAttribute('action'),"
          "{method:'POST',headers:{'X-Requested-With':'fetch'},body:body});"
          "var data={ok:true,msg:''};try{data=await r.json();}catch(e){}"
          "if(data.msg)showToast(!!data.ok,data.msg);await reloadApp();}"
          "catch(e){showToast(false,'Request failed (device may have switched Wi-Fi)');}"
          "finally{busyOff();}});"
          // no-JS fallback flash toast, if present
          "var fe=document.getElementById('flash');"
          "if(fe){new bootstrap.Toast(fe,{delay:6000}).show();}"
          "initSortable();setupChart();"
          // Keep the chart live; the interval persists across #app swaps.
          "if(!window._thi){window._thi=setInterval(drawChart,60000);}"
          "</script></body></html>";
  server.send(200, "text/html", html);
}

static void handleWifi() {
  String s = server.hasArg("ssid") ? server.arg("ssid") : "";
  String p = server.hasArg("pass") ? server.arg("pass") : "";
  // Plain-page setup flow = phone on the SoftAP submitting the minimal page (no
  // AJAX). Capture it BEFORE the join, since a success schedules the AP teardown.
  bool setupFlow = wifiInSetupMode() && !server.hasHeader("X-Requested-With");
  // "store" = save without testing; anything else (default) tests the join first.
  // The setup flow must actually connect, so force a test there.
  bool test = setupFlow || (server.arg("mode") != "store");
  // Note: testing a new network drops the current link, so this HTTP response
  // may not reach the browser; reconnect via http://esp32.local/ afterwards.
  bool ok = test ? wifiAddNetwork(s, p) : wifiStoreNetwork(s, p);
  logInfo("WiFi %s via web: %s -> %s", test ? "test+add" : "add", s.c_str(), ok ? "saved" : "rejected");
  if (setupFlow) {
    // Minimal result page. On success the AP is mid-teardown (scheduled with a
    // grace period), so this reply is the last thing the phone gets over it.
    String h =
      "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Wi-Fi setup</title><style>"
      "body{font-family:system-ui,Arial,sans-serif;max-width:24rem;margin:2rem auto;padding:0 1rem;color:#222}"
      "a{color:#0d6efd}</style></head><body>";
    if (ok) {
      h += "<h1>Connected</h1><p>The device joined <b>";
      h += htmlEscape(s);
      h += "</b> and saved it. It is now on your network &mdash; you can disconnect from the <b>";
      h += htmlEscape(wifiSetupApSsid());
      h += "</b> hotspot. The panel is at <b>http://";
      h += wifiHostname();
      h += ".local/</b>.</p>";
    } else {
      h += "<h1>Couldn&#39;t connect</h1><p>Could not join <b>";
      h += htmlEscape(s);
      h += "</b>. Check the password and <a href='/'>try again</a>.</p>";
    }
    h += "</body></html>";
    server.send(200, "text/html", h);
    return;
  }
  if (test)
    respond(ok, ok ? ("Connected & saved: " + s)
                   : ("Failed: couldn't connect to '" + s + "', or couldn't save (SD card?)"));
  else
    respond(ok, ok ? ("Saved (not tested): " + s)
                   : (s.length() ? "Save failed (SD card?)" : "SSID required"));
}

static void handleWifiEdit() {
  int idx = server.hasArg("idx") ? server.arg("idx").toInt() : -1;
  String act = server.hasArg("act") ? server.arg("act") : "";
  bool ok = true;
  String msg = "";
  if (act == "del") {
    String s = wifiNetSSID(idx);  // capture the name before removal
    ok = wifiRemoveNetwork(s);
    msg = ok ? ("Removed: " + s) : "Remove failed (SD card?)";
  } else if (act == "top") {
    // Bubble the item up to index 0, preserving the order of the rest.
    for (int k = idx; k > 0; k--) wifiMoveNetwork(k, -1);
    if (idx > 0) msg = "Moved to top - click 'Save order' to keep it";
  }
  logInfo("WiFi edit via web: idx=%d act=%s", idx, act.c_str());
  respond(ok, msg);
}

// "Save order": apply the drag order if one was sent ("order" = CSV of old
// indices in their new positions; empty = keep the current RAM order, e.g. the
// no-JS / move-to-top path), then persist to SD. Dragging itself never hits the
// device; this is the only place the order is saved.
static void handleWifiSave() {
  String csv = server.hasArg("order") ? server.arg("order") : "";
  csv.trim();
  bool ok = true;
  if (csv.length()) {
    int order[16];  // matches MAX_NETS in wifi_net
    int n = 0;
    int start = 0;
    while (start <= (int)csv.length() && n < 16) {
      int comma = csv.indexOf(',', start);
      String tok = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
      tok.trim();
      if (tok.length()) order[n++] = tok.toInt();
      if (comma < 0) break;
      start = comma + 1;
    }
    ok = wifiApplyOrder(order, n);
  }
  if (!ok) {
    respond(false, "Reorder failed");
    return;
  }
  bool saved = wifiSaveNetworks();
  logInfo("WiFi order save via web: %s", saved ? "ok" : "failed");
  respond(saved, saved ? "Priority order saved" : "Save failed (SD card?)");
}

static void handleClaude() {
  // A pasted full cookie takes priority over the individual org/key fields.
  String cookie = server.hasArg("cookie") ? server.arg("cookie") : String("");
  cookie.trim();
  if (cookie.length() > 0) {
    if (!claudeUsageSetFromCookie(cookie)) {
      respond(false, "No sessionKey found in the pasted cookie");
      return;
    }
    bool saved = claudeUsageSave();  // persist the new org id + key to config
    logInfo("Claude credentials updated from pasted cookie via web UI");
    claudeUsageUpdate();
    respond(saved, saved ? "Claude credentials saved from cookie" : "Save failed (SD card?)");
    return;
  }
  if (server.hasArg("org")) claudeUsageSetOrgId(server.arg("org"));
  if (server.hasArg("key")) claudeUsageSetSessionKey(server.arg("key"));  // empty -> keep current
  bool saved = claudeUsageSave();  // persist the new org id + key to config
  logInfo("Claude credentials updated via web UI");
  claudeUsageUpdate();  // refresh now so the result shows on the LCD immediately
  respond(saved, saved ? "Claude credentials saved" : "Save failed (SD card?)");
}

static void handleGdoc() {
  if (!server.hasArg("url")) {
    respond(false, "No URL provided");
    return;
  }
  gdocSetUrl(server.arg("url"));  // normalizes a Docs link to the txt export
  bool saved = gdocSaveUrl();     // persist to config (esp32.json)
  logInfo("gdoc URL updated via web UI");
  gdocUpdate();  // refresh the Notes box now
  respond(saved, saved ? "Google Doc URL saved" : "Save failed (SD card?)");
}

static void handleTz() {
  int primary = server.hasArg("primary") ? server.arg("primary").toInt() : timePrimaryZone();
  int secondary = server.hasArg("secondary") ? server.arg("secondary").toInt() : timeSecondaryZone();
  timeSetZones(primary, secondary);  // out-of-range values are ignored
  bool saved = timeSaveZones();      // persist to config (esp32.json)
  logInfo("Time zones updated via web UI: %s / %s",
          timeZoneLabel(timePrimaryZone()), timeZoneLabel(timeSecondaryZone()));
  respond(saved, saved ? "Time zones saved" : "Save failed (SD card?)");
}

// Serve a cached asset by streaming it from the SD card in small chunks (so a
// ~250 KB file never has to sit in RAM). If it isn't cached yet, redirect to the
// CDN so an online browser still gets styled.
static void handleAsset() {
  const CachedAsset *a = assetByRoute(server.uri().c_str());
  if (!a) {
    server.send(404, "text/plain", "Not found");
    return;
  }
  String path = sdPath(a->file);
  FILE *f = path.length() ? fopen(path.c_str(), "rb") : nullptr;
  if (!f) {
    server.sendHeader("Location", a->url);  // not cached -> let the browser hit the CDN
    server.send(302);
    return;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  server.sendHeader("Cache-Control", "max-age=604800");  // let the browser cache it for a week too
  server.setContentLength(size);
  server.send(200, a->contentType, "");
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) server.sendContent(buf, n);
  fclose(f);
}

static void handleFavicon() {
  server.sendHeader("Cache-Control", "max-age=604800");  // browser may cache it a week
  server.send_P(200, "image/x-icon", (const char *)favicon_ico, favicon_ico_len);
}

static void handleWeatherAdd() {
  String q = server.hasArg("city") ? server.arg("city") : "";
  String resolved;
  bool ok = weatherAddCity(q, resolved);  // geocodes, appends, persists on success
  logInfo("Weather city add via web: %s -> %s", q.c_str(), ok ? "added" : "rejected");
  respond(ok, ok ? ("Added: " + resolved) : ("Could not add '" + q + "': " + resolved));
}

// Per-row weather actions (mirrors handleWifiEdit): remove, or move-to-top (RAM
// only -> user clicks Save order). A change to the top set re-fetches the shown
// cities so the LCD reflects it.
static void handleWeatherEdit() {
  int idx = server.hasArg("idx") ? server.arg("idx").toInt() : -1;
  String act = server.hasArg("act") ? server.arg("act") : "";
  bool ok = true;
  String msg = "";
  if (act == "del") {
    String name = weatherCityName(idx);  // capture before removal
    ok = weatherRemoveCity(idx);
    if (ok) weatherUpdateAll();  // top set may have shifted
    msg = ok ? ("Removed: " + name) : "Remove failed (SD card?)";
  } else if (act == "top") {
    for (int k = idx; k > 0; k--) weatherMoveCity(k, -1);  // bubble up to index 0
    if (idx > 0) msg = "Moved to top - click 'Save order' to keep it";
  }
  logInfo("Weather city edit via web: idx=%d act=%s", idx, act.c_str());
  respond(ok, msg);
}

// "Save order" for weather cities (mirrors handleWifiSave): apply the drag order
// if sent, persist, then re-fetch the (possibly new) top cities.
static void handleWeatherOrder() {
  String csv = server.hasArg("order") ? server.arg("order") : "";
  csv.trim();
  bool ok = true;
  if (csv.length()) {
    int order[16];  // matches MAX_CITIES in weather
    int n = 0;
    int start = 0;
    while (start <= (int)csv.length() && n < 16) {
      int comma = csv.indexOf(',', start);
      String tok = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
      tok.trim();
      if (tok.length()) order[n++] = tok.toInt();
      if (comma < 0) break;
      start = comma + 1;
    }
    ok = weatherApplyOrder(order, n);
  }
  if (!ok) {
    respond(false, "Reorder failed");
    return;
  }
  bool saved = weatherSaveCities();
  if (saved) weatherUpdateAll();  // new top set -> refresh shown cities
  logInfo("Weather order save via web: %s", saved ? "ok" : "failed");
  respond(saved, saved ? "Priority order saved" : "Save failed (SD card?)");
}

// Store a relayed/pasted Codex access token. `machine` picks the reply style: the
// cron relay is a plain curl, so it gets plain text instead of the form's toast
// JSON / redirect. An unchanged token is accepted without touching the SD card,
// so an hourly relay doesn't rewrite esp32.json 24 times a day.
static void applyCodexToken(bool machine) {
  String tok = server.hasArg("token") ? server.arg("token") : String("");
  tok.trim();
  bool ok = true;
  String msg;
  if (tok.length() == 0) {
    ok = false;
    msg = "No token supplied";
  } else if (codexTokenMatches(tok)) {
    msg = "Token unchanged";
  } else {
    codexUsageSetToken(tok);
    ok = codexUsageSave();
    msg = ok ? "Codex token saved" : "Save failed (SD card?)";
    logInfo("Codex token updated via %s", machine ? "relay" : "web UI");
    if (ok) codexUsageUpdate();  // refresh now so the result shows on the LCD immediately
  }
  if (machine) server.send(ok ? 200 : 400, "text/plain", msg + "\n");
  else respond(ok, msg);
}

static void handleCodex() {
  applyCodexToken(false);
}
static void handleCodexToken() {
  applyCodexToken(true);
}

static void handleIntervals() {
  bool ok = true;
  if (server.hasArg("claude")) ok = claudeUsageSetIntervalMin(server.arg("claude").toInt()) && ok;
  if (server.hasArg("codex")) ok = codexUsageSetIntervalMin(server.arg("codex").toInt()) && ok;
  if (server.hasArg("gdoc")) ok = gdocSetIntervalMin(server.arg("gdoc").toInt()) && ok;
  logInfo("Refresh intervals updated via web UI: claude=%d min, codex=%d min, gdoc=%d min",
          claudeUsageIntervalMin(), codexUsageIntervalMin(), gdocIntervalMin());
  respond(ok, ok ? "Refresh intervals saved" : "Save failed (SD card?)");
}

// Persist the to-do list to the SD card as a markdown checklist:
//   - [x] done item
//   - [ ] open item
static bool todoSave() {
  String md;
  for (int i = 0; i < todoCount; i++) {
    md += todos[i].done ? "- [x] " : "- [ ] ";
    md += todos[i].text;
    md += '\n';
  }
  return sdWriteText("todo.md", md);
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

// Apply the browser's drag order after rebuildFromArgs() has captured the
// current text and checkbox values. The order must be a full permutation of
// the submitted rows; malformed/stale requests are rejected without saving.
static bool todoApplyOrder(const String &csv) {
  Todo reordered[MAX_TODOS];
  bool seen[MAX_TODOS] = {};
  int start = 0;
  for (int n = 0; n < todoCount; n++) {
    if (start > (int)csv.length()) return false;
    int comma = csv.indexOf(',', start);
    if ((n < todoCount - 1 && comma < 0) || (n == todoCount - 1 && comma >= 0)) return false;
    String tok = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
    tok.trim();
    if (!tok.length()) return false;
    for (size_t j = 0; j < tok.length(); j++)
      if (tok[j] < '0' || tok[j] > '9') return false;
    int idx = tok.toInt();
    if (idx < 0 || idx >= todoCount || seen[idx]) return false;
    seen[idx] = true;
    reordered[n] = todos[idx];
    start = comma + 1;
  }
  for (int i = 0; i < todoCount; i++) todos[i] = reordered[i];
  return true;
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
    respond(true, "");  // empty msg -> no toast, just refresh the list with the new row
  } else if (action.startsWith("del")) {
    // Remove the item at the given index (rebuildFromArgs kept the other edits).
    int idx = action.substring(3).toInt();
    if (idx >= 0 && idx < todoCount) {
      for (int j = idx; j < todoCount - 1; j++) todos[j] = todos[j + 1];
      todoCount--;
    }
    bool saved = todoSave();  // removal is durable
    logInfo("To-do item %d removed (%d left) -> %s", idx, todoCount, saved ? "saved" : "save failed");
    respond(saved, saved ? "To-do item removed" : "Save failed (SD card?)");
  } else {
    // Save the current fields in their dragged order; empty items are kept.
    String order = server.hasArg("order") ? server.arg("order") : "";
    order.trim();
    if (order.length() && !todoApplyOrder(order)) {
      respond(false, "Reorder failed");
      return;
    }
    bool saved = todoSave();  // persist to /sdcard/todo.md
    logInfo("To-do saved (%d items) -> %s", todoCount, saved ? "ok" : "failed");
    respond(saved, saved ? "To-do saved" : "Save failed (SD card?)");
  }
}

// GET /stats: machine-readable snapshot (date+time, temperature, humidity) for
// curl/scripts. Values are null when unavailable (clock not synced / no sensor).
static void handleStats() {
  float tC = NAN, rh = NAN;
  bool haveSensor = sensorsPresent() && sensorsRead(&tC, &rh);
  char tbuf[64];
  bool haveTime = timeFormatDateTime(tbuf, sizeof(tbuf));
  time_t now = time(nullptr);

  String j = "{\"datetime\":";
  if (haveTime) {
    j += "\"";
    j += jsonEscape(tbuf);
    j += "\"";
  } else {
    j += "null";
  }
  j += ",\"epoch\":";
  j += String((long)now);
  j += ",\"temperature_c\":";
  j += haveSensor ? String(tC, 1) : String("null");
  j += ",\"humidity_pct\":";
  j += haveSensor ? String(rh, 1) : String("null");
  j += "}\n";
  server.send(200, "application/json", j);
}

static long bucketSeconds(const String &b) {
  if (b == "minutely") return 60;
  if (b == "hourly") return 3600;
  if (b == "daily") return 86400;
  if (b == "weekly") return 7L * 86400;
  if (b == "monthly") return 30L * 86400;
  return 3600;
}

// GET /history?from=<epoch>&to=<epoch>&bucket=<name>: averaged temp/humidity over
// the range, as JSON for the NOW chart (defaults to the last 24 h, hourly).
static void handleHistory() {
  time_t to = server.hasArg("to") ? (time_t)server.arg("to").toInt() : 0;
  time_t from = server.hasArg("from") ? (time_t)server.arg("from").toInt() : 0;
  long bs = bucketSeconds(server.hasArg("bucket") ? server.arg("bucket") : "hourly");
  if (to <= 0) to = time(nullptr);
  if (from <= 0) from = to - 86400;
  server.send(200, "application/json", historyQuery(from, to, bs));
}

void webBegin() {
  todoLoad();  // restore the to-do list from SD (requires sdBegin() earlier in setup)
  // Capture this request header so respond() can tell fetch() calls from plain posts.
  static const char *HEADER_KEYS[] = {"X-Requested-With"};
  server.collectHeaders(HEADER_KEYS, sizeof(HEADER_KEYS) / sizeof(HEADER_KEYS[0]));
  server.on("/", HTTP_GET, handleRoot);
  server.on("/favicon.ico", HTTP_GET, handleFavicon);  // embedded star icon
  server.on("/stats", HTTP_GET, handleStats);    // curl-friendly JSON snapshot
  server.on("/history", HTTP_GET, handleHistory);  // recent samples for the NOW chart
  server.on("/save", HTTP_POST, handleSave);
  server.on("/claude", HTTP_POST, handleClaude);
  server.on("/codex", HTTP_POST, handleCodex);            // token pasted into the web form
  server.on("/codextoken", HTTP_POST, handleCodexToken);  // token relayed by the cron one-liner
  server.on("/gdoc", HTTP_POST, handleGdoc);
  server.on("/tz", HTTP_POST, handleTz);
  server.on("/intervals", HTTP_POST, handleIntervals);
  server.on("/weatheradd", HTTP_POST, handleWeatherAdd);
  server.on("/weatheredit", HTTP_POST, handleWeatherEdit);
  server.on("/weatherorder", HTTP_POST, handleWeatherOrder);
  // Serve the cached third-party assets (Bootstrap) from SD for offline use.
  for (int i = 0; i < assetCount(); i++)
    server.on(assetAt(i)->route, HTTP_GET, handleAsset);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/wifiedit", HTTP_POST, handleWifiEdit);
  server.on("/wifisave", HTTP_POST, handleWifiSave);
  server.onNotFound([]() {
    // Captive portal: while the setup AP is up, the OS connectivity probes
    // (Android /generate_204, iOS /hotspot-detect.html, Windows /connecttest.txt,
    // ...) land here via the catch-all DNS. Redirect them to the setup page so the
    // phone auto-opens the "Sign in to network" sheet.
    if (wifiInSetupMode()) {
      server.sendHeader("Location", "http://" + wifiSetupApIp() + "/");
      server.send(302, "text/plain", "");
      return;
    }
    server.send(404, "text/plain", "Not found");
  });
  server.begin();
  logInfo("Web UI listening on port 80");
}

void webHandle() {
  server.handleClient();
}

void webReloadTodo() {
  todoLoad();  // re-read /sdcard/todo.md (e.g. after a card was re-inserted)
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
