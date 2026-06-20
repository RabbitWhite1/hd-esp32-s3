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
- The board has **16 MB flash**, so build with `FlashSize=16M` and a 16 MB partition scheme. Use `PartitionScheme=app3M_fat9M_16MB` (3 MB app partition) — the default scheme only gives ~1.25 MB of app space, which the firmware already nearly fills. With the 3 MB partition the sketch sits around 40%.

Typical commands (adjust the port). Define the FQBN once so compile/upload agree:

```bash
FQBN=esp32:esp32:esp32s3:PSRAM=enabled,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB
arduino-cli compile --fqbn "$FQBN" .
arduino-cli upload  --fqbn "$FQBN" -p /dev/ttyACM0 .
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200   # Serial logs are 115200
```

Required Arduino libraries (installed separately, not vendored): **U8g2**, **ArduinoJson**, **SensorLib** (provides `SensorPCF85063.hpp`). `WiFi`, `WiFiClientSecure`, `HTTPClient`, `SPI` ship with Arduino-ESP32. There is no test suite.

## Architecture

### Project layout

Only `h4d-esp32-s3.ino` lives at the sketch root; every other source file is grouped by feature into its own folder under **`src/`** (Arduino compiles the root plus `src/` **recursively**, and silently ignores any other root subfolder — so module folders *must* live under `src/`):

```
h4d-esp32-s3.ino
src/
  weather/      weather.{h,cpp}   draw.{h,cpp}      # backend fetch + its LCD rendering
  wifi_net/ time_sync/ sensors/ web_ui/ claude_usage/ gdoc/ sdcard/ config/ asset_cache/ logging/   # one folder each
  bsp/          i2c_bsp, i2c_equipment, codec_bsp, ST7305_U8g2
  ExternLib/ Music/   # vendored (see below)
```

Includes use **paths relative to the including file**: the `.ino` includes `"src/<module>/<file>.h"`; a file in `src/foo/` reaches a sibling module with `"../bar/bar.h"` and its own folder with `"file.h"`. (This avoids depending on `-I` flags.) The module table below uses short names — e.g. `weather.*` means `src/weather/weather.{h,cpp}`.

### Frontend / backend split

The sketch (`h4d-esp32-s3.ino`) holds **most frontend**: the `drawScreen` compositor, the `draw*` primitives that remain in the sketch, the audio chime + KEY-button handling (user-facing I/O), and `setup()`/`loop()`. It also owns the shared hardware objects (`I2cbus`, `lcd`, `codec`, and the global `U8G2 *u8g2`). Backend logic lives in dedicated modules and is reached only through their small headers:

