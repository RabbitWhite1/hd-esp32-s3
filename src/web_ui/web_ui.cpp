#include "web_ui.h"
#include "../claude_usage/claude_usage.h"  // configure org id + session key from the web form
#include "../sensors/sensors.h"            // live temp/humidity shown on the page
#include "../wifi_net/wifi_net.h"          // add/list saved Wi-Fi networks from the form
#include "../weather/weather.h"            // add/remove weather cities (geocoded) from the form
#include "../sdcard/sdcard.h"              // persist the to-do list to /sdcard/todo.md
#include "../asset_cache/asset_cache.h"    // serve Bootstrap from SD (offline-capable)
#include "../history/history.h"            // temp/humidity ring buffer for the NOW chart
#include "../gdoc/gdoc.h"                  // configure the Google Doc URL from the form
#include "../time_sync/time_sync.h"        // select primary/secondary time zones from the form
#include "../logging/logging.h"
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
    "<title>h4d panel</title>";
  html += cssLink("/bootstrap.css");
  html +=
    "<style>html{scroll-behavior:smooth}.card{scroll-margin-top:4rem}"
    "@media(max-width:767px){#sidenav{position:static!important}}</style>"
    "</head><body class='bg-body-tertiary'>"
    "<div id='app' class='container pb-4' style='max-width:980px'>"
    // Sticky title bar: stays pinned at the top while the page scrolls.
    "<div class='sticky-top bg-body-tertiary py-3 mb-3 border-bottom'>"
    "<h1 class='h3 m-0'>h4d panel</h1></div>"
    "<div class='row g-4'>"
    // Left-side navigator: sticky on desktop, stacks on top on narrow screens.
    "<div class='col-12 col-md-3'>"
    "<nav id='sidenav' class='nav flex-column position-sticky' style='top:4.5rem'>"
    "<a class='nav-link' href='#now'>Now</a>"
    "<a class='nav-link' href='#todo'>To-do</a>"
    "<a class='nav-link' href='#gdoc'>Google Doc</a>"
    "<a class='nav-link' href='#tz'>Time zones</a>"
    "<a class='nav-link' href='#weather'>Weather cities</a>"
    "<a class='nav-link' href='#intervals'>Refresh intervals</a>"
    "<a class='nav-link' href='#claude'>Claude usage</a>"
    "<a class='nav-link' href='#wifi'>Wi-Fi</a>"
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
                   "<button class='btn btn-sm btn-primary' form='todoform' "
                   "name='action' value='save'>Save</button>");
  html += "<form id='todoform' action='/save' method='POST'>";
  for (int i = 0; i < todoCount; i++) {
    html += "<div class='input-group mb-2'><div class='input-group-text'>"
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
  // "+" adds a blank row (a server round-trip that also preserves current edits).
  if (todoCount < MAX_TODOS)
    html += "<button class='btn btn-outline-secondary' type='submit' name='action' value='add'>+ Add row</button>";
  html += "</form>";
  html += cardClose;

  // Google Doc URL shown in the Notes box (paste a normal Docs/sharing link; it's
  // reduced to the base doc URL and the txt export is fetched in code). Persisted
  // to /sdcard/gdoc_url.txt.
  html += cardOpen("gdoc", "Google Doc", saveBtn("gdocform"));
  html += "<form id='gdocform' action='/gdoc' method='POST'>"
          "<label class='form-label'>Doc URL</label>"
          "<input type='text' class='form-control' name='url' value='";
  html += htmlEscape(gdocUrl());
  html += "'></form>";
  html += cardClose;

  // Time zones: two dropdowns (primary shown first, secondary in parentheses on
  // the LCD). Each option is an abbreviation + a famous city in that zone. The
  // selection is persisted to /sdcard/tz.txt.
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

  // Weather cities: add by name (geocoded to coordinates), remove, persisted to
  // /sdcard/cities.txt. The forecast API uses the resolved lat/lon. Shown 2 per row.
  html += cardOpen("weather", "Weather cities");
  html += "<div class='row g-2 mb-3'>";
  if (weatherCityCount() == 0) html += "<div class='col-12 text-muted'>(none)</div>";
  for (int i = 0; i < weatherCityCount(); i++) {
    html += "<div class='col-6'><div class='border rounded p-2 d-flex justify-content-between align-items-center'>";
    html += htmlEscape(weatherCityName(i));
    html += "<form action='/weatherdel' method='POST' class='m-0'>"
            "<input type='hidden' name='idx' value='";
    html += i;
    html += "'><button class='btn btn-sm btn-outline-danger'>Remove</button></form></div></div>";
  }
  html += "</div>";
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

  // Auto-refresh intervals (minutes), persisted to esp32.conf. Each input shows a
  // non-editable light-grey "min" suffix.
  html += cardOpen("intervals", "Refresh intervals", saveBtn("intervalsform"));
  html += "<form id='intervalsform' action='/intervals' method='POST' class='row g-3'>"
          "<div class='col-sm-6'><label class='form-label'>Claude usage</label>"
          "<div class='input-group'><input type='number' class='form-control' name='claude' min='1' value='";
  html += claudeUsageIntervalMin();
  html += "'><span class='input-group-text text-muted'>min</span></div></div>"
          "<div class='col-sm-6'><label class='form-label'>Google Doc</label>"
          "<div class='input-group'><input type='number' class='form-control' name='gdoc' min='1' value='";
  html += gdocIntervalMin();
  html += "'><span class='input-group-text text-muted'>min</span></div></div></form>";
  html += cardClose;

  // Claude usage credentials (kept in RAM on the device, never in the firmware).
  // The stored session key is NEVER written into the page, so anyone on the LAN
  // can't read it from the source. Leave the field blank to keep the current key.
  html += cardOpen("claude", "Claude usage", saveBtn("claudeform"));
  html += "<form id='claudeform' action='/claude' method='POST'>"
          "<label class='form-label'>Org ID</label>"
          "<input type='text' class='form-control mb-3' name='org' value='";
  html += htmlEscape(claudeUsageOrgId());
  html += "'><label class='form-label'>Session key ";
  html += claudeUsageHasKey() ? "<span class='badge text-bg-success'>set</span>"
                              : "<span class='badge text-bg-secondary'>not set</span>";
  html += "</label><input type='text' class='form-control' name='key' "
          "placeholder='leave blank to keep current'></form>";
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
    html += "'><span><span class='wifi-handle me-2' style='cursor:grab' title='Drag to reorder'>"
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
  // Up/down only reorder in RAM; this button persists the order to the SD card.
  // It lights up (primary) once the order has unsaved changes.
  html += "<form action='/wifisave' method='POST' class='mb-3'>"
          "<button class='btn btn-sm ";
  html += wifiOrderDirty() ? "btn-primary" : "btn-outline-secondary";
  html += "'>Save order</button></form>";
  html += "<form action='/wifi' method='POST'>"
          "<label class='form-label'>SSID</label>"
          "<input type='text' class='form-control mb-2' name='ssid'>"
          "<label class='form-label'>Password</label>"
          "<input type='text' class='form-control mb-3' name='pass'>"
          "<button class='btn btn-primary'>Add (tests before saving)</button></form>";
  html += cardClose;

  html += "</div></div></div>";  // /content col, /row, /#app

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

  html += jsScript("/bootstrap.js", "bootstrap");
  html += jsScript("/sortable.js", "Sortable");
  html += jsScript("/chart.js", "Chart");

  // Submit every POST form via fetch() so saving never reloads the whole page:
  // show a toast from the JSON result, then swap just #app with fresh content.
  html += "<script>"
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
          "async function reloadApp(){try{"
          "var r=await fetch('/',{headers:{'X-Requested-With':'fetch'}});"
          "var doc=new DOMParser().parseFromString(await r.text(),'text/html');"
          "var fresh=doc.getElementById('app');"
          "if(fresh)document.getElementById('app').replaceWith(fresh);}catch(e){}"
          "initSortable();setupChart();}"
          // NOW trend chart. setupChart() seeds the date inputs (local time) +
          // wires controls; drawChart() queries /history for the averaged series.
          "function pad(n){return ('0'+n).slice(-2);}"
          "function fmtLocal(d){return d.getFullYear()+'-'+pad(d.getMonth()+1)+'-'+pad(d.getDate())"
          "+'T'+pad(d.getHours())+':'+pad(d.getMinutes());}"
          // y-axis bounds: 10% below min / 10% above max of the data (10% of the
          // absolute value, so it also widens for negative temps). {} when empty.
          "function yrange(a){var v=[];for(var i=0;i<a.length;i++){if(a[i]!=null&&!isNaN(a[i]))v.push(a[i]);}"
          "if(!v.length)return {};var mn=Math.min.apply(null,v),mx=Math.max.apply(null,v);"
          "var lo=mn-0.1*Math.abs(mn),hi=mx+0.1*Math.abs(mx);"
          "if(lo>=hi){lo=mn-1;hi=mx+1;}return {min:lo,max:hi};}"
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
          "function buildChart(id,label,data,raw,color,unit,from,to,showDate){"
          "var cv=document.getElementById(id);if(!cv)return null;var yr=yrange(raw);"
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
          "window._thc=buildChart('thchart_t','Temp \\u00b0C',tp,h.temp,'#dc3545','\\u00b0C',f,t,showDate);"
          "window._thh=buildChart('thchart_h','Humidity %',hm,h.hum,'#0d6efd','%',f,t,showDate);}"
          // Make the Wi-Fi list drag-sortable; on drop, POST the new order (the
          // data-idx values in their new DOM order) and refresh in place.
          "function initSortable(){var el=document.getElementById('wifilist');"
          "if(!el||typeof Sortable==='undefined')return;"
          "Sortable.create(el,{handle:'.wifi-handle',animation:150,onEnd:async function(){"
          "var ids=[].map.call(el.children,function(li){return li.getAttribute('data-idx');})"
          ".filter(function(x){return x!==null;}).join(',');"
          "try{var r=await fetch('/wifiorder',{method:'POST',"
          "headers:{'X-Requested-With':'fetch','Content-Type':'application/x-www-form-urlencoded'},"
          "body:'order='+encodeURIComponent(ids)});"
          "var d={ok:true,msg:''};try{d=await r.json();}catch(e){}"
          "if(d.msg)showToast(!!d.ok,d.msg);await reloadApp();}"
          "catch(e){showToast(false,'Reorder failed');}}});}"
          "document.addEventListener('submit',async function(ev){"
          "var f=ev.target;if((f.method||'').toLowerCase()!=='post')return;"
          "ev.preventDefault();"
          "var body=new URLSearchParams(new FormData(f));"
          "var s=ev.submitter;if(s&&s.name)body.append(s.name,s.value);"
          "try{var r=await fetch(f.getAttribute('action'),"
          "{method:'POST',headers:{'X-Requested-With':'fetch'},body:body});"
          "var data={ok:true,msg:''};try{data=await r.json();}catch(e){}"
          "if(data.msg)showToast(!!data.ok,data.msg);await reloadApp();}"
          "catch(e){showToast(false,'Request failed (device may have switched Wi-Fi)');}});"
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
  // Note: testing a new network drops the current link, so this HTTP response
  // may not reach the browser; reconnect via http://esp32.local/ afterwards.
  bool ok = wifiAddNetwork(s, p);
  logInfo("WiFi add via web: %s -> %s", s.c_str(), ok ? "saved" : "rejected");
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
  respond(ok, ok ? ("Connected & saved: " + s) : ("Could not connect to '" + s + "' - not saved"));
}

