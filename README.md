# h4d-esp32-s3

Arduino (Arduino-ESP32) firmware for an **ESP32-S3** desktop weather/clock device. It reads
temperature/humidity from an SHTC3 sensor, fetches weather from Open-Meteo over HTTPS, renders to a
300×400 ST7305 reflective monochrome LCD via U8g2, and plays a chime through an ES8311 codec. A LAN
web UI (port 80) exposes settings and history charts.

## Required Arduino libraries

These are **not** vendored in the repo and must be installed through the Arduino IDE Library Manager
(or `arduino-cli lib install`). Everything else used by the sketch either ships with the
**Arduino-ESP32** core (`WiFi`, `WiFiClientSecure`, `HTTPClient`, `WebServer`, `ESPmDNS`, `SPI`,
`driver/*`, `esp_*`, FreeRTOS, SD/MMC) or is vendored under `src/ExternLib/` (`esp_codec_dev`,
`codec_board`).

| Library | Provides / used for | Header |
|---------|--------------------|--------|
| **U8g2** | Monochrome graphics + the custom ST7305 panel driver | `U8g2lib.h` |
| **ArduinoJson** | Parsing Open-Meteo / claude.ai / weather JSON responses | `ArduinoJson.h` |
| **SensorLib** | PCF85063 RTC support | `SensorPCF85063.hpp` |

Install via CLI:

```bash
arduino-cli lib install "U8g2" "ArduinoJson" "SensorLib"
```

Or in the Arduino IDE: **Tools → Manage Libraries…**, then search for and install each of the three above.

You also need the **ESP32 board package** (`esp32:esp32`) installed via the Boards Manager.

## Build / flash

Plain Arduino sketch — build with `arduino-cli` or the Arduino IDE (not PlatformIO/idf.py). The
sketch folder name must match the `.ino`: `h4d-esp32-s3.ino` inside `h4d-esp32-s3/`.

Target board is ESP32-S3. **PSRAM must be enabled** (the codec echo task allocates from SPIRAM). The
board has 16 MB flash, so use a 16 MB partition scheme with a 3 MB app partition — the default scheme
only gives ~1.25 MB of app space, which the firmware nearly fills.

```bash
FQBN=esp32:esp32:esp32s3:PSRAM=enabled,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB
arduino-cli compile --fqbn "$FQBN" .
arduino-cli upload  --fqbn "$FQBN" -p /dev/ttyACM0 .
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200   # Serial logs are 115200
```

See `CLAUDE.md` for architecture, the module/BSP layout, the pin map, and the list of persisted
SD-card files.
