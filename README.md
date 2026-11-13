# ATC v2 — CrowPanel Advance 7.0 PlatformIO migration

## Hardware assumptions
- CrowPanel Advance 7.0 HMI, ESP32-S3
- 16 MB flash / 8 MB octal PSRAM (N16R8)
- RGB panel and GT911 touch wiring retained from the supplied sketch
- CrowPanel Advance V1.5 controller I2C startup sequence retained

## First setup
1. Install Visual Studio Code.
2. Install the PlatformIO IDE extension.
3. Open this folder in VS Code (the folder containing `platformio.ini`).
4. Copy `include/secrets.h.example` to `include/secrets.h`.
5. Edit `include/secrets.h` with your Wi-Fi SSID and password.
6. Connect the CrowPanel by USB.
7. Use PlatformIO: Build, then PlatformIO: Upload.
8. Open PlatformIO: Monitor at 115200 baud.

## Important migration notes
- The supplied sketch uses LovyanGFX for drawing. It does **not** currently use LVGL.
- The original sketch has been kept as one `src/main.cpp` file to minimize migration risk.
- No display, radar, API, timing, or drawing logic has intentionally been refactored.
- ArduinoJson 6 is pinned because the sketch uses the ArduinoJson 6 `StaticJsonDocument`
  and `DynamicJsonDocument` APIs.
- The board definition is a starting configuration for the ESP32-S3 N16R8 module.
  Verify the flash/PSRAM settings against the exact CrowPanel hardware revision and
  the Arduino IDE settings that successfully built your original sketch.

## Security
A Wi-Fi password was present in the uploaded sketch. It has been removed from
`main.cpp` and replaced by a local `include/secrets.h` file, which is ignored by Git.
If that was a real active password, change it on your router/access point.

## If build errors occur
Save the complete PlatformIO build output and share it. Do not change library versions
or board memory settings at random; resolve the first compiler error first.
