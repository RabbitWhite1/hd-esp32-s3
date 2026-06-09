// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#include "ST7305_U8g2.h"
#include "i2c_bsp.h"
#include "codec_bsp.h"  // CodecPort
#include "logging.h"    // logDebug / logInfo / logWarn / logError
#include "wifi_net.h"   // wifiBegin / wifiConnected / wifiIP / wifiSSID
#include "time_sync.h"  // timeBegin / timeFormatDateTime
#include "weather.h"    // City, cities[], weatherUpdateAll
#include "sensors.h"    // sensorsBegin / sensorsPresent / sensorsRead
#include "web_ui.h"     // webBegin / webHandle / webTodo*
#include "claude_usage.h"  // claudeUsageUpdate / claudeFiveHour / claudeSevenDay

// ---------- RLCD SPI pins ----------
#define RLCD_SCK_PIN 11
#define RLCD_MOSI_PIN 12
#define RLCD_DC_PIN 5
#define RLCD_CS_PIN 40
#define RLCD_RST_PIN 41

// ---------- KEY button ----------
#define KEY_PIN 18  // active low (confirmed from Waveshare button_bsp)

#define DISP_W 400
#define DISP_H 300
#define SHOW_DIAGNOSTIC true

// One I2C bus shared by the SHTC3 sensor and the audio codec (scl=14, sda=13, port 0)
I2cMasterBus I2cbus(14, 13, 0);
CodecPort *codec = nullptr;

ST7305_U8g2 lcd(RLCD_SCK_PIN, RLCD_MOSI_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN);
U8G2 *u8g2 = nullptr;

const unsigned long SAMPLE_INTERVAL = 1000;
const unsigned long SAMPLE_PRINT_INTERVAL = 60000;  // log temp/humidity once per this span (a multiple of SAMPLE_INTERVAL)
const unsigned long WEATHER_INTERVAL = 10UL * 60 * 1000;
const unsigned long CLAUDE_USAGE_INTERVAL = 10UL * 60 * 1000;  // refresh Claude usage every 10 min
unsigned long lastSample = 0;
unsigned long lastWeather = 0;
unsigned long lastClaudeUsage = 0;
unsigned long sampleCount = 0;

// latest sensor readings cached for redraw
float lastTemp = NAN, lastHum = NAN;
bool sensorOK = false;

// KEY debounce state
int keyPrev = HIGH;
unsigned long keyLastChange = 0;