| Module | Responsibility | Key API |
|--------|----------------|---------|
| `wifi_net.*` | STA connect/reconnect/status + mDNS (advertises `esp32.local`); holds the hardcoded fallback SSID/password/hostname plus a list of saved networks persisted to `/sdcard/wifi.txt`. `wifiBegin` tries saved networks first, then the fallback; `wifiAddNetwork` stores a new one **only after it verifies the join** | `wifiBegin`, `wifiEnsureConnected`, `wifiConnected`, `wifiSSID`, `wifiIP`, `wifiHostname`, `wifiLoadNetworks`, `wifiSaveNetworks`, `wifiAddNetwork`, `wifiRemoveNetwork`, `wifiMoveNetwork` / `wifiApplyOrder` / `wifiOrderDirty`, `wifiNetCount` / `wifiNetSSID` |
| `time_sync.*` | NTP sync + dual-TZ formatting. Holds a table of selectable zones (POSIX TZ + "ABBR - City" label); the primary/secondary pick is set via the web UI and persisted to `/sdcard/tz.txt` (defaults: LA / New York) | `timeBegin`, `timeFormatDateTime`, `timeZoneCount` / `timeZoneLabel`, `timePrimaryZone` / `timeSecondaryZone`, `timeSetZones`, `timeLoadZones` / `timeSaveZones` |
| `weather.*` | Open-Meteo fetch (defines `City`, `cities[]`). Cities are added **by name** via the web UI: the name is resolved once to lat/lon through Open-Meteo's **geocoding API** (`geocoding-api.open-meteo.com`), and the resolved coordinates are persisted to `/sdcard/cities.txt`. The LCD weather band fits `weatherMaxCities()` (2) rows; defaults seed Sunnyvale/New York when the file is empty | `weatherUpdateAll`, `weatherCityCount` / `weatherMaxCities` / `weatherCityName`, `weatherLoadCities`, `weatherAddCity`, `weatherRemoveCity` |
| `weather/draw.*` | **Frontend** for weather: renders one city as a gauge + condition icon (uses the global `u8g2`). Lives in the weather folder, called from `drawScreen()` | `drawWeatherRow` |
| `sensors.*` | SHTC3 temp/humidity (owns the `Shtc3Port` instance) | `sensorsBegin(I2cMasterBus&)`, `sensorsPresent`, `sensorsRead` |
| `web_ui.*` | LAN HTTP server (owns a `WebServer` on port 80). Styled with **Bootstrap 5** (served offline from `asset_cache`). Forms submit via **`fetch()`** (no full-page reload): each POST handler ends in `respond(ok,msg)`, which returns JSON `{ok,msg}` to AJAX callers (detected via the `X-Requested-With` header) or falls back to a `flash()` + 303 redirect for no-JS; the client shows a **toast** (green/red) and swaps just the `#app` container with freshly fetched content. Has a **left-side sticky navigator** linking to each section. Also serves **`GET /stats`** — a curl-friendly JSON snapshot of date+time, temperature, humidity. Routes: editable to-do checklist shown on the LCD and persisted to `/sdcard/todo.md`, a form that sets the `claude_usage` credentials, and a Wi-Fi panel that adds a network via `wifiAddNetwork` (tested-before-saved), removes one, and reorders priority by **drag-and-drop** (SortableJS, posts to `/wifiorder` → `wifiApplyOrder`) or ↑/↓ buttons, then persists with **Save order** (which lights blue while the order is dirty), two time-zone dropdowns (primary/secondary, persisted via `timeSetZones`/`timeSaveZones`), a Weather-cities panel that adds a city by name via `weatherAddCity` (geocoded) and removes one, and refresh-interval fields for Claude usage + the Google Doc (minutes, persisted to `esp32.conf`) | `webBegin`, `webHandle`, `webTodoCount` / `webTodoText` / `webTodoDone` |
| `claude_usage.*` | Fetches the claude.ai org usage summary over HTTPS. Org id + `sessionKey` are set at **runtime via the web UI** (no secret in code). Endpoint is **authenticated** and may be blocked by Cloudflare/anti-bot from an embedded client. Its auto-refresh interval (minutes, default 30) is web-configurable and persisted in `esp32.conf` | `claudeUsageUpdate`, `claudeUsageOk`, `claudeFiveHour`, `claudeSevenDay`, `claudeUsageSetOrgId` / `claudeUsageSetSessionKey`, `claudeUsageIntervalMin` / `claudeUsageSetIntervalMin` |
| `gdoc.*` | Fetches a **link-shared Google Doc**'s plain-text export over HTTPS (`export?format=txt`, which 307-redirects to `googleusercontent.com` — redirect-following is on), strips the BOM, and caches ASCII lines (blank lines kept; trailing blanks dropped). The doc title is read from the export's `Content-Disposition` filename. No URL is baked in — it is set entirely at **runtime via the web UI** (a normal Docs link is stored as its base `…/d/<id>` form; the `export?format=txt` suffix is appended only at fetch time) and persisted to `/sdcard/gdoc_url.txt` (empty → `gdocUpdate()` no-ops); the doc must stay shared as "anyone with the link". Its auto-refresh interval (minutes, default 240) is web-configurable and persisted in `esp32.conf` | `gdocUpdate`, `gdocOk`, `gdocLineCount`, `gdocLine`, `gdocTitle`, `gdocAsOf`, `gdocSetUrl` / `gdocUrl` / `gdocLoadUrl` / `gdocSaveUrl`, `gdocIntervalMin` / `gdocSetIntervalMin` |
| `asset_cache.*` | Offline cache for the web UI's third-party assets (Bootstrap CSS/JS + SortableJS for drag-reorder). Each is downloaded once from jsDelivr and stored on SD, re-fetched only when **missing or older than a 1-week TTL** *and* online; the TTL gate is skipped entirely when offline, so a cached copy is always served regardless of age. `web_ui` serves the SD copy (streamed in chunks) and links to the CDN only until the cache is first populated | `assetsEnsureFresh`, `assetCount` / `assetAt` / `assetByRoute`, `assetIsCached` |
| `config.*` | A tiny dict-like persistent key/value store backed by a single text file `/sdcard/esp32.conf` (one `key=value` line each). Holds small runtime settings that don't each warrant their own file (currently the gdoc + claude-usage refresh intervals). `configBegin()` loads the store into a small in-RAM table at boot; get/set are in-RAM and only hit the card on `configSave()`. Feature modules own their key names + defaults | `configBegin`, `configGet` / `configGetInt`, `configSet` / `configSetInt`, `configSave` |
| `sdcard.*` | microSD over **SDMMC** (1-bit) mounted at `/sdcard`; whole-file text read/write + explicit reformat. Mounts with `format_if_mount_failed` so a raw card becomes usable; all ops no-op gracefully when no card is present. Backs the persisted files: `claude.txt`, `wifi.txt`, `todo.md`, `gdoc_url.txt`, `tz.txt`, `cities.txt`, `esp32.conf`, and the cached web assets `bootstrap.css` / `bootstrap.js` / `sortable.js`. `sdPath()` exposes the absolute `/sdcard/<name>` path for callers that stream a file directly (e.g. `asset_cache`) | `sdBegin`, `sdMounted`, `sdFormat`, `sdReadText`, `sdWriteText`, `sdPath` |

