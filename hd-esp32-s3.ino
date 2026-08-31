// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "src/bsp/ST7305_U8g2.h"
#include "src/bsp/i2c_bsp.h"
#include "src/bsp/codec_bsp.h"               // CodecPort
#include "src/version/version.h"                 // FW_VERSION — stamped by CI
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
#include "src/codex_usage/codex_usage.h"    // codexUsageUpdate / codexPrimaryPercent — Codex (ChatGPT) limits
#include "src/codex_usage/codex_icon.h"     // codex_icon_bits — OpenAI mark drawn left of the Codex gauges
#include "src/gdoc/gdoc.h"                    // gdocUpdate / gdocLineCount / gdocLine — Google Doc notes
#include "src/ota/ota.h"                     // otaBegin / otaHandle — LAN firmware updates
#include "src/sdcard/sdcard.h"                // sdBegin / sdFormat / sdReadText / sdWriteText — microSD storage
#include "src/config/config.h"               // configBegin — shared persistent settings store (esp32.json)
#include "src/asset_cache/asset_cache.h"     // assetsEnsureFresh — cache Bootstrap on SD for offline web UI
#include "src/history/history.h"             // historyAdd/historyAddBattery — per-year CSV logging
#include "src/netsync/netsync.h"             // locks + version counter shared with the fetch task

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

// On-screen x centers of the three top-edge buttons, which run KEY, PWR, BOOT
// from left to right. The header clock is drawn in the fixed-width 6x13 font from
// x=12, and KEY (refresh) sits above its second ':' (column 25) while PWR sits
// above the closing ')' of the secondary time (column 32) -- i.e. 165 and 207.
// Extrapolating that 42 px pitch put BOOT at 249, which read a touch too far
// left against the real button, so it is trimmed by eye: nudge BTN_BOOT_X alone
// if the popup's [x] still doesn't line up. Used to line an on-screen control up
// with the button that works it.
#define BTN_KEY_X 165
#define BTN_PWR_X 207
#define BTN_BOOT_X 257

// One I2C bus shared by the SHTC3 sensor and the audio codec (scl=14, sda=13, port 0)
I2cMasterBus I2cbus(14, 13, 0);
CodecPort *codec = nullptr;

ST7305_U8g2 lcd(RLCD_SCK_PIN, RLCD_MOSI_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN);
U8G2 *u8g2 = nullptr;

const unsigned long SAMPLE_INTERVAL = 10 * 1000;
const unsigned long SAMPLE_PRINT_INTERVAL = 10UL * 60 * 1000;  // log temp/humidity once per this span (a multiple of SAMPLE_INTERVAL)
const unsigned long HISTORY_INTERVAL = 60UL * 1000;            // append one temp/humidity + battery sample to the yearly CSVs per minute
const unsigned long WEATHER_INTERVAL = 10UL * 60 * 1000;
// Claude-usage, Codex-usage and Google-Doc refresh intervals are user-configurable
// (minutes) and live in esp32.json — see claudeUsageIntervalMin() /
// codexUsageIntervalMin() / gdocIntervalMin().
unsigned long lastSample = 0;
unsigned long lastHistory = 0;
unsigned long lastWeather = 0;
unsigned long lastClaudeUsage = 0;
unsigned long lastCodexUsage = 0;
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
  // Same GB2312 font + drawUTF8 as the doc box, so Chinese items render here too.
  u8g2->setFont(u8g2_font_wqy12_t_gb2312);
  u8g2->setClipWindow(x + 1, y + 1, x + w - 1, y + h - 1);  // keep text inside the box
  u8g2->drawUTF8(x + 6, y + 13, "To-do");
  u8g2->drawHLine(x + 4, y + 17, w - 8);

  int n = webTodoCount();
  if (n == 0) {
    u8g2->drawUTF8(x + 6, y + 31, "(empty)");
    u8g2->setMaxClipWindow();
    return;
  }
  const int textX = x + 18;              // text starts after the checkbox
  const int textRight = x + w - 6;       // right edge available to the text
  int ty = y + 31;                       // first item's text baseline
  for (int i = 0; i < n && ty <= y + h - 3; i++) {
    int cbx = x + 6, cby = ty - 8;
    u8g2->drawFrame(cbx, cby, 8, 8);
    if (webTodoDone(i)) {
      u8g2->drawLine(cbx, cby, cbx + 7, cby + 7);
      u8g2->drawLine(cbx + 7, cby, cbx, cby + 7);
    }
    // Overflow is clipped by the clip window rather than char-counted, which is
    // unreliable with mixed half/full-width glyphs (and would cut UTF-8 mid-sequence).
    const char *t = webTodoText(i);
    u8g2->drawUTF8(textX, ty, t);
    if (webTodoDone(i)) {
      int tw = u8g2->getUTF8Width(t);
      if (tw > textRight - textX) tw = textRight - textX;
      if (tw > 0) u8g2->drawHLine(textX, ty - 4, tw);
    }
    ty += 14;  // line height for the 12px GB2312 font
  }
  u8g2->setMaxClipWindow();  // restore the full drawing area for the rest of the UI
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

