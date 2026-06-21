// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "src/bsp/ST7305_U8g2.h"
#include "src/bsp/i2c_bsp.h"
#include "src/bsp/codec_bsp.h"               // CodecPort
#include "src/logging/logging.h"             // logDebug / logInfo / logWarn / logError
#include "src/wifi_net/wifi_net.h"           // wifiBegin / wifiConnected / wifiIP / wifiSSID
#include "src/time_sync/time_sync.h"         // timeBegin / timeFormatDateTime
#include "src/weather/weather.h"             // City, cities[], weatherUpdateAll
#include "src/weather/draw.h"                // drawWeatherRow (weather rendering)
#include "src/sensors/sensors.h"             // sensorsBegin / sensorsPresent / sensorsRead
#include "src/battery/battery.h"             // batteryBegin / batteryUpdate / batteryPercent
#include "src/web_ui/web_ui.h"               // webBegin / webHandle / webTodo*
#include "src/claude_usage/claude_usage.h"   // claudeUsageUpdate / claudeFiveHour / claudeSevenDay
#include "src/claude_usage/clawd_icon.h"      // clawd_icon_bits — mascot drawn left of the usage gauges
#include "src/gdoc/gdoc.h"                    // gdocUpdate / gdocLineCount / gdocLine — Google Doc notes
#include "src/sdcard/sdcard.h"                // sdBegin / sdFormat / sdReadText / sdWriteText — microSD storage
#include "src/config/config.h"               // configBegin — shared persistent settings store (esp32.json)
#include "src/asset_cache/asset_cache.h"     // assetsEnsureFresh — cache Bootstrap on SD for offline web UI
#include "src/history/history.h"             // historyAdd — temp/humidity ring buffer + CSV logging

// ---------- RLCD SPI pins ----------
#define RLCD_SCK_PIN 11
#define RLCD_MOSI_PIN 12
#define RLCD_DC_PIN 5
#define RLCD_CS_PIN 40
#define RLCD_RST_PIN 41

// ---------- buttons (active-low; see Waveshare button_bsp) ----------
#define KEY_PIN 18   // "GP18" button
#define BOOT_PIN 0   // BOOT button; also the download strapping pin. Cycles the on-screen view.

#define DISP_W 400
#define DISP_H 300

// One I2C bus shared by the SHTC3 sensor and the audio codec (scl=14, sda=13, port 0)
I2cMasterBus I2cbus(14, 13, 0);
CodecPort *codec = nullptr;

ST7305_U8g2 lcd(RLCD_SCK_PIN, RLCD_MOSI_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN);
U8G2 *u8g2 = nullptr;

const unsigned long SAMPLE_INTERVAL = 10 * 1000;
const unsigned long SAMPLE_PRINT_INTERVAL = 10UL * 60 * 1000;  // log temp/humidity once per this span (a multiple of SAMPLE_INTERVAL)
const unsigned long HISTORY_INTERVAL = 60UL * 1000;            // append one temp/humidity sample to the yearly CSV per minute
const unsigned long WEATHER_INTERVAL = 10UL * 60 * 1000;
// Claude-usage and Google-Doc refresh intervals are user-configurable (minutes)
// and live in esp32.json — see claudeUsageIntervalMin() / gdocIntervalMin().
unsigned long lastSample = 0;
unsigned long lastHistory = 0;
unsigned long lastWeather = 0;
unsigned long lastClaudeUsage = 0;
unsigned long lastGdoc = 0;
unsigned long sampleCount = 0;

// latest sensor readings cached for redraw
float lastTemp = NAN, lastHum = NAN;
bool sensorOK = false;

// KEY debounce state
int keyPrev = HIGH;
unsigned long keyLastChange = 0;

// Tracks the Wi-Fi link so loop() can auto-refresh on the disconnected->connected
// edge. Seeded at the end of setup() so a boot-time connection doesn't re-refresh.
bool wifiWasConnected = false;

// Screen views, cycled by the BOOT button.
enum View { VIEW_OVERVIEW = 0, VIEW_GDOC, VIEW_TODO, VIEW_COUNT };
int currentView = VIEW_OVERVIEW;
int bootPrev = HIGH;
unsigned long bootLastChange = 0;

