#include "draw.h"
#include <Arduino.h>
#include <U8g2lib.h>
#include <math.h>
#include <string.h>

extern U8G2 *u8g2;  // owned by the sketch (h4d-esp32-s3.ino)

// ---------- weather condition icons (~16x16, drawn from top-left x,y) ----------
static void drawWxSun(int x, int y) {
  int cx = x + 8, cy = y + 8, r = 3;
  u8g2->drawDisc(cx, cy, r, U8G2_DRAW_ALL);
  for (int i = 0; i < 8; i++) {
    float a = i * 0.7854f;  // every 45 deg
    u8g2->drawLine(cx + (int)((r + 2) * cos(a)), cy + (int)((r + 2) * sin(a)),
                   cx + (int)((r + 4) * cos(a)), cy + (int)((r + 4) * sin(a)));
  }
}
static void drawWxCloud(int x, int y) {
  u8g2->drawDisc(x + 4, y + 6, 3, U8G2_DRAW_ALL);
  u8g2->drawDisc(x + 8, y + 4, 4, U8G2_DRAW_ALL);
  u8g2->drawDisc(x + 12, y + 6, 3, U8G2_DRAW_ALL);
  u8g2->drawBox(x + 3, y + 6, 10, 3);
}
static void drawWxPartly(int x, int y) {
  int cx = x + 5, cy = y + 5, r = 2;  // small sun peeking from the top-left
  u8g2->drawDisc(cx, cy, r, U8G2_DRAW_ALL);
  for (int i = 0; i < 8; i++) {
    float a = i * 0.7854f;
    u8g2->drawLine(cx + (int)((r + 1) * cos(a)), cy + (int)((r + 1) * sin(a)),
                   cx + (int)((r + 3) * cos(a)), cy + (int)((r + 3) * sin(a)));
  }
  drawWxCloud(x + 2, y + 5);  // cloud in front, lower-right
}
static void drawWxRain(int x, int y) {
  drawWxCloud(x + 1, y);
  for (int i = 0; i < 3; i++) {
    int dx = x + 4 + i * 4;
    u8g2->drawLine(dx, y + 11, dx - 1, y + 15);  // slanted drops
  }
}
static void drawWxFlake(int cx, int cy) {
  u8g2->drawHLine(cx - 2, cy, 5);
  u8g2->drawVLine(cx, cy - 2, 5);
  u8g2->drawPixel(cx - 1, cy - 1);
  u8g2->drawPixel(cx + 1, cy + 1);
  u8g2->drawPixel(cx - 1, cy + 1);
  u8g2->drawPixel(cx + 1, cy - 1);
}
static void drawWxSnow(int x, int y) {
  drawWxCloud(x + 1, y);
  drawWxFlake(x + 5, y + 13);
  drawWxFlake(x + 11, y + 13);
}
static void drawWxStorm(int x, int y) {
  drawWxCloud(x + 1, y);
  u8g2->drawLine(x + 9, y + 10, x + 6, y + 13);  // lightning bolt
  u8g2->drawLine(x + 6, y + 13, x + 9, y + 13);
  u8g2->drawLine(x + 9, y + 13, x + 6, y + 15);
}
static void drawWxWind(int x, int y) {
  u8g2->drawHLine(x + 2, y + 4, 8);  // three streaks with a small curl at the right end
  u8g2->drawPixel(x + 10, y + 3); u8g2->drawPixel(x + 11, y + 4); u8g2->drawPixel(x + 10, y + 5);
  u8g2->drawHLine(x + 2, y + 8, 11);
  u8g2->drawPixel(x + 13, y + 7); u8g2->drawPixel(x + 14, y + 8); u8g2->drawPixel(x + 13, y + 9);
  u8g2->drawHLine(x + 2, y + 12, 7);
  u8g2->drawPixel(x + 9, y + 11); u8g2->drawPixel(x + 10, y + 12); u8g2->drawPixel(x + 9, y + 13);
}

// Pick a condition icon for a WMO weather code at top-left (x, y). Precipitation
// and storms take priority; an otherwise calm sky that is windy shows the wind icon.
static void drawWeatherIcon(int x, int y, int code, float wind) {
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
  int base = y + 21;  // baseline for the city/low/high text and the bar's bottom
  u8g2->setFont(u8g2_font_6x10_tf);  // city name a bit larger than the values
  u8g2->drawStr(x, base, c.name);
  u8g2->setFont(u8g2_font_5x7_tf);   // low/high/current values + bar stay small

  if (!c.ok) {
    u8g2->drawStr(x + 68, base, "--");
    return;
  }

  char s[8];
  int lowX = x + 68;  // extra gap between the city name and the gauge
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