// Popup listing what changed in the Google Doc since the previous revision. It is
// drawn last, on top of whatever view is showing, so the normal screen stays
// visible around it. The title bar carries a single [x] close control, placed at
// BOOT's own x and against the screen's top edge so it sits directly under the
// button that dismisses it (see loop()); the popup is up for as long as
// gdocDiffCount() is non-zero, and a further doc update just appends to the diff
// already waiting there.
// Overflowing content ends in "..." rather than being silently cut.
void drawDocPopup() {
  const int w = 320;
  const int y = 2;       // hugged to the top edge: the title bar's [x] then sits
                         // just below the button that presses it, so the two read
                         // as one control. The bottom edge stays where it was.
  const int bottom = 240;
  const int h = bottom - y;
  const int x = (DISP_W - w) / 2;
  const int barH = 16;

  u8g2->setDrawColor(0);
  u8g2->drawBox(x, y, w, h);  // punch a hole in the view underneath
  u8g2->setDrawColor(1);
  u8g2->drawFrame(x, y, w, h);
  u8g2->drawFrame(x + 1, y + 1, w - 2, h - 2);  // double border so it reads as "on top"
  u8g2->drawHLine(x + 1, y + barH, w - 2);

  u8g2->setFont(u8g2_font_6x12_tf);
  u8g2->drawStr(x + 6, y + 12, "Doc updated");

  // Close control: a boxed cross centered on the BOOT button's column.
  const int bs = 11;  // button box side
  int bx = BTN_BOOT_X - bs / 2, by = y + (barH - bs) / 2;
  u8g2->drawFrame(bx, by, bs, bs);
  u8g2->drawLine(bx + 3, by + 3, bx + bs - 4, by + bs - 4);
  u8g2->drawLine(bx + bs - 4, by + 3, bx + 3, by + bs - 4);

  // Changed lines, in the GB2312 font so Chinese renders like the Notes box does.
  u8g2->setFont(u8g2_font_wqy12_t_gb2312);
  u8g2->setClipWindow(x + 2, y + barH + 1, x + w - 2, y + h - 2);
  const int lineH = 14;
  const int firstBase = y + barH + 13;
  int rows = (y + h - 4 - firstBase) / lineH + 1;  // baselines that fit in the box
  int n = gdocDiffCount();
  bool more = (n > rows) || gdocDiffTruncated();
  int shown = more ? rows - 1 : n;  // last row is given to the "..." marker
  int ty = firstBase;
  for (int i = 0; i < shown; i++) {
    u8g2->drawUTF8(x + 6, ty, gdocDiffLine(i));
    ty += lineH;
  }
  if (more) u8g2->drawUTF8(x + 6, ty, "...");
  u8g2->setMaxClipWindow();
}