// ---------- chime ----------
// Play a sequence of notes as 16-bit stereo PCM through the codec, each note
// fading in/out so the transitions don't click.
static void playNotes(const int *noteFreqs, int numNotes, int noteMs) {
  if (!codec) return;
  const int sampleRate = 16000;
  codec->CodecPort_SetInfo("es8311", 1, sampleRate, 2, 16);  // open playback
  codec->CodecPort_SetSpeakerVol(85);

  for (int n = 0; n < numNotes; n++) {
    int samples = sampleRate * noteMs / 1000;
    for (int i = 0; i < samples; i += 64) {
      int16_t buf[64 * 2];  // stereo interleaved
      int chunk = (samples - i < 64) ? (samples - i) : 64;
      for (int j = 0; j < chunk; j++) {
        float t = (float)(i + j) / sampleRate;
        // simple fade envelope to avoid clicks
        float env = 1.0f;
        int pos = i + j;
        if (pos < 200) env = pos / 200.0f;
        else if (pos > samples - 200) env = (samples - pos) / 200.0f;
        int16_t s = (int16_t)(env * 9000.0f * sinf(2 * PI * noteFreqs[n] * t));
        buf[j * 2] = s;
        buf[j * 2 + 1] = s;
      }
      codec->CodecPort_PlayWrite(buf, chunk * 2 * sizeof(int16_t));
    }
  }
}

// Short single-note blip for immediate KEY-press feedback.
void playChimeShort() {
  const int notes[1] = { 1175 };  // D6
  playNotes(notes, 1, 90);
}

// Longer ascending jingle signalling a refresh (or boot) has finished.
void playChimeLong() {
  const int notes[4] = { 880, 1047, 1319, 1568 };  // A5 - C6 - E6 - G6
  playNotes(notes, 4, 140);
}

void drawThermometer(int x, int y, int h) {
  int sW = h / 6, bR = h / 6, sX = x + bR - sW / 2, sT = y, sB = y + h - 2 * bR;
  u8g2->drawRFrame(sX, sT, sW, sB - sT, sW / 2);
  u8g2->drawCircle(x + bR, y + h - bR, bR, U8G2_DRAW_ALL);
  u8g2->drawDisc(x + bR, y + h - bR, bR - 2, U8G2_DRAW_ALL);
  int fT = sT + (sB - sT) / 2;
  u8g2->drawBox(sX + 1, fT, sW - 2, sB - fT);
}
void drawDroplet(int x, int y, int h) {
  int r = h / 3, cx = x + r, cy = y + h - r;
  u8g2->drawDisc(cx, cy, r, U8G2_DRAW_ALL);
  u8g2->drawTriangle(cx, y, cx - r, cy, cx + r, cy);
}

// Render the web-edited to-do list inside a framed box. Each item gets a small
// checkbox (X'd + struck through when done) and is clipped to the box width.
void drawTodoBox(int x, int y, int w, int h) {
  u8g2->drawFrame(x, y, w, h);
  u8g2->setFont(u8g2_font_6x10_tf);
  u8g2->drawStr(x + 6, y + 11, "To-do");
  u8g2->drawHLine(x + 4, y + 15, w - 8);

  int n = webTodoCount();
  if (n == 0) {
    u8g2->drawStr(x + 6, y + 28, "(empty)");
    return;
  }
  int maxChars = (w - 20) / 6;  // chars that fit after the checkbox
  if (maxChars > 64) maxChars = 64;
  int ty = y + 28;  // first item's text baseline
  for (int i = 0; i < n && ty <= y + h - 4; i++) {
    int cbx = x + 6, cby = ty - 8;
    u8g2->drawFrame(cbx, cby, 8, 8);
    if (webTodoDone(i)) {
      u8g2->drawLine(cbx, cby, cbx + 7, cby + 7);
      u8g2->drawLine(cbx + 7, cby, cbx, cby + 7);
    }
    char line[66];
    snprintf(line, sizeof(line), "%.*s", maxChars, webTodoText(i));
    u8g2->drawStr(cbx + 12, ty, line);
    if (webTodoDone(i)) u8g2->drawHLine(cbx + 12, ty - 3, (int)strlen(line) * 6);
    ty += 13;
  }
}