// ---------- chime ----------
// Generate a short two-note chime as 16-bit stereo PCM and play it.
void playChime() {
  if (!codec) return;
  const int sampleRate = 16000;
  codec->CodecPort_SetInfo("es8311", 1, sampleRate, 2, 16);  // open playback
  codec->CodecPort_SetSpeakerVol(85);

  const int noteFreqs[2] = { 880, 1175 };  // A5 then D6
  const int noteMs = 120;
  for (int n = 0; n < 2; n++) {
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
void drawDiagnostic() {
  u8g2->setFont(u8g2_font_5x7_tf);
  u8g2->drawStr(2, 8, "TL");
  u8g2->drawStr(DISP_W - 12, 8, "TR");
  u8g2->drawStr(2, DISP_H - 2, "BL");
  u8g2->drawStr(DISP_W - 12, DISP_H - 2, "BR");
  for (int y = 50; y < DISP_H; y += 50) u8g2->drawHLine(0, y, 8);  // edge ticks, no labels
  for (int x = 50; x < DISP_W; x += 50) u8g2->drawVLine(x, 0, 8);
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
  if (maxChars > 40) maxChars = 40;
  int ty = y + 28;  // first item's text baseline
  for (int i = 0; i < n && ty <= y + h - 4; i++) {
    int cbx = x + 6, cby = ty - 8;
    u8g2->drawFrame(cbx, cby, 8, 8);
    if (webTodoDone(i)) {
      u8g2->drawLine(cbx, cby, cbx + 7, cby + 7);
      u8g2->drawLine(cbx + 7, cby, cbx, cby + 7);
    }
    char line[44];
    snprintf(line, sizeof(line), "%.*s", maxChars, webTodoText(i));
    u8g2->drawStr(cbx + 12, ty, line);
    if (webTodoDone(i)) u8g2->drawHLine(cbx + 12, ty - 3, (int)strlen(line) * 6);
    ty += 13;
  }
}

// ---------- weather condition icons (~16x16, drawn from top-left x,y) ----------
void drawWxSun(int x, int y) {
  int cx = x + 8, cy = y + 8, r = 3;
  u8g2->drawDisc(cx, cy, r, U8G2_DRAW_ALL);
  for (int i = 0; i < 8; i++) {
    float a = i * 0.7854f;  // every 45 deg
    u8g2->drawLine(cx + (int)((r + 2) * cos(a)), cy + (int)((r + 2) * sin(a)),
                   cx + (int)((r + 4) * cos(a)), cy + (int)((r + 4) * sin(a)));
  }
}
void drawWxCloud(int x, int y) {
  u8g2->drawDisc(x + 4, y + 6, 3, U8G2_DRAW_ALL);
  u8g2->drawDisc(x + 8, y + 4, 4, U8G2_DRAW_ALL);
  u8g2->drawDisc(x + 12, y + 6, 3, U8G2_DRAW_ALL);
  u8g2->drawBox(x + 3, y + 6, 10, 3);
}
void drawWxPartly(int x, int y) {
  int cx = x + 5, cy = y + 5, r = 2;  // small sun peeking from the top-left
  u8g2->drawDisc(cx, cy, r, U8G2_DRAW_ALL);
  for (int i = 0; i < 8; i++) {
    float a = i * 0.7854f;
    u8g2->drawLine(cx + (int)((r + 1) * cos(a)), cy + (int)((r + 1) * sin(a)),
                   cx + (int)((r + 3) * cos(a)), cy + (int)((r + 3) * sin(a)));
  }
  drawWxCloud(x + 2, y + 5);  // cloud in front, lower-right
}
void drawWxRain(int x, int y) {
  drawWxCloud(x + 1, y);
  for (int i = 0; i < 3; i++) {
    int dx = x + 4 + i * 4;
    u8g2->drawLine(dx, y + 11, dx - 1, y + 15);  // slanted drops
  }
}
void drawWxFlake(int cx, int cy) {
  u8g2->drawHLine(cx - 2, cy, 5);
  u8g2->drawVLine(cx, cy - 2, 5);
  u8g2->drawPixel(cx - 1, cy - 1);
  u8g2->drawPixel(cx + 1, cy + 1);
  u8g2->drawPixel(cx - 1, cy + 1);
  u8g2->drawPixel(cx + 1, cy - 1);
}
void drawWxSnow(int x, int y) {
  drawWxCloud(x + 1, y);
  drawWxFlake(x + 5, y + 13);
  drawWxFlake(x + 11, y + 13);
}
void drawWxStorm(int x, int y) {
  drawWxCloud(x + 1, y);
  u8g2->drawLine(x + 9, y + 10, x + 6, y + 13);  // lightning bolt
  u8g2->drawLine(x + 6, y + 13, x + 9, y + 13);
  u8g2->drawLine(x + 9, y + 13, x + 6, y + 15);
}
void drawWxWind(int x, int y) {
  u8g2->drawHLine(x + 2, y + 4, 8);  // three streaks with a small curl at the right end
  u8g2->drawPixel(x + 10, y + 3); u8g2->drawPixel(x + 11, y + 4); u8g2->drawPixel(x + 10, y + 5);
  u8g2->drawHLine(x + 2, y + 8, 11);
  u8g2->drawPixel(x + 13, y + 7); u8g2->drawPixel(x + 14, y + 8); u8g2->drawPixel(x + 13, y + 9);
  u8g2->drawHLine(x + 2, y + 12, 7);
  u8g2->drawPixel(x + 9, y + 11); u8g2->drawPixel(x + 10, y + 12); u8g2->drawPixel(x + 9, y + 13);
}

// Pick a condition icon for a WMO weather code at top-left (x, y). Precipitation
// and storms take priority; an otherwise calm sky that is windy shows the wind icon.
void drawWeatherIcon(int x, int y, int code, float wind) {
  if (code < 0) return;  // unknown -> draw nothing
  const float WINDY_KMH = 30.0f;
  if (code >= 95) {
    drawWxStorm(x, y);  // thunderstorm
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    drawWxSnow(x, y);  // snow / snow showers
  } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    drawWxRain(x, y);  // drizzle / rain / rain showers
  } else if (wind >= WINDY_KMH) {
    drawWxWind(x, y);  // clear/cloudy/fog but strong wind
  } else if (code == 0) {
    drawWxSun(x, y);  // clear
  } else if (code == 1 || code == 2) {
    drawWxPartly(x, y);  // mainly clear / partly cloudy
  } else {
    drawWxCloud(x, y + 4);  // 3 overcast, 45/48 fog (centered in the icon box)
  }
}

// One city's weather as a horizontal temperature gauge shaped like the
// thermometer's stem (a rounded bar): "<city> <low> [==fill==] <high>", with the
// <current> value floating just above the fill tip and a condition icon at the right.
void drawWeatherRow(int x, int y, int w, const City &c) {
  u8g2->setFont(u8g2_font_5x7_tf);
  int base = y + 21;  // baseline for the city/low/high text and the bar's bottom
  u8g2->drawStr(x, base, c.name);

  if (!c.ok) {
    u8g2->drawStr(x + 58, base, "--");
    return;
  }

  char s[8];
  int lowX = x + 58;
  snprintf(s, sizeof(s), "%.0f", c.lo);
  u8g2->drawStr(lowX, base, s);

  // Reserve the right end of the row for the condition icon; the bar is shortened to fit.
  int iconSz = 16;
  int iconX = x + w - iconSz;
  int highX = iconX - 16;  // <high> sits just left of the icon
  snprintf(s, sizeof(s), "%.0f", c.hi);
  u8g2->drawStr(highX, base, s);

  int barH = 7;
  int barX = lowX + 16;
  int barY = base - barH;
  int barW = highX - 4 - barX;
  if (barW < 8) barW = 8;
  u8g2->drawRFrame(barX, barY, barW, barH, barH / 2);

  // Fill from the left edge to where <current> falls between <low> and <high>.
  float span = c.hi - c.lo;
  float frac = (span > 0.01f) ? (c.cur - c.lo) / span : 0.0f;
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  int fillW = (int)(frac * (barW - 2));
  if (fillW > 0) u8g2->drawBox(barX + 1, barY + 1, fillW, barH - 2);

  // <current> centered just above the fill tip, kept left of <high>.
  snprintf(s, sizeof(s), "%.0f", c.cur);
  int tw = (int)strlen(s) * 5;
  int curX = barX + 1 + fillW - tw / 2;
  if (curX < x) curX = x;
  if (curX + tw > highX) curX = highX - tw;
  u8g2->drawStr(curX, barY - 2, s);

  // Condition icon at the right end, vertically centered on the gauge bar.
  drawWeatherIcon(iconX, barY + barH / 2 - iconSz / 2, c.code, c.wind);
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

void drawScreen() {
  u8g2->clearBuffer();
  u8g2->setDrawColor(1);
  char buf[48];
  const int mx = 12;                  // left margin, pulled toward the left edge
  const int lineW = DISP_W - 8 - mx;  // full-width lines run from mx to the to-do box's right edge

  u8g2->setFont(u8g2_font_6x13_tf);
  if (timeFormatDateTime(buf, sizeof(buf))) u8g2->drawStr(mx, 34, buf);
  else u8g2->drawStr(mx, 34, "Syncing time...");
  u8g2->drawHLine(mx, 40, lineW);

  // Temperature + humidity, small, tucked into the upper-left under the date.
  // Both icons are drawn from their left edge, but the thermometer bulb (r=h/6) is
  // narrower than the droplet (r=h/3); offset each so they share one center column.
  const int icoH = 22, icoCx = mx + 14;  // ~80% icon; shares a center column with the droplet
  drawThermometer(icoCx - icoH / 6, 50, icoH);
  u8g2->setFont(u8g2_font_helvB12_tf);
  if (sensorOK && !isnan(lastTemp)) snprintf(buf, sizeof(buf), "%.1f C", lastTemp);
  else snprintf(buf, sizeof(buf), "-- C");
  u8g2->drawStr(mx + 33, 72, buf);

  drawDroplet(icoCx - icoH / 3, 82, icoH);
  u8g2->setFont(u8g2_font_helvB12_tf);
  if (sensorOK && !isnan(lastHum)) snprintf(buf, sizeof(buf), "%.1f %%", lastHum);
  else snprintf(buf, sizeof(buf), "-- %%");
  u8g2->drawStr(mx + 33, 104, buf);

  // To-do box to the right of the temp/humidity column.
  drawTodoBox(190, 48, DISP_W - 190 - 8, 130);

  // Separator under the temp/humidity column — left column only, clear of the to-do box.
  u8g2->drawHLine(mx, 112, 150);

  // Weather: one horizontal temperature-gauge row per city.
  int wy = 118;
  for (int i = 0; i < NUM_CITIES; i++) {
    drawWeatherRow(mx, wy, 150, cities[i]);
    wy += 28;
  }

  // Divider below the main content (weather column + to-do box).
  u8g2->drawHLine(mx, 182, lineW);

  // Claude usage, just below the second full divider.
  u8g2->setFont(u8g2_font_6x12_tf);
  u8g2->drawStr(mx, 197, "Claude usage");
  drawUsageBar(mx, 205, lineW, "5h", claudeFiveHour());
  drawUsageBar(mx, 221, lineW, "7d", claudeSevenDay());

  // Wi-Fi footer pinned to the bottom, with a divider right above it.
  u8g2->drawHLine(mx, 283, lineW);
  u8g2->setFont(u8g2_font_5x7_tf);
  if (wifiConnected()) {
    snprintf(buf, sizeof(buf), "SSID: %s    IP: %s", wifiSSID(), wifiIP().c_str());
    u8g2->drawStr(mx, 294, buf);
  } else u8g2->drawStr(mx, 294, "WiFi: disconnected");

  if (SHOW_DIAGNOSTIC) drawDiagnostic();
  u8g2->sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  lcd.begin(0, U8G2_R1);
  u8g2 = lcd.getU8g2();

  pinMode(KEY_PIN, INPUT_PULLUP);

  // SHTC3 + codec share I2cbus
  sensorsBegin(I2cbus);
  sensorOK = sensorsPresent();
  codec = new CodecPort(I2cbus, "S3_RLCD_4_2");

  drawScreen();
  wifiBegin();
  timeBegin();
  webBegin();  // start the LAN message server once Wi-Fi is up
  weatherUpdateAll();
  lastWeather = millis();
  claudeUsageUpdate();
  lastClaudeUsage = millis();
  drawScreen();
  playChime();  // boot confirmation beep
}

void loop() {
  wifiEnsureConnected();
  webHandle();  // serve any pending HTTP requests (kept out of the sample gate so it stays responsive)

  // KEY button: debounce, chime on press (HIGH->LOW)
  int k = digitalRead(KEY_PIN);
  if (k != keyPrev && millis() - keyLastChange > 40) {
    keyLastChange = millis();
    if (k == LOW) {
      logInfo("KEY pressed -> chime");
      playChime();
    }
    keyPrev = k;
  }

  unsigned long now = millis();
  if (now - lastWeather >= WEATHER_INTERVAL) {
    lastWeather = now;
    weatherUpdateAll();
  }

  if (now - lastClaudeUsage >= CLAUDE_USAGE_INTERVAL) {
    lastClaudeUsage = now;
    claudeUsageUpdate();
  }

  if (now - lastSample >= SAMPLE_INTERVAL) {
    lastSample = now;
    bool gotReading = sensorOK && sensorsRead(&lastTemp, &lastHum);
    // print temp/humidity once every SAMPLE_PRINT_INTERVAL (= every Nth sample)
    if (gotReading && ++sampleCount % (SAMPLE_PRINT_INTERVAL / SAMPLE_INTERVAL) == 0)
      logInfo("Temperature: %.2f C  Humidity: %.2f %%", lastTemp, lastHum);
    drawScreen();
  }
}