static void handleWifiEdit() {
  int idx = server.hasArg("idx") ? server.arg("idx").toInt() : -1;
  String act = server.hasArg("act") ? server.arg("act") : "";
  bool ok = true;
  String msg = "";
  if (act == "del") {
    String s = wifiNetSSID(idx);  // capture the name before removal
    ok = wifiRemoveNetwork(s);
    msg = ok ? ("Removed: " + s) : "Remove failed";
  } else if (act == "top") {
    // Bubble the item up to index 0, preserving the order of the rest.
    for (int k = idx; k > 0; k--) wifiMoveNetwork(k, -1);
    if (idx > 0) msg = "Moved to top - click 'Save order' to keep it";
  }
  logInfo("WiFi edit via web: idx=%d act=%s", idx, act.c_str());
  respond(ok, msg);
}

static void handleWifiSave() {
  wifiSaveNetworks();
  logInfo("WiFi priority order saved via web");
  respond(true, "Priority order saved");
}

// Apply a drag-and-drop reorder: "order" is a CSV of the old indices in their
// new positions. Reorders in RAM only; the user still clicks "Save order".
static void handleWifiOrder() {
  String csv = server.hasArg("order") ? server.arg("order") : "";
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
  bool ok = wifiApplyOrder(order, n);
  logInfo("WiFi reorder via web (%d items): %s", n, ok ? "applied" : "rejected");
  respond(ok, ok ? "Order changed - click 'Save order' to keep it" : "Reorder failed");
}