// Render the fetched Google Doc lines inside a framed box. Unlike the rest of the
// UI, this box uses a GB2312 font (ASCII + Chinese) and drawUTF8, so Chinese text
// renders here. Overflow is clipped to the box interior, so we don't have to
// char-count (which is unreliable with mixed half/full-width glyphs).
void drawDocBox(int x, int y, int w, int h) {
  u8g2->drawFrame(x, y, w, h);
  u8g2->setFont(u8g2_font_wqy12_t_gb2312);
  u8g2->setClipWindow(x + 1, y + 1, x + w - 1, y + h - 1);  // keep text inside the box

  const char *t = gdocTitle();
  u8g2->drawUTF8(x + 6, y + 13, (t && t[0]) ? t : "Doc");  // doc title (may be Chinese)
  u8g2->drawHLine(x + 4, y + 17, w - 8);

  int n = gdocLineCount();
  if (n == 0) {
    u8g2->drawUTF8(x + 6, y + 31, gdocOk() ? "(empty)" : "(no data)");
  } else {
    int ty = y + 31;  // first line's baseline
    for (int i = 0; i < n && ty <= y + h - 3; i++) {
      u8g2->drawUTF8(x + 6, ty, gdocLine(i));
      ty += 14;  // line height for the 12px GB2312 font
    }
  }
  u8g2->setMaxClipWindow();  // restore the full drawing area for the rest of the UI
}

// A labeled 0-100% utilization bar: "<label> [===    ] NN%", drawn from (x, y).
void drawUsageBar(int x, int y, int w, const char *label, float pct) {
  u8g2->setFont(u8g2_font_6x10_tf);
  u8g2->drawStr(x, y + 8, label);
  int barH = 8;
  int barX = x + 28;
  int pctW = 36;  // room for the trailing "NN%"
  int barW = (x + w) - pctW - barX;
  if (barW < 8) barW = 8;
  u8g2->drawRFrame(barX, y, barW, barH, barH / 2);
  if (!isnan(pct)) {
    float f = pct / 100.0f;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    int fillW = (int)(f * (barW - 2));
    if (fillW > 0) u8g2->drawBox(barX + 1, y + 1, fillW, barH - 2);
    char s[8];
    snprintf(s, sizeof(s), "%.0f%%", pct);
    u8g2->drawStr(barX + barW + 6, y + 8, s);
  } else {
    u8g2->drawStr(barX + barW + 6, y + 8, "--");
  }
}