The sketch passes `I2cbus` into `sensorsBegin()` and constructs the codec itself, so the shared bus stays owned by the frontend while sensor access is encapsulated. When adding logic, keep this rule: data acquisition / networking / hardware reads go in a backend module; `drawScreen()` (the compositor) stays in the `.ino`. Per-feature **rendering** may live in that feature's folder as a `draw.*` file (as `weather/draw.*` does) and be called from `drawScreen()` — it draws via the shared global `U8G2 *u8g2` (declared `extern` in the draw file, defined in the sketch). Networking that produces non-LCD output still belongs in a backend module — e.g. `web_ui.*` serves an HTML page, but the sketch is what reads the to-do items and renders them.

`logging.*` is a cross-cutting utility used by every module and the sketch: `logDebug/logInfo/logWarn/logError` print to `Serial` with a `yymmdd-hhmmss` local-time prefix and a `[LEVEL]` tag, gated by the compile-time `LOG_LEVEL` constant (Python-`logging` style; defaults to `LOG_DEBUG` = everything). **Prefer these over raw `Serial.print*`** — the only direct `Serial` writes should be `Serial.begin()` and the single sink inside `src/logging/logging.cpp`. Timestamps read ~1970 until `timeBegin()` finishes NTP sync.

### Device drivers (BSP layer)

The backend/frontend code sits on top of hand-written **BSP/port classes** that live together in **`src/bsp/`**; the three I2C devices share a single bus by **reference injection**.

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
| microSD (SDMMC, 1-bit) | clk 38, cmd 21, d0 39 | `src/sdcard/sdcard.cpp` (from Waveshare `06_SD_Card`) |

Note the I2C pins are declared in **two places** that must agree: the sketch's `I2cbus(14, 13, 0)` and the codec board profile's `i2c: {sda: 13, scl: 14}`.

## `src/ExternLib` — vendored, do not edit

`src/ExternLib/esp_codec_dev` and `src/ExternLib/codec_board` are upstream Espressif ESP-IDF components copied in for the Arduino build (codec drivers for ES8311/ES7210, TCA9554 expander, etc.). Treat them as third-party. The only project-specific file under here is **`board_cfg.h`**, which hardcodes the `S3_RLCD_4_2` profile. `src/Music/canon.h` is a large generated PCM byte array.

## Hardcoded values to be aware of

These are literals, each living with its owning module: the mDNS hostname (`esp32`) in `src/wifi_net/wifi_net.cpp` (no Wi-Fi network is baked into the firmware — networks come only from the SD card / web UI), the default seed `cities[]` weather list in `src/weather/weather.cpp` (used only when `cities.txt` is empty), the selectable time-zone table (POSIX TZ + label, with the default primary/secondary indices) in `src/time_sync/time_sync.cpp`, the SHTC3 temperature offset in `src/bsp/i2c_equipment.cpp`, the default claude.ai org id in `src/claude_usage/claude_usage.cpp` (the `sessionKey` cookie is **not** in code — it is entered at runtime via the web UI and held in RAM), and the SDMMC pins (clk 38 / cmd 21 / d0 39) in `src/sdcard/sdcard.cpp`. (The Google Doc URL is **not** in code either — set at runtime via the web UI, persisted to `/sdcard/gdoc_url.txt`.)

**One-time SD format:** the end of `setup()` calls `sdFormat()` inside a clearly-marked block that **wipes the card on every boot** — it exists only to prepare a raw card and is meant to be commented out once the card is ready (the two lines after it re-persist the loaded Claude key + Wi-Fi list so the wipe doesn't strand them). Persistent state lives in `/sdcard/claude.txt` (one `org\nkey` pair, rewritten from `loop()` on change), `/sdcard/wifi.txt` (one `ssid\tpass` line per saved network, in priority order), `/sdcard/todo.md` (a markdown checklist — `- [x]`/`- [ ]` per item, saved from the web "Save" and loaded in `webBegin()`), `/sdcard/gdoc_url.txt` (the configured Google Doc URL), `/sdcard/tz.txt` (the two selected time-zone POSIX strings, one per line), `/sdcard/cities.txt` (one `name<TAB>lat<TAB>lon` line per weather city), the cached web assets `/sdcard/bootstrap.css` + `/sdcard/bootstrap.js` + `/sdcard/sortable.js` (refreshed by `asset_cache` with a 1-week TTL), and `/sdcard/esp32.conf` (the shared `key=value` settings store — currently the gdoc + claude refresh intervals). The claude/wifi/gdoc-url/tz/cities files plus `configBegin()` are loaded early in `setup()` before `wifiBegin()`.
