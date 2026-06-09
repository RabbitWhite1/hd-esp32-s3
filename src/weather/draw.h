#pragma once
#include "weather.h"  // City

// Weather rendering (the frontend half of the weather feature, split out of the
// sketch). Draws one city as a horizontal temperature gauge with a condition
// icon. Uses the sketch's shared global `U8G2 *u8g2`.
void drawWeatherRow(int x, int y, int w, const City &c);