// Overview view: temp/humidity row, weather rows, the Notes box, Claude usage, and
// the to-do box — the original composited layout between the header and footer.
void drawOverview(int mx, int lineW) {
  char buf[40];

  // Temperature + humidity on one row in the upper-left under the date:
  // thermometer + value on the left, droplet + value on the right. Each icon is
  // offset by its own bulb radius so it sits centered over its column.
  const int icoH = 22, iconY = 46;    // ~80% icon, pulled up to keep the temp/humi band tight
  const int tempCx = mx + 14;         // thermometer column center
  const int humiCx = mx + 14 + 90;    // droplet column center, 90 px to the right
  const int textBase = iconY + icoH;  // value baseline at the icon bottom
  u8g2->setFont(u8g2_font_helvB12_tf);

  drawThermometer(tempCx - icoH / 6, iconY, icoH);
  if (sensorOK && !isnan(lastTemp)) snprintf(buf, sizeof(buf), "%.1f C", lastTemp);
  else snprintf(buf, sizeof(buf), "-- C");
  u8g2->drawStr(tempCx + 19, textBase, buf);

  drawDroplet(humiCx - icoH / 3, iconY, icoH);
  if (sensorOK && !isnan(lastHum)) snprintf(buf, sizeof(buf), "%.1f %%", lastHum);
  else snprintf(buf, sizeof(buf), "-- %%");
  u8g2->drawStr(humiCx + 19, textBase, buf);

  // Google Doc box to the right of the temp/humidity column.
  drawDocBox(190, 38, DISP_W - 190 - 8, 130);

  // Separator under the temp/humidity column — left column only, clear of the to-do box.
  // Pulled up so temp/humi stays compact and the weather rows below get more room.
  u8g2->drawHLine(mx, 82, 172);

  // Weather: one horizontal temperature-gauge row per city, started higher and
  // spaced wider to fill the larger band down to the y=172 divider.
  int wy = 90;
  for (int i = 0; i < weatherCityCount(); i++) {
    drawWeatherRow(mx, wy, 172, cities[i]);
    wy += 36;
  }

  // Divider below the main content (weather column + to-do box).
  u8g2->drawHLine(mx, 172, lineW);

  // The region below the second divider is split in two: Claude usage on the left
  // half, the Google Doc "Notes" box on the right half.
  const int halfGap = 10;
  const int halfW = (lineW - halfGap) / 2;
  const int rightX = mx + halfW + halfGap;

  // Claude usage (left half). The title keeps its full width within the half; the
  // "As of" timestamp drops to a small second line so it fits.
  u8g2->setFont(u8g2_font_6x12_tf);
  u8g2->drawStr(mx, 187, "Claude Usage");
  u8g2->setFont(u8g2_font_5x7_tf);
  time_t cuAsOf = claudeUsageAsOf();
  if (cuAsOf > 0) {
    struct tm cuTm;
    localtime_r(&cuAsOf, &cuTm);
    char asOfStr[28];
    strftime(asOfStr, sizeof(asOfStr), "As of %Y-%m-%d %H:%M", &cuTm);
    u8g2->drawStr(mx, 197, asOfStr);
  } else {
    u8g2->drawStr(mx, 197, "never updated");
  }
  // Mascot on the left; the two gauges are shifted right to make room for it.
  const int clawdGap = 6;
  const int gaugeX = mx + CLAWD_ICON_W + clawdGap;
  const int gaugeW = halfW - (CLAWD_ICON_W + clawdGap);
  u8g2->drawXBMP(mx, 222 - CLAWD_ICON_H / 2, CLAWD_ICON_W, CLAWD_ICON_H, clawd_icon_bits);
  drawUsageBar(gaugeX, 210, gaugeW, "5h", claudeFiveHour());
  drawUsageBar(gaugeX, 226, gaugeW, "7d", claudeSevenDay());

  // To-do list (right half), in a framed box.
  drawTodoBox(rightX, 176, mx + lineW - rightX, 102);
}

// A small lightning bolt (two interlocking filled triangles) drawn via XOR so it
// stays visible over either the filled or empty part of the battery glyph.
static void drawBolt(int cx, int y0) {
  u8g2->setDrawColor(2);  // XOR
  u8g2->drawTriangle(cx + 1, y0, cx - 2, y0 + 5, cx + 1, y0 + 5);
  u8g2->drawTriangle(cx - 1, y0 + 9, cx + 2, y0 + 4, cx - 1, y0 + 4);
  u8g2->setDrawColor(1);
}

// Battery indicator: a battery glyph filled proportionally to charge (held SoC
// while charging), the percentage just to its left, the realtime terminal
// voltage further left, and a bolt over the glyph while charging. rightX is the
// right edge of the glyph.
void drawBattery(int rightX, int topY) {
  const int bw = 22, bh = 11, nub = 2;  // body width/height + positive-terminal nub
  int bx = rightX - nub - bw;
  int pct = batteryPercent();
  bool chg = batteryCharging();
  u8g2->drawFrame(bx, topY, bw, bh);
  u8g2->drawBox(bx + bw, topY + (bh - 4) / 2, nub, 4);
  if (pct >= 0) {
    int fw = (bw - 4) * pct / 100;
    if (fw > 0) u8g2->drawBox(bx + 2, topY + 2, fw, bh - 4);
  }
  if (chg) drawBolt(bx + bw / 2, topY + 1);

  u8g2->setFont(u8g2_font_6x10_tf);
  int baseY = topY + bh - 1;

  // percentage just left of the glyph
  char s[8];
  if (pct >= 0) snprintf(s, sizeof(s), "%d%%", pct);
  else snprintf(s, sizeof(s), "--%%");
  int sx = bx - 4 - u8g2->getStrWidth(s);
  u8g2->drawStr(sx, baseY, s);

  // realtime terminal voltage, always shown, further left
  char v[10];
  if (!isnan(batteryVoltage())) snprintf(v, sizeof(v), "%.2fV", batteryVoltage());
  else snprintf(v, sizeof(v), "--V");
  u8g2->drawStr(sx - 5 - u8g2->getStrWidth(v), baseY, v);
}

