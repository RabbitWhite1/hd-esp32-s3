// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Zhanghan Wang

#pragma once
#include "weather.h"  // City

// Weather rendering (the frontend half of the weather feature, split out of the
// sketch). Draws one city as a horizontal temperature gauge with a condition
// icon. Uses the sketch's shared global `U8G2 *u8g2`.
//
// `nameW` is the pixel width reserved for the city-name column, shared across the
// rows so every gauge starts at the same x (compute it once with
// weatherNameColWidth). The gauge then fills whatever space is left.
void drawWeatherRow(int x, int y, int w, const City &c, int nameW);

// Width (px) for the name column = the longest name among the `count` cities,
// each capped at the name-length limit. Pass the shown cities so gauges align.
int weatherNameColWidth(const City *list, int count);
