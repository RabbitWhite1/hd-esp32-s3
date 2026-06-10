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
#include "src/gdoc/gdoc.h"                    // gdocUpdate / gdocLineCount / gdocLine — Google Doc notes
#include "src/sdcard/sdcard.h"                // sdBegin / sdFormat / sdReadText / sdWriteText — microSD storage

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

// One I2C bus shared by the SHTC3 sensor and the audio codec (scl=14, sda=13, port 0)
I2cMasterBus I2cbus(14, 13, 0);
CodecPort *codec = nullptr;

ST7305_U8g2 lcd(RLCD_SCK_PIN, RLCD_MOSI_PIN, RLCD_DC_PIN, RLCD_CS_PIN, RLCD_RST_PIN);
U8G2 *u8g2 = nullptr;

const unsigned long SAMPLE_INTERVAL = 10 * 1000;
const unsigned long SAMPLE_PRINT_INTERVAL = 10UL * 60 * 1000;  // log temp/humidity once per this span (a multiple of SAMPLE_INTERVAL)
const unsigned long WEATHER_INTERVAL = 10UL * 60 * 1000;
const unsigned long CLAUDE_USAGE_INTERVAL = 30UL * 60 * 1000;  // refresh Claude usage every 30 min
const unsigned long GDOC_INTERVAL = 4UL * 60 * 60 * 1000;      // refresh the Google Doc notes every 4 hours
unsigned long lastSample = 0;
unsigned long lastWeather = 0;
unsigned long lastClaudeUsage = 0;
unsigned long lastGdoc = 0;
unsigned long sampleCount = 0;

// latest sensor readings cached for redraw
float lastTemp = NAN, lastHum = NAN;
bool sensorOK = false;

// Last Claude org+key pair written to SD, so the loop only rewrites on change.
String lastSavedClaude = "";

// KEY debounce state
int keyPrev = HIGH;
unsigned long keyLastChange = 0;

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

// Render the fetched Google Doc lines inside a framed box, styled like the to-do
// box (titled header + rule, then one clipped line per paragraph).
void drawDocBox(int x, int y, int w, int h) {
  u8g2->drawFrame(x, y, w, h);
  u8g2->setFont(u8g2_font_6x10_tf);
  const char *t = gdocTitle();
  u8g2->drawStr(x + 6, y + 11, (t && t[0]) ? t : "Doc");  // the Google Doc's own title
  u8g2->drawHLine(x + 4, y + 15, w - 8);

  int n = gdocLineCount();
  if (n == 0) {
    u8g2->drawStr(x + 6, y + 28, gdocOk() ? "(empty)" : "(no data)");
    return;
  }
  int maxChars = (w - 12) / 6;  // chars that fit inside the box padding
  if (maxChars > 40) maxChars = 40;
  int ty = y + 28;  // first line's text baseline
  for (int i = 0; i < n && ty <= y + h - 4; i++) {
    char line[44];
    snprintf(line, sizeof(line), "%.*s", maxChars, gdocLine(i));
    u8g2->drawStr(x + 6, ty, line);
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
  char buf[80];
  const int mx = 12;                  // left margin, pulled toward the left edge
  const int lineW = DISP_W - 8 - mx;  // full-width lines run from mx to the to-do box's right edge

  u8g2->setFont(u8g2_font_6x13_tf);
  if (timeFormatDateTime(buf, sizeof(buf))) u8g2->drawStr(mx, 24, buf);
  else u8g2->drawStr(mx, 24, "Syncing time...");
  u8g2->drawHLine(mx, 30, lineW);

  // Temperature + humidity on one row in the upper-left under the date:
  // thermometer + value on the left, droplet + value on the right. Each icon is
  // offset by its own bulb radius so it sits centered over its column.
  const int icoH = 22, iconY = 54;    // ~80% icon, vertically centered in the section
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
  u8g2->drawHLine(mx, 102, 172);

  // Weather: one horizontal temperature-gauge row per city.
  int wy = 108;
  for (int i = 0; i < NUM_CITIES; i++) {
    drawWeatherRow(mx, wy, 172, cities[i]);
    wy += 28;
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

// ---------- Claude credential persistence (one id-key pair on the SD card) ----------
// Stored as "<orgId>\n<sessionKey>"; a new pair overwrites the old one.
bool saveClaudeCreds() {
  return sdWriteText("claude.txt", claudeUsageOrgId() + "\n" + claudeUsageSessionKey());
}
void loadClaudeCreds() {
  String data = sdReadText("claude.txt");
  if (data.length() == 0) return;
  int nl = data.indexOf('\n');
  String org = (nl < 0) ? data : data.substring(0, nl);
  String key = (nl < 0) ? String("") : data.substring(nl + 1);
  org.trim();
  key.trim();
  if (org.length()) claudeUsageSetOrgId(org);
  if (key.length()) claudeUsageSetSessionKey(key);
  logInfo("Claude creds loaded from SD");
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
  if (sensorOK) sensorsRead(&lastTemp, &lastHum);  // populate temp/humidity before the first drawScreen (no Wi-Fi needed)
  codec = new CodecPort(I2cbus, "S3_RLCD_4_2");

  // microSD: mount, then load persisted creds + saved Wi-Fi networks BEFORE
  // connecting, so wifiBegin() can try the saved networks.
  sdBegin();

  // ============================ ONE-TIME SD FORMAT ============================
  // Wipes the card to a fresh FAT filesystem on EVERY boot — here only to prepare
  // a raw/unreadable card. >>> COMMENT OUT the sdFormat() line below <<< once the
  // card is prepared, otherwise the saved Claude key + Wi-Fi list are erased each
  // boot. The two lines after it re-persist what we loaded above so the freshly
  // wiped card isn't left empty; they are harmless to keep (or remove together).
  // sdFormat();
  // ===========================================================================

  loadClaudeCreds();
  wifiLoadNetworks();

  drawScreen();
  wifiSetRedrawHook(drawScreen);  // let wifiBegin() show "Trying <ssid>" on the footer
  wifiBegin();
  timeBegin();
  drawScreen();

  weatherUpdateAll();
  lastWeather = millis();
  drawScreen();

  webBegin();  // start the LAN message server once Wi-Fi is up
  drawScreen();

  claudeUsageUpdate();
  lastClaudeUsage = millis();
  drawScreen();

  gdocUpdate();
  lastGdoc = millis();
  drawScreen();

  playChimeLong();  // boot updates done
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
      playChimeShort();            // immediate press feedback
      weatherUpdateAll();
      claudeUsageUpdate();
      gdocUpdate();
      lastWeather = millis();      // reset the periodic timers so the next auto-refresh is a full interval away
      lastClaudeUsage = millis();
      lastGdoc = millis();
      drawScreen();                // show the freshly fetched data
      playChimeLong();             // updates done
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

  if (now - lastGdoc >= GDOC_INTERVAL) {
    lastGdoc = now;
    gdocUpdate();
  }

  // Persist the Claude org id + session key to SD once provided, rewriting only
  // when it changes. Exactly one pair is kept on the card (new overwrites old).
  if (claudeUsageHasKey()) {
    String cur = claudeUsageOrgId() + "\n" + claudeUsageSessionKey();
    if (cur != lastSavedClaude && saveClaudeCreds()) lastSavedClaude = cur;
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
