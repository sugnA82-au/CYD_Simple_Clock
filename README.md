# CYD NTP Clock

A simple, flicker-free 12-hour clock with date for the ESP32 Cheap Yellow
Display (ESP32-2432S028R, 2.8" ILI9341). Syncs with NTP over WiFi every
15 minutes; handles DST automatically via the POSIX timezone string.

## Setup

1. Copy `include/config.h.example` to `include/config.h` and set your
   WiFi SSID/password and timezone.
2. Build and flash:

   ```sh
   pio run -t upload
   ```

## Hardware setup

Matches the CYD-Dual-SPI reference project: screen on VSPI via TFT_eSPI,
XPT2046 touch on its own HSPI bus (CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36),
LDR on GPIO 34 driving backlight PWM on GPIO 21.

- The backlight auto-dims with ambient light (LDR range constrained to
  0-170 because backlight bleed means it never reads full dark; adjust
  `SENSOR_MAX_DARK` in `main.cpp` to calibrate).
- Tap the screen for full brightness for 10 seconds.

## Notes

- Rendering uses TFT_eSPI sprites pushed in one SPI transfer, so digit
  refreshes never flicker. The colon blinks once per second.
- The dot in the bottom-right shows sync health: green = synced,
  orange = sync overdue, red = WiFi disconnected.
- Some CYD variants ship with a regular ILI9341 panel. If colours look
  inverted/wrong, change `ILI9341_2_DRIVER` to `ILI9341_DRIVER` (and/or
  flip `TFT_RGB_ORDER`) in `platformio.ini`.