void drawScreen() {
  u8g2->clearBuffer();
  u8g2->setDrawColor(1);
  char buf[80];
  const int mx = 12;                  // left margin, pulled toward the left edge
  const int lineW = DISP_W - 8 - mx;  // full-width lines run from mx to the to-do box's right edge

  u8g2->setFont(u8g2_font_6x13_tf);
  if (timeFormatDateTime(buf, sizeof(buf))) u8g2->drawStr(mx, 24, buf);
  else u8g2->drawStr(mx, 24, "Syncing time...");
  drawBattery(DISP_W - 8, 12);  // top-right corner, above the header divider
  u8g2->drawHLine(mx, 30, lineW);

  // Body depends on the selected view (cycled by the BOOT button). The gdoc/to-do
  // views fill the whole band between the date header and the Wi-Fi footer.
  if (currentView == VIEW_GDOC) {
    drawDocBox(mx, 36, lineW, 244);
  } else if (currentView == VIEW_TODO) {
    drawTodoBox(mx, 36, lineW, 244);
  } else {
    drawOverview(mx, lineW);
  }

  // Wi-Fi footer pinned to the bottom, with a divider right above it.
  u8g2->drawHLine(mx, 283, lineW);
  u8g2->setFont(u8g2_font_5x7_tf);
  if (wifiConnected()) {
    snprintf(buf, sizeof(buf), "SSID: %s    IP: %s    mDNS: %s.local", wifiSSID(), wifiIP().c_str(), wifiHostname());
    u8g2->drawStr(mx, 294, buf);
  } else if (wifiStatus()[0]) {
    u8g2->drawStr(mx, 294, wifiStatus());  // e.g. "Trying <ssid>" while wifiBegin() iterates
  } else u8g2->drawStr(mx, 294, "WiFi: disconnected");

  u8g2->sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  lcd.begin(0, U8G2_R1);
  u8g2 = lcd.getU8g2();

  pinMode(KEY_PIN, INPUT_PULLUP);
  pinMode(BOOT_PIN, INPUT_PULLUP);  // BOOT button cycles the on-screen view

  // SHTC3 + codec share I2cbus
  sensorsBegin(I2cbus);
  sensorOK = sensorsPresent();
  if (sensorOK) sensorsRead(&lastTemp, &lastHum);  // populate temp/humidity before the first drawScreen (no Wi-Fi needed)
  batteryBegin();                                  // prime the battery reading before the first drawScreen
  codec = new CodecPort(I2cbus, "S3_RLCD_4_2");

  // microSD: mount, then load persisted creds + saved Wi-Fi networks BEFORE
  // connecting, so wifiBegin() can try the saved networks.
  sdBegin();
  configBegin();   // load small persistent settings (esp32.json) before features read them
  historyBegin();  // ensure /sdcard/sensor_data exists for the temp/humidity logs

  // ============================ ONE-TIME SD FORMAT ============================
  // Wipes the card to a fresh FAT filesystem on EVERY boot — here only to prepare
  // a raw/unreadable card. >>> COMMENT OUT the sdFormat() line below <<< once the
  // card is prepared, otherwise the persisted settings + Wi-Fi list are erased
  // each boot. The line after it re-seeds a network so the freshly wiped card
  // isn't left empty; harmless to keep (or remove together).
  sdFormat();
  // ===========================================================================

  // Settings that come from config (esp32.json) need configBegin() first (done above).
  claudeUsageLoad();  // restore the Claude org id + session key from config
  wifiLoadNetworks();
  gdocLoadUrl();  // restore the configured Google Doc URL before the first gdocUpdate()
  timeLoadZones();  // restore the selected primary/secondary time zones before timeBegin()
  weatherLoadCities();  // restore the configured weather cities before the first weatherUpdateAll()

  drawScreen();
  wifiSetRedrawHook(drawScreen);  // let wifiBegin() show "Trying <ssid>" on the footer
  wifiBegin();
  timeBegin();
  drawScreen();

  // Start the LAN server FIRST, before the slow/hang-prone network fetches below
  // (asset cache, weather, Claude, gdoc), so http://esp32.local stays reachable
  // even if one of those stalls. The page falls back to the CDN for any asset
  // not cached yet.
  webBegin();
  drawScreen();

  assetsEnsureFresh();  // refresh the cached web-UI assets (Bootstrap/Chart.js) if stale + online
  drawScreen();

  weatherUpdateAll();
  lastWeather = millis();
  drawScreen();

  claudeUsageUpdate();
  lastClaudeUsage = millis();
  drawScreen();

  gdocUpdate();
  lastGdoc = millis();
  drawScreen();

  playChimeLong();  // boot updates done
  wifiWasConnected = wifiConnected();  // seed the edge detector; boot already refreshed
}

