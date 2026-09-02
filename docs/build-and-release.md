# Build, flash, and release

This is a plain Arduino sketch. Use Arduino IDE or `arduino-cli`, not PlatformIO or `idf.py`. The
sketch directory must remain named `hd-esp32-s3` so it matches `hd-esp32-s3.ino`.

## Install the toolchain

Install the `esp32:esp32` board package and follow the
[Waveshare Arduino IDE setup guide](https://docs.waveshare.com/ESP32-Arduino-Tutorials/Arduino-IDE-Setup)
if this is your first ESP32 Arduino project.

The project also needs these Arduino libraries:

| Library | Purpose |
|---------|---------|
| U8g2 | Monochrome graphics and the ST7305 display driver |
| ArduinoJson | Configuration and network response parsing |
| SensorLib | PCF85063 RTC support |

Install them from Arduino IDE's Library Manager or with:

```bash
arduino-cli lib install "U8g2" "ArduinoJson" "SensorLib"
```

Wi-Fi, HTTPS, the web server, SPI, FreeRTOS, and SD/MMC support come with Arduino-ESP32. The codec
components are vendored under `src/ExternLib/`.

## Compile and flash over USB

The target has 16 MB flash. Enable PSRAM and use the 3 MB application partition; the default
partition is too small for this firmware.

```bash
FQBN=esp32:esp32:esp32s3:PSRAM=enabled,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB
arduino-cli compile --fqbn "$FQBN" .
arduino-cli upload --fqbn "$FQBN" -p /dev/ttyACM0 .
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200
```

Adjust the serial port for your machine. The first flash must use USB.

## Flash a local build over Wi-Fi

After firmware containing ArduinoOTA is running:

```bash
./flash.sh
```

This compiles and uploads to `esp32.local`. Useful variants are:

```bash
./flash.sh /dev/ttyACM0       # compile and upload over USB
OTA_HOST=192.0.2.10 ./flash.sh
OTA_PASS=your-password ./flash.sh
```

The OTA password is optional and must match `ota_pass` in the device configuration.

## Continuous integration

`.github/workflows/firmware.yml` builds pushes to `main` and pull requests with ESP32 core 3.3.11,
U8g2 2.36.19, ArduinoJson 7.4.3, and SensorLib 0.4.1. It retains the firmware binary and SHA-256 as
workflow artifacts for 90 days.

A regular build is stamped `main-<short-sha>`; a local build reports `dev`.

## Publish a release

Only a pushed `v*` tag publishes a GitHub Release and makes the image available to the device's web
updater. A normal commit or push never becomes an update candidate.

```bash
git tag v0.1.8
git push origin v0.1.8
```

Tags may include a build date, for example `v0.1.8+20260901`. Git, GitHub Actions, GitHub Releases,
and the device's version picker accept that form.

The release contains `hd-esp32-s3.ino.bin` and its `.sha256`. The device verifies that checksum before
activating an OTA image.