static void handleClaude() {
  if (server.hasArg("org")) claudeUsageSetOrgId(server.arg("org"));
  if (server.hasArg("key")) claudeUsageSetSessionKey(server.arg("key"));  // empty -> keep current
  logInfo("Claude credentials updated via web UI");
  claudeUsageUpdate();  // refresh now so the result shows on the LCD immediately
  respond(true, "Claude credentials saved");
}

static void handleGdoc() {
  if (!server.hasArg("url")) {
    respond(false, "No URL provided");
    return;
  }
  gdocSetUrl(server.arg("url"));  // normalizes a Docs link to the txt export
  gdocSaveUrl();                  // persist to /sdcard/gdoc_url.txt
  logInfo("gdoc URL updated via web UI");
  gdocUpdate();  // refresh the Notes box now
  respond(true, "Google Doc URL saved");
}

static void handleTz() {
  int primary = server.hasArg("primary") ? server.arg("primary").toInt() : timePrimaryZone();
  int secondary = server.hasArg("secondary") ? server.arg("secondary").toInt() : timeSecondaryZone();
  timeSetZones(primary, secondary);  // out-of-range values are ignored
  timeSaveZones();                   // persist to /sdcard/tz.txt
  logInfo("Time zones updated via web UI: %s / %s",
          timeZoneLabel(timePrimaryZone()), timeZoneLabel(timeSecondaryZone()));
  respond(true, "Time zones saved");
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

static void handleWeatherAdd() {
  String q = server.hasArg("city") ? server.arg("city") : "";
  String resolved;
  bool ok = weatherAddCity(q, resolved);  // geocodes, appends, persists on success
  logInfo("Weather city add via web: %s -> %s", q.c_str(), ok ? "added" : "rejected");
  respond(ok, ok ? ("Added: " + resolved) : ("Could not add '" + q + "': " + resolved));
}

static void handleWeatherDel() {
  int idx = server.hasArg("idx") ? server.arg("idx").toInt() : -1;
  String name = weatherCityName(idx);  // capture before removal
  bool ok = weatherRemoveCity(idx);
  respond(ok, ok ? ("Removed: " + name) : "Remove failed");
}

static void handleIntervals() {
  if (server.hasArg("claude")) claudeUsageSetIntervalMin(server.arg("claude").toInt());
  if (server.hasArg("gdoc")) gdocSetIntervalMin(server.arg("gdoc").toInt());
  logInfo("Refresh intervals updated via web UI: claude=%d min, gdoc=%d min",
          claudeUsageIntervalMin(), gdocIntervalMin());
  respond(true, "Refresh intervals saved");
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
    respond(true, "");  // empty msg -> no toast, just refresh the list with the new row
  } else if (action.startsWith("del")) {
    // Remove the item at the given index (rebuildFromArgs kept the other edits).
    int idx = action.substring(3).toInt();
    if (idx >= 0 && idx < todoCount) {
      for (int j = idx; j < todoCount - 1; j++) todos[j] = todos[j + 1];
      todoCount--;
    }
    todoSave();  // removal is durable
    logInfo("To-do item %d removed (%d left)", idx, todoCount);
    respond(true, "To-do item removed");
  } else {
    // Save the list as-is; empty items are kept.
    todoSave();  // persist to /sdcard/todo.md
    logInfo("To-do saved (%d items)", todoCount);
    respond(true, "To-do saved");
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
  server.on("/stats", HTTP_GET, handleStats);    // curl-friendly JSON snapshot
  server.on("/history", HTTP_GET, handleHistory);  // recent samples for the NOW chart
  server.on("/save", HTTP_POST, handleSave);
  server.on("/claude", HTTP_POST, handleClaude);
  server.on("/gdoc", HTTP_POST, handleGdoc);
  server.on("/tz", HTTP_POST, handleTz);
  server.on("/intervals", HTTP_POST, handleIntervals);
  server.on("/weatheradd", HTTP_POST, handleWeatherAdd);
  server.on("/weatherdel", HTTP_POST, handleWeatherDel);
  // Serve the cached third-party assets (Bootstrap) from SD for offline use.
  for (int i = 0; i < assetCount(); i++)
    server.on(assetAt(i)->route, HTTP_GET, handleAsset);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/wifiedit", HTTP_POST, handleWifiEdit);
  server.on("/wifisave", HTTP_POST, handleWifiSave);
  server.on("/wifiorder", HTTP_POST, handleWifiOrder);
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