// Force-refresh every network feed (weather, Claude usage, Google Doc), reset the
// periodic timers so the next auto-refresh is a full interval away, and redraw.
// Shared by the KEY button and the on-(re)connect trigger in loop().
void refreshAll() {
  weatherUpdateAll();
  claudeUsageUpdate();
  gdocUpdate();
  lastWeather = millis();
  lastClaudeUsage = millis();
  lastGdoc = millis();
  drawScreen();  // show the freshly fetched data
}

void loop() {
  wifiEnsureConnected();
  wifiLoop();   // tear down the first-time setup AP once a real network is joined
  webHandle();  // serve any pending HTTP requests (kept out of the sample gate so it stays responsive)

  // Auto-refresh on the disconnected->connected edge (e.g. just after first-time
  // setup), exactly like a KEY press, so the screen fills in as soon as we're online.
  bool wifiNow = wifiConnected();
  if (wifiNow && !wifiWasConnected) {
    logInfo("WiFi connected -> auto refresh");
    refreshAll();
    playChimeLong();  // updates done
  }
  wifiWasConnected = wifiNow;

  // KEY button: debounce; on press (HIGH->LOW) chime + force-refresh weather/Claude usage
  int k = digitalRead(KEY_PIN);
  if (k != keyPrev && millis() - keyLastChange > 40) {
    keyLastChange = millis();
    if (k == LOW) {
      logInfo("KEY pressed -> chime + refresh");
      playChimeShort();  // immediate press feedback
      refreshAll();
      playChimeLong();   // updates done
    }
    keyPrev = k;
  }

  // BOOT button: debounce; on press cycle the view (overview -> gdoc -> to-do).
  int b = digitalRead(BOOT_PIN);
  if (b != bootPrev && millis() - bootLastChange > 40) {
    bootLastChange = millis();
    if (b == LOW) {
      currentView = (currentView + 1) % VIEW_COUNT;
      logInfo("BOOT pressed -> view %d", currentView);
      drawScreen();
    }
    bootPrev = b;
  }

  unsigned long now = millis();
  if (now - lastWeather >= WEATHER_INTERVAL) {
    lastWeather = now;
    weatherUpdateAll();
  }

  if (now - lastClaudeUsage >= claudeUsageIntervalMin() * 60UL * 1000UL) {
    lastClaudeUsage = now;
    claudeUsageUpdate();
  }

  if (now - lastGdoc >= gdocIntervalMin() * 60UL * 1000UL) {
    lastGdoc = now;
    gdocUpdate();
  }

  if (now - lastSample >= SAMPLE_INTERVAL) {
    lastSample = now;
    batteryUpdate();  // refresh the battery gauge alongside the temp/humidity sample
    bool gotReading = sensorOK && sensorsRead(&lastTemp, &lastHum);
    // print temp/humidity once every SAMPLE_PRINT_INTERVAL (= every Nth sample)
    if (gotReading && ++sampleCount % (SAMPLE_PRINT_INTERVAL / SAMPLE_INTERVAL) == 0)
      logInfo("Temperature: %.2f C  Humidity: %.2f %%", lastTemp, lastHum);

    // Append a sample to the yearly CSV once a minute, but only after the clock
    // is set so the timestamps are real wall-clock time.
    if (gotReading && time(nullptr) > 1700000000 && now - lastHistory >= HISTORY_INTERVAL) {
      lastHistory = now;
      historyAdd(time(nullptr), lastTemp, lastHum);
    }
    drawScreen();
  }
}
