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
#include "src/web_ui/web_ui.h"               // webBegin / webHandle / webTodo*
#include "src/claude_usage/claude_usage.h"   // claudeUsageUpdate / claudeFiveHour / claudeSevenDay
#include "src/claude_usage/clawd_icon.h"      // clawd_icon_bits — mascot drawn left of the usage gauges

// ---------- RLCD SPI pins ----------
#define RLCD_SCK_PIN 11
#define RLCD_MOSI_PIN 12
#define RLCD_DC_PIN 5
#define RLCD_CS_PIN 40
#define RLCD_RST_PIN 41

// ---------- buttons (active-low; see Waveshare button_bsp) ----------
#define KEY_PIN 18   // "GP18" button
#define BOOT_PIN 0   // BOOT button; also the download strapping pin. Unused for now.

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
  if (timeFormatDateTime(buf, sizeof(buf))) u8g2->drawStr(mx, 24, buf);
  else u8g2->drawStr(mx, 24, "Syncing time...");
  u8g2->drawHLine(mx, 30, lineW);

  // Temperature + humidity, small, tucked into the upper-left under the date.
  // Both icons are drawn from their left edge, but the thermometer bulb (r=h/6) is
  // narrower than the droplet (r=h/3); offset each so they share one center column.
  const int icoH = 22, icoCx = mx + 14;  // ~80% icon; shares a center column with the droplet
  drawThermometer(icoCx - icoH / 6, 40, icoH);
  u8g2->setFont(u8g2_font_helvB12_tf);
  if (sensorOK && !isnan(lastTemp)) snprintf(buf, sizeof(buf), "%.1f C", lastTemp);
  else snprintf(buf, sizeof(buf), "-- C");
  u8g2->drawStr(mx + 33, 62, buf);

  drawDroplet(icoCx - icoH / 3, 72, icoH);
  u8g2->setFont(u8g2_font_helvB12_tf);
  if (sensorOK && !isnan(lastHum)) snprintf(buf, sizeof(buf), "%.1f %%", lastHum);
  else snprintf(buf, sizeof(buf), "-- %%");
  u8g2->drawStr(mx + 33, 94, buf);

  // To-do box to the right of the temp/humidity column.
  drawTodoBox(190, 38, DISP_W - 190 - 8, 130);

  // Separator under the temp/humidity column — left column only, clear of the to-do box.
  u8g2->drawHLine(mx, 102, 150);

  // Weather: one horizontal temperature-gauge row per city.
  int wy = 108;
  for (int i = 0; i < NUM_CITIES; i++) {
    drawWeatherRow(mx, wy, 150, cities[i]);
    wy += 28;
  }

  // Divider below the main content (weather column + to-do box).
  u8g2->drawHLine(mx, 172, lineW);

  // Claude usage, just below the second full divider.
  u8g2->setFont(u8g2_font_6x12_tf);
  time_t cuAsOf = claudeUsageAsOf();
  if (cuAsOf > 0) {
    struct tm cuTm;
    localtime_r(&cuAsOf, &cuTm);
    char asOfStr[24];
    strftime(asOfStr, sizeof(asOfStr), "%Y-%m-%d %H:%M:%S", &cuTm);
    snprintf(buf, sizeof(buf), "Claude Usage (As of %s)", asOfStr);
  } else {
    snprintf(buf, sizeof(buf), "Claude Usage (never)");
  }
  u8g2->drawStr(mx, 187, buf);
  // Mascot on the left; the two gauges are shifted right to make room for it.
  const int clawdGap = 6;
  const int gaugeX = mx + CLAWD_ICON_W + clawdGap;
  const int gaugeW = lineW - (CLAWD_ICON_W + clawdGap);
  u8g2->drawXBMP(mx, 207 - CLAWD_ICON_H / 2, CLAWD_ICON_W, CLAWD_ICON_H, clawd_icon_bits);
  drawUsageBar(gaugeX, 195, gaugeW, "5h", claudeFiveHour());
  drawUsageBar(gaugeX, 211, gaugeW, "7d", claudeSevenDay());

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

  // KEY button: debounce; on press (HIGH->LOW) chime + force-refresh weather/Claude usage
  int k = digitalRead(KEY_PIN);
  if (k != keyPrev && millis() - keyLastChange > 40) {
    keyLastChange = millis();
    if (k == LOW) {
      logInfo("KEY pressed -> chime + refresh");
      playChime();
      weatherUpdateAll();
      claudeUsageUpdate();
      lastWeather = millis();      // reset the periodic timers so the next auto-refresh is a full interval away
      lastClaudeUsage = millis();
      drawScreen();                // show the freshly fetched data
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
