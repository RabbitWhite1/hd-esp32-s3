# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An Arduino (Arduino-ESP32 framework) firmware for an **ESP32-S3** desktop weather/clock device.
`setup()`/`loop()` live in `h4d-esp32-s3.ino`. The device:

- reads temperature/humidity from an **SHTC3** sensor over I2C,
- fetches current+daily weather for a hardcoded list of `cities[]` from `api.open-meteo.com` over HTTPS,
- renders everything to a **300×400 ST7305 reflective monochrome LCD** via U8g2,
- plays a synthesized chime through an **ES8311** codec on boot and on KEY-button press.

There is no README or build-config file in the repo; the facts below come from reading the source.

## Build / flash

This is a plain Arduino sketch — build with `arduino-cli` or the Arduino IDE, not PlatformIO/idf.py.

- **The sketch folder name must match the `.ino` name.** The file on disk is `h4d-esp32-s3.ino` inside the `h4d-esp32-s3/` folder. (The initial git commit still tracks the old name `h4d.ino`; the working tree has been renamed.)
- Target board: ESP32-S3. **PSRAM must be enabled** — the codec echo task allocates with `MALLOC_CAP_SPIRAM`.

Typical commands (adjust the port):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=enabled .
arduino-cli upload  --fqbn esp32:esp32:esp32s3:PSRAM=enabled -p /dev/ttyACM0 .
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200   # Serial logs are 115200
```

Required Arduino libraries (installed separately, not vendored): **U8g2**, **ArduinoJson**, **SensorLib** (provides `SensorPCF85063.hpp`). `WiFi`, `WiFiClientSecure`, `HTTPClient`, `SPI` ship with Arduino-ESP32. There is no test suite.

## Architecture

### Frontend / backend split

The sketch (`h4d-esp32-s3.ino`) holds **frontend only**: all U8g2 drawing (`drawScreen` and the `draw*` primitives), the audio chime + KEY-button handling (user-facing I/O), and `setup()`/`loop()`. It also owns the shared hardware objects (`I2cbus`, `lcd`, `codec`). Backend logic lives in dedicated modules and is reached only through their small headers:

| Module | Responsibility | Key API |
|--------|----------------|---------|
| `wifi_net.*` | STA connect/reconnect/status (holds SSID/password) | `wifiBegin`, `wifiEnsureConnected`, `wifiConnected`, `wifiIP` |
| `time_sync.*` | NTP sync + dual-TZ formatting (holds TZ strings) | `timeBegin`, `timeFormatDateTime` |
| `weather.*` | Open-Meteo fetch (defines `City`, `cities[]`, `NUM_CITIES`) | `weatherUpdateAll` |
| `sensors.*` | SHTC3 temp/humidity (owns the `Shtc3Port` instance) | `sensorsBegin(I2cMasterBus&)`, `sensorsPresent`, `sensorsRead` |

The sketch passes `I2cbus` into `sensorsBegin()` and constructs the codec itself, so the shared bus stays owned by the frontend while sensor access is encapsulated. When adding logic, keep this rule: data acquisition / networking / hardware reads go in a backend module; anything that draws or produces user-facing output stays in the `.ino`.

`logging.*` is a cross-cutting utility used by every module and the sketch: `logDebug/logInfo/logWarn/logError` print to `Serial` with a `yymmdd-hhmmss` local-time prefix and a `[LEVEL]` tag, gated by the compile-time `LOG_LEVEL` constant (Python-`logging` style; defaults to `LOG_DEBUG` = everything). **Prefer these over raw `Serial.print*`** — the only direct `Serial` writes should be `Serial.begin()` and the single sink inside `logging.cpp`. Timestamps read ~1970 until `timeBegin()` finishes NTP sync.

### Device drivers (BSP layer)

The backend/frontend code sits on top of three hand-written **BSP/port classes**, all sharing a single I2C bus by **reference injection**.

- **`I2cMasterBus` (`i2c_bsp.*`)** — thin wrapper over ESP-IDF `driver/i2c_master.h`. One instance is created in the sketch (`I2cbus(scl=14, sda=13, port 0)`) and passed by `&` into every device class. Convention: a `reg == -1` argument means "no register byte" → raw `i2c_master_transmit`/`receive`; otherwise the register is prepended.
- **`Shtc3Port` (`i2c_equipment.*`)** — SHTC3 temp/humidity driver with CRC checking. Note `SHTC3_PETP_VOL = 4` is **subtracted from the temperature** to compensate for self-heating. The same file also has free functions `Rtc_Setup/SetTime/GetTime` for a **PCF85063 RTC** (via SensorLib, bridged to `I2cMasterBus` through a C callback and file-static singletons). The RTC code is present but not currently called from the sketch.
- **`CodecPort` (`codec_bsp.*`)** — audio over `esp_codec_dev` + `codec_board`. Drives **ES8311** (DAC/speaker, addr 0x18) and **ES7210** (ADC/mic, addr 0x40). The constructor takes a board-profile string (`"S3_RLCD_4_2"`) that selects the I2S/PA pinout baked into `src/ExternLib/codec_board/board_cfg.h`. Provides `CodecPort_CreateMusicTask` (loops the PCM in `src/Music/canon.h`) and `CodecPort_CreateEchoTask` (mic→speaker loopback); the sketch itself uses neither and instead synthesizes a chime inline in `playChime()`.
- **`ST7305_U8g2` (`ST7305_U8g2.*`)** — a custom U8g2 device+byte callback driver for the ST7305 panel, talking SPI directly (not through U8g2's HAL). Key points:
  - Panel is natively **300×400**; the sketch calls `lcd.begin(0, U8G2_R1)`, so logical coordinates are **400×300** (`DISP_W=400`, `DISP_H=300`).
  - `tile_buf_height = 0` selects **full-buffer mode** (~15 KB).
  - U8g2's C callbacks can't capture `this`, so the driver routes through a `static g_lcd_instance` singleton.
  - `U8X8_MSG_DISPLAY_DRAW_TILE` does the non-trivial work: repacking U8g2's 8-px vertical tiles into the ST7305's **3-pixels-per-column-address** layout via the `st_lut` lookup table and column-address arithmetic around base `0x12`.

### Pin map (three separate buses)

| Bus | Pins | Source |
|-----|------|--------|
| Display SPI | sck 11, mosi 12, dc 5, cs 40, rst 41 | `#define`s in the `.ino` |
| I2C (SHTC3 + codec ctrl) | scl 14, sda 13, port 0 | `I2cbus(...)` in the `.ino` |
| Codec I2S + PA | mclk 16, bclk 9, ws 45, din 10, dout 8, PA 46 | `board_cfg.h` (`S3_RLCD_4_2`) |
| KEY button | 18 (active-low, `INPUT_PULLUP`) | `#define` in the `.ino` |

Note the I2C pins are declared in **two places** that must agree: the sketch's `I2cbus(14, 13, 0)` and the codec board profile's `i2c: {sda: 13, scl: 14}`.

## `src/ExternLib` — vendored, do not edit

`src/ExternLib/esp_codec_dev` and `src/ExternLib/codec_board` are upstream Espressif ESP-IDF components copied in for the Arduino build (codec drivers for ES8311/ES7210, TCA9554 expander, etc.). Treat them as third-party. The only project-specific file under here is **`board_cfg.h`**, which hardcodes the `S3_RLCD_4_2` profile. `src/Music/canon.h` is a large generated PCM byte array.

## Hardcoded values to be aware of

These are literals, each living with its owning module: WiFi SSID/password in `wifi_net.cpp`, the `cities[]` weather list in `weather.cpp`, the two timezone strings (`TZ_PACIFIC`/`TZ_EASTERN`) in `time_sync.cpp`, and the SHTC3 temperature offset in `i2c_equipment.cpp`. `SHOW_DIAGNOSTIC` (in the `.ino`) toggles an on-screen calibration overlay (corner stars + axis ticks).
