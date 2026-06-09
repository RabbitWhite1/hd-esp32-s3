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
const unsigned long SAMPLE_PRINT_INTERVAL = 10000;  // log temp/humidity once per this span (a multiple of SAMPLE_INTERVAL)
const unsigned long WEATHER_INTERVAL = 10UL * 60 * 1000;
unsigned long lastSample = 0;
unsigned long lastWeather = 0;
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
void drawStar(int cx, int cy, int r) {
  float in = r * 0.4f;
  int px[10], py[10];
  for (int i = 0; i < 10; i++) {
    float rad = (i % 2 == 0) ? r : in;
    float a = -1.5708f + i * 0.62832f;
    px[i] = cx + (int)(rad * cos(a));
    py[i] = cy + (int)(rad * sin(a));
  }
  for (int i = 0; i < 10; i++) {
    int j = (i + 1) % 10;
    u8g2->drawLine(px[i], py[i], px[j], py[j]);
  }
}

void drawDiagnostic() {
  int m = 14;
  drawStar(m, m, 11);
  drawStar(DISP_W - m, m, 11);
  drawStar(m, DISP_H - m, 11);
  drawStar(DISP_W - m, DISP_H - m, 11);
  u8g2->setFont(u8g2_font_5x7_tf);
  u8g2->drawStr(m + 12, m + 3, "TL");
  u8g2->drawStr(DISP_W - m - 22, m + 3, "TR");
  u8g2->drawStr(m + 12, DISP_H - m + 3, "BL");
  u8g2->drawStr(DISP_W - m - 22, DISP_H - m + 3, "BR");
  for (int y = 50; y < DISP_H; y += 50) {
    u8g2->drawHLine(0, y, 8);
    char t[8];
    snprintf(t, sizeof(t), "%d", y);
    u8g2->drawStr(10, y + 3, t);
  }
  for (int x = 50; x < DISP_W; x += 50) {
    u8g2->drawVLine(x, 0, 8);
    char t[8];
    snprintf(t, sizeof(t), "%d", x);
    u8g2->drawStr(x - 6, 18, t);
  }
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

void drawScreen() {
  u8g2->clearBuffer();
  u8g2->setDrawColor(1);
  char buf[48];

  u8g2->setFont(u8g2_font_6x13_tf);
  if (timeFormatDateTime(buf, sizeof(buf))) u8g2->drawStr(35, 40, buf);
  else u8g2->drawStr(35, 40, "Syncing time...");
  u8g2->drawHLine(35, 46, DISP_W - 50);

  // Temperature + humidity, half-size, tucked into the upper-left under the date.
  // Both icons are drawn from their left edge, but the thermometer bulb (r=h/6) is
  // narrower than the droplet (r=h/3); offset each so they share one center column.
  const int icoH = 28, icoCx = 49;
  drawThermometer(icoCx - icoH / 6, 54, icoH);
  u8g2->setFont(u8g2_font_logisoso16_tf);
  if (sensorOK && !isnan(lastTemp)) snprintf(buf, sizeof(buf), "%.1f C", lastTemp);
  else snprintf(buf, sizeof(buf), "-- C");
  u8g2->drawStr(70, 74, buf);

  drawDroplet(icoCx - icoH / 3, 86, icoH);
  u8g2->setFont(u8g2_font_logisoso16_tf);
  if (sensorOK && !isnan(lastHum)) snprintf(buf, sizeof(buf), "%.1f %%", lastHum);
  else snprintf(buf, sizeof(buf), "-- %%");
  u8g2->drawStr(70, 106, buf);

  // To-do box to the right of the temp/humidity column.
  drawTodoBox(190, 52, DISP_W - 190 - 8, 124);

  u8g2->drawHLine(35, 200, DISP_W - 50);
  u8g2->setFont(u8g2_font_6x12_tf);
  u8g2->drawStr(35, 215, "Weather (C)");
  u8g2->setFont(u8g2_font_6x10_tf);
  int wy = 232;
  for (int i = 0; i < NUM_CITIES; i++) {
    if (cities[i].ok)
      snprintf(buf, sizeof(buf), "%-10s %.0f  H%.0f L%.0f", cities[i].name, cities[i].cur, cities[i].hi, cities[i].lo);
    else
      snprintf(buf, sizeof(buf), "%-10s  --", cities[i].name);
    u8g2->drawStr(35, wy, buf);
    wy += 16;
  }
  u8g2->setFont(u8g2_font_5x7_tf);
  if (wifiConnected()) {
    snprintf(buf, sizeof(buf), "ssid: %s    IP: %s", wifiSSID(), wifiIP().c_str());
    u8g2->drawStr(35, wy + 2, buf);
  } else u8g2->drawStr(35, wy + 2, "WiFi: disconnected");

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

  if (now - lastSample >= SAMPLE_INTERVAL) {
    lastSample = now;
    bool gotReading = sensorOK && sensorsRead(&lastTemp, &lastHum);
    // print temp/humidity once every SAMPLE_PRINT_INTERVAL (= every Nth sample)
    if (gotReading && ++sampleCount % (SAMPLE_PRINT_INTERVAL / SAMPLE_INTERVAL) == 0)
      logInfo("Temperature: %.2f C  Humidity: %.2f %%", lastTemp, lastHum);
    drawScreen();
  }
}