// Progress box shown while an OTA image is being received. loop() is parked inside
// otaHandle() for the duration, so this is painted from the ota module's redraw
// hook rather than the normal draw cadence.
void drawOtaPopup() {
  const int w = 260, h = 58;
  const int x = (DISP_W - w) / 2, y = (DISP_H - h) / 2;
  u8g2->setDrawColor(0);
  u8g2->drawBox(x, y, w, h);
  u8g2->setDrawColor(1);
  u8g2->drawFrame(x, y, w, h);
  u8g2->drawFrame(x + 1, y + 1, w - 2, h - 2);
  u8g2->setFont(u8g2_font_6x12_tf);
  u8g2->drawStr(x + 10, y + 20, otaStatus());
  drawUsageBar(x + 10, y + 32, w - 20, "OTA", (float)otaPercent());
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

// A usage section heading: "<Title>  As of yyyy-mm-dd hh:mm" on one baseline, the
// timestamp in a smaller font right after the title (or "never updated" before the
// first successful fetch).
void drawUsageHeading(int x, int y, const char *title, time_t asOf) {
  u8g2->setFont(u8g2_font_6x12_tf);
  u8g2->drawStr(x, y, title);
  int titleW = u8g2->getStrWidth(title);  // measure while the title font is active
  u8g2->setFont(u8g2_font_5x7_tf);
  if (asOf > 0) {
    struct tm asOfTm;
    localtime_r(&asOf, &asOfTm);
    char asOfStr[28];
    strftime(asOfStr, sizeof(asOfStr), "As of %Y-%m-%d %H:%M", &asOfTm);
    u8g2->drawStr(x + titleW + 6, y, asOfStr);
  } else {
    u8g2->drawStr(x + titleW + 6, y, "never updated");
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
  int shown = weatherCityCount() < weatherShownMax() ? weatherCityCount() : weatherShownMax();
  int nameW = weatherNameColWidth(cities, shown);  // align every gauge to the longest name
  for (int i = 0; i < shown; i++) {
    drawWeatherRow(mx, wy, 172, cities[i], nameW);
    wy += 36;
  }

  // Divider below the main content (weather column + to-do box).
  u8g2->drawHLine(mx, 172, lineW);

  // The region below the second divider is split in two: Claude usage on the left
  // half, the Google Doc "Notes" box on the right half.
  const int halfGap = 10;
  const int halfW = (lineW - halfGap) / 2;
  const int rightX = mx + halfW + halfGap;

  // Claude usage, then Codex usage, stacked in the left half. Each is a heading
  // row (title + fetch time) above a mascot with its gauges to the right. Both
  // gauge columns start after the widest icon so they line up with each other.
  const int iconGap = 6;
  const int iconSlotW = CLAWD_ICON_W;  // widest of the two marks
  const int gaugeX = mx + iconSlotW + iconGap;
  const int gaugeW = halfW - (iconSlotW + iconGap);

  drawUsageHeading(mx, 184, "Claude Usage", claudeUsageAsOf());
  u8g2->drawXBMP(mx, 202 - CLAWD_ICON_H / 2, CLAWD_ICON_W, CLAWD_ICON_H, clawd_icon_bits);
  drawUsageBar(gaugeX, 190, gaugeW, "5h", claudeFiveHour());
  drawUsageBar(gaugeX, 206, gaugeW, "7d", claudeSevenDay());

  // Codex reports its windows generically (their lengths vary by plan, and Plus
  // has no secondary one), so the labels come from the reported window lengths;
  // with a single window its gauge is centred on the icon instead of paired.
  // Two spaces: 6x12 is fixed-width, so the extra cell makes up for "Codex" being
  // a character shorter than "Claude" and lines both "Usage" words up.
  drawUsageHeading(mx, 230, "Codex  Usage", codexUsageAsOf());
  u8g2->drawXBMP(mx + (iconSlotW - CODEX_ICON_W) / 2, 248 - CODEX_ICON_H / 2, CODEX_ICON_W,
                 CODEX_ICON_H, codex_icon_bits);
  if (!codexUsageHasToken() || codexTokenExpired()) {
    // The token is relayed in by the machine running the Codex CLI, so say which
    // half of that is missing rather than silently drawing empty gauges -- and
    // name the README section that explains how to set the relay up.
    u8g2->setFont(u8g2_font_5x7_tf);
    u8g2->drawStr(gaugeX, 245, codexUsageHasToken() ? "access token expired"
                                                    : "no access token relayed yet");
    u8g2->drawStr(gaugeX, 256, "see README: Codex usage relay");
  } else if (codexSecondaryWindowMin() > 0) {
    drawUsageBar(gaugeX, 236, gaugeW, codexPrimaryLabel(), codexPrimaryPercent());
    drawUsageBar(gaugeX, 252, gaugeW, codexSecondaryLabel(), codexSecondaryPercent());
  } else {
    drawUsageBar(gaugeX, 244, gaugeW, codexPrimaryLabel(), codexPrimaryPercent());
  }

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
int drawBattery(int rightX, int topY) {
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
  int vx = sx - 5 - u8g2->getStrWidth(v);
  u8g2->drawStr(vx, baseY, v);
  return vx;  // leftmost x drawn, so the caller can place an icon to the left
}

// Small SD-card glyph at top-left (x,y): a chamfered-corner body with contact
// pins. When the card is absent it's crossed out with an X.
void drawSdcardIcon(int x, int y, bool present) {
  const int w = 9, h = 12, c = 3;  // body width/height + top-right chamfer
  u8g2->drawLine(x, y, x + w - c, y);          // top edge up to the chamfer
  u8g2->drawLine(x + w - c, y, x + w, y + c);  // chamfered corner
  u8g2->drawVLine(x + w, y + c, h - c + 1);    // right edge
  u8g2->drawHLine(x, y + h, w + 1);            // bottom edge
  u8g2->drawVLine(x, y, h);                    // left edge
  for (int i = 0; i < 3; i++) u8g2->drawVLine(x + 2 + i * 2, y + 2, 2);  // contact pins
  if (!present) {  // cross it out to mean "no card"
    u8g2->drawLine(x, y, x + w, y + h);
    u8g2->drawLine(x + w, y, x, y + h);
  }
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
  int battLeft = drawBattery(DISP_W - 8, 12);  // top-right corner, above the header divider
  drawSdcardIcon(battLeft - 6 - 9, 12, sdMounted());  // SD status just left of the voltage
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

  // Firmware version, right-aligned on the same footer row: "dev" for a locally
  // built image, the CI stamp (tag or main-<sha>) otherwise. Drawn last so it
  // wins if a long SSID/IP line would otherwise reach this far.
  int fwW = u8g2->getStrWidth(FW_VERSION);
  u8g2->setDrawColor(0);
  u8g2->drawBox(mx + lineW - fwW - 3, 287, fwW + 3, 9);  // clear a slot for it
  u8g2->setDrawColor(1);
  u8g2->drawStr(mx + lineW - fwW, 294, FW_VERSION);

  // Popups go last so they overlay the view they interrupt; an in-flight firmware
  // update outranks the doc diff, which will still be waiting afterwards.
  if (otaActive()) drawOtaPopup();
  else if (gdocDiffCount() > 0) drawDocPopup();

  u8g2->sendBuffer();
}

// ---------- background fetch task ----------
// Every network feed used to be fetched inline from loop(), which parked the
// whole UI for the length of four blocking HTTPS round trips: buttons went dead,
// the web panel stopped answering, and the screen showed stale data throughout.
// The fetches now run here; loop() keeps drawing, polling buttons and serving
// HTTP, and repaints whenever this task commits something new.
static const uint32_t FETCH_TICK_MS = 200;
static const uint32_t NET_TRY_MS = 50;  // background: glance at the radio, don't wait for it
static const uint32_t FETCH_STACK = 10 * 1024;  // mbedTLS needs room; the loop task gets 8K

// Set to force a feed on the next tick, cleared only once it has actually run --
// so a feed skipped because the web UI held the radio is simply retried.
static volatile bool forceWeather = false, forceClaude = false, forceCodex = false, forceGdoc = false;
static volatile bool refreshInFlight = false;  // loop() chimes on the falling edge

// Ask for every feed to be refreshed now. Returns immediately; the task does the
// work. Used by the KEY button and the on-(re)connect trigger in loop().
void requestRefreshAll() {
  forceWeather = forceClaude = forceCodex = forceGdoc = true;
  refreshInFlight = true;
}

// Run one feed if it is due or forced. Returns true if it actually ran, so the
// caller can stop after one -- see fetchTask().
static bool maybeFetch(volatile bool *force, unsigned long *stamp, unsigned long intervalMs,
                       void (*fn)()) {
  if (!*force && millis() - *stamp < intervalMs) return false;
  NetGuard net(NET_TRY_MS);
  if (!net.ok) return false;  // the web UI is mid-TLS; try again next tick
  fn();  // stages a result; loop() promotes it in the drain below
  *stamp = millis();
  *force = false;
  return true;
}

static void fetchTask(void *) {
  for (;;) {
    // At most ONE feed per wake, so the network lock is released in between and
    // an interactive TLS call (the web UI's GitHub requests) gets a turn instead
    // of queueing behind a whole refresh round. `||` short-circuits after the
    // first one that runs.
    if (wifiConnected()) {
      maybeFetch(&forceClaude, &lastClaudeUsage, claudeUsageIntervalMin() * 60UL * 1000UL, claudeUsageFetch) ||
          maybeFetch(&forceCodex, &lastCodexUsage, codexUsageIntervalMin() * 60UL * 1000UL, codexUsageFetch) ||
          maybeFetch(&forceWeather, &lastWeather, WEATHER_INTERVAL, weatherFetch) ||
          maybeFetch(&forceGdoc, &lastGdoc, gdocIntervalMin() * 60UL * 1000UL, gdocFetch);
      if (refreshInFlight && !forceWeather && !forceClaude && !forceCodex && !forceGdoc)
        refreshInFlight = false;
    }
    vTaskDelay(pdMS_TO_TICKS(FETCH_TICK_MS));
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  logInfo("hd-esp32-s3 firmware %s", FW_VERSION);

  netsyncBegin();  // locks must exist before anything can draw or fetch

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
  // sdFormat();
  // ===========================================================================

  // Settings that come from config (esp32.json) need configBegin() first (done above).
  claudeUsageLoad();  // restore the Claude org id + session key from config
  codexUsageLoad();   // restore the relayed Codex access token from config
  wifiLoadNetworks();
  gdocLoadUrl();  // restore the configured Google Doc URL before the first gdocUpdate()
  timeLoadZones();  // restore the selected primary/secondary time zones before timeBegin()
  weatherLoadCities();  // restore the configured weather cities before the first weatherUpdateAll()

  drawScreen();
  wifiSetRedrawHook(drawScreen);  // let wifiBegin() show "Trying <ssid>" on the footer
  wifiBegin();
  otaSetRedrawHook(drawScreen);  // let the OTA progress box paint while loop() is parked
  otaBegin();                    // listen for `arduino-cli upload -p esp32.local` pushes
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

  codexUsageUpdate();
  lastCodexUsage = millis();
  drawScreen();

  gdocUpdate();
  lastGdoc = millis();
  drawScreen();

  playChimeLong();  // boot updates done
  wifiWasConnected = wifiConnected();  // seed the edge detector; boot already refreshed

  // Boot fetched everything synchronously above (the screen should be complete
  // before we reach loop()); from here on the feeds run in the background.
  xTaskCreate(fetchTask, "fetch", FETCH_STACK, nullptr, 1, nullptr);
}

// Re-read everything persisted on the SD card (call after a card is (re)mounted).
// configBegin() must run first since the other loaders read from the config store.
void reloadFromSd() {
  // Re-seeding the modules rewrites state the fetch task reads -- city names,
  // the doc URL, the API credentials. Taking the network lock the interactive
  // way waits out any fetch in flight and keeps the task out until we are done,
  // since it only touches that state while holding this lock.
  NetGuard net(0);
  configBegin();
  claudeUsageLoad();
  codexUsageLoad();
  wifiLoadNetworks();
  gdocLoadUrl();
  timeLoadZones();
  weatherLoadCities();
  webReloadTodo();
  historyBegin();  // recreate sensor_data/ on the (possibly new/blank) card
  drawScreen();
}

// Detect the SD card being pulled or (re)inserted at runtime and reload data on
// re-insert. Polls at a slow cadence: presence is a quick CMD13, but a remount
// attempt with no card blocks briefly, so we don't want to do it every loop.
void sdHotplugCheck() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 3000) return;
  lastCheck = millis();
  if (sdMounted()) {
    if (!sdCardPresent()) {
      logWarn("SD card removed");
      sdUnmount();
    }
  } else if (sdRemount()) {
    logInfo("SD card inserted -> reloading data");
    reloadFromSd();
  }
}


void loop() {
  wifiEnsureConnected();
  wifiLoop();   // tear down the first-time setup AP once a real network is joined
  otaHandle();  // accept a LAN firmware push (also starts the listener once Wi-Fi is up)
  webHandle();  // serve any pending HTTP requests (kept out of the sample gate so it stays responsive)
  sdHotplugCheck();  // reload persisted data if the card was pulled + re-inserted

  // Auto-refresh on the disconnected->connected edge (e.g. just after first-time
  // setup), exactly like a KEY press, so the screen fills in as soon as we're online.
  bool wifiNow = wifiConnected();
  if (wifiNow && !wifiWasConnected) {
    logInfo("WiFi connected -> auto refresh");
    requestRefreshAll();
  }
  wifiWasConnected = wifiNow;

  // KEY button: debounce; on press (HIGH->LOW) chime + force-refresh weather/Claude usage
  int k = digitalRead(KEY_PIN);
  if (k != keyPrev && millis() - keyLastChange > 40) {
    keyLastChange = millis();
    if (k == LOW) {
      logInfo("KEY pressed -> chime + refresh");
      playChimeShort();    // immediate press feedback
      requestRefreshAll();  // the fetch task picks this up; loop() keeps running
    }
    keyPrev = k;
  }

  // BOOT button: debounce; on press cycle the view (overview -> gdoc -> to-do).
  int b = digitalRead(BOOT_PIN);
  if (b != bootPrev && millis() - bootLastChange > 40) {
    bootLastChange = millis();
    if (b == LOW) {
      // While the doc-update popup is up BOOT is its close button (the on-screen
      // [x] is drawn right under it) instead of the view cycler.
      if (gdocDiffCount() > 0) {
        logInfo("BOOT pressed -> dismiss doc-update popup");
        gdocDiffClear();
      } else {
        currentView = (currentView + 1) % VIEW_COUNT;
        logInfo("BOOT pressed -> view %d", currentView);
      }
      drawScreen();
    }
    bootPrev = b;
  }

  unsigned long now = millis();

  // Drain: the fetch task stages results, this is where they become visible.
  // Promoting on the loop task means every value drawScreen() reads was also
  // written here, so the two threads share no mutable state and neither needs a
  // lock. Repainting per committed feed is what makes the screen fill in one
  // feed at a time rather than all at once at the end of a refresh.
  static bool wasRefreshing = false;
  bool changed = false;
  if (claudeUsageCommit()) changed = true;
  if (codexUsageCommit()) changed = true;
  if (weatherCommit()) changed = true;
  if (gdocCommit()) changed = true;
  if (changed) drawScreen();

  if (wasRefreshing && !refreshInFlight) playChimeLong();  // a full refresh just finished
  wasRefreshing = refreshInFlight;

  if (now - lastSample >= SAMPLE_INTERVAL) {
    lastSample = now;
    batteryUpdate();  // refresh the battery gauge alongside the temp/humidity sample
    bool gotReading = sensorOK && sensorsRead(&lastTemp, &lastHum);
    // print temp/humidity once every SAMPLE_PRINT_INTERVAL (= every Nth sample)
    if (gotReading && ++sampleCount % (SAMPLE_PRINT_INTERVAL / SAMPLE_INTERVAL) == 0)
      logInfo("Temperature: %.2f C  Humidity: %.2f %%", lastTemp, lastHum);

    // Append a sample to the yearly CSVs once a minute, but only after the clock
    // is set so the timestamps are real wall-clock time.
    if (time(nullptr) > 1700000000 && now - lastHistory >= HISTORY_INTERVAL) {
      lastHistory = now;
      time_t ts = time(nullptr);
      if (gotReading) historyAdd(ts, lastTemp, lastHum);
      // Battery has its own CSV, so it keeps being logged even on a boot where
      // the SHTC3 never answers.
      historyAddBattery(ts, batteryVoltage(), batteryPercent(), batteryCharging());
    }
    drawScreen();
  }
}
