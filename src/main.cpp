// Simple 12-hour NTP clock for the ESP32 Cheap Yellow Display (ESP32-2432S028R).
//
// Hardware setup follows the CYD-Dual-SPI reference project:
//   - Screen on VSPI (TFT_eSPI, pins in platformio.ini)
//   - Touch (XPT2046) on its own HSPI bus
//   - LDR auto-brightness on GPIO 34 -> backlight PWM on GPIO 21
//
// Flicker-free rendering: the time and date are each drawn into an off-screen
// sprite (framebuffer in RAM) and pushed to the panel in a single SPI burst,
// so the display never shows a half-drawn frame.
//
// Timekeeping: the ESP32 system clock is set by SNTP over WiFi and re-synced
// every NTP_SYNC_INTERVAL_MIN minutes. Between syncs the clock free-runs on
// the internal RTC.

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "esp_sntp.h"
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include "config.h"

// HARDWARE DEFINITIONS
// Dual SPI Pinout
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CS   33
#define TOUCH_IRQ  36

// LDR auto-brightness
#define CYD_LDR 34
#define CYD_BL  21
#define MIN_BRIGHTNESS 58    // Never go full black
#define MAX_BRIGHTNESS 255
#define SENSOR_MAX_DARK 170  // CALIBRATE THIS! (Value when sensor is covered)
#define TOUCH_BOOST_MS 10000 // Tap -> full brightness for this long

// OBJECTS
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite timeSpr = TFT_eSprite(&tft);
TFT_eSprite dateSpr = TFT_eSprite(&tft);

// Separate SPI bus instance for the Touch Controller
// (HSPI, because VSPI is used by the screen)
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

// Screen is used in landscape: 320 x 240
constexpr int SCR_W = 320;
constexpr int SCR_H = 240;

constexpr int TIME_SPR_H = 100;
constexpr int TIME_SPR_Y = 52;
constexpr int DATE_SPR_H = 34;
constexpr int DATE_SPR_Y = 172;

// Colours (RGB565)
constexpr uint16_t COL_BG        = 0x0841;  // very dark blue-grey
constexpr uint16_t COL_TIME      = 0xFFFF;  // white
constexpr uint16_t COL_ACCENT    = 0xFD20;  // orange
constexpr uint16_t COL_COLON_OFF = 0x2965;  // dim grey (colon "off" phase)
constexpr uint16_t COL_DATE      = 0xAD75;  // light grey
constexpr uint16_t COL_OK        = 0x2E4B;  // muted green
constexpr uint16_t COL_STALE     = 0xFD20;  // orange
constexpr uint16_t COL_ERR       = 0xE946;  // muted red

static const char *DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                             "Thursday", "Friday", "Saturday"};
static const char *DAYS_SHORT[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *MONTHS[] = {"January", "February", "March", "April",
                               "May", "June", "July", "August",
                               "September", "October", "November", "December"};

volatile time_t lastSyncTime = 0;
float smoothedLight = 0;      // For animation/smoothing
uint32_t boostUntil = 0;      // millis() deadline for touch brightness boost

void onNtpSync(struct timeval *) {
  time(const_cast<time_t *>(&lastSyncTime));
}

void drawTime(const tm &t, bool colonOn) {
  timeSpr.fillSprite(COL_BG);

  int hour12 = t.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  char hh[4], mm[4];
  snprintf(hh, sizeof(hh), "%d", hour12);
  snprintf(mm, sizeof(mm), "%02d", t.tm_min);
  const char *ampm = (t.tm_hour < 12) ? "AM" : "PM";

  timeSpr.setTextFont(8);  // 75px numeric font
  int hhW = timeSpr.textWidth(hh);
  int coW = timeSpr.textWidth(":");
  int mmW = timeSpr.textWidth(mm);
  timeSpr.setTextFont(4);
  int apW = timeSpr.textWidth(ampm);
  const int gap = 10;

  int x = (SCR_W - (hhW + coW + mmW + gap + apW)) / 2;
  int yMid = TIME_SPR_H / 2;

  timeSpr.setTextDatum(ML_DATUM);
  timeSpr.setTextFont(8);
  timeSpr.setTextColor(COL_TIME, COL_BG);
  timeSpr.drawString(hh, x, yMid);
  x += hhW;
  timeSpr.setTextColor(colonOn ? COL_ACCENT : COL_COLON_OFF, COL_BG);
  timeSpr.drawString(":", x, yMid);
  x += coW;
  timeSpr.setTextColor(COL_TIME, COL_BG);
  timeSpr.drawString(mm, x, yMid);
  x += mmW + gap;

  // AM/PM, baseline-aligned with the bottom of the digits
  timeSpr.setTextFont(4);
  timeSpr.setTextDatum(BL_DATUM);
  timeSpr.setTextColor(COL_ACCENT, COL_BG);
  timeSpr.drawString(ampm, x, yMid + 34);

  timeSpr.pushSprite(0, TIME_SPR_Y);
}

void drawDate(const tm &t) {
  dateSpr.fillSprite(COL_BG);
  dateSpr.setFreeFont(&FreeSans12pt7b);
  dateSpr.setTextDatum(MC_DATUM);
  dateSpr.setTextColor(COL_DATE, COL_BG);

  char buf[48];
  snprintf(buf, sizeof(buf), "%s, %s %d, %d",
           DAYS[t.tm_wday], MONTHS[t.tm_mon], t.tm_mday, 1900 + t.tm_year);
  if (dateSpr.textWidth(buf) > SCR_W - 8) {
    snprintf(buf, sizeof(buf), "%s, %s %d, %d",
             DAYS_SHORT[t.tm_wday], MONTHS[t.tm_mon], t.tm_mday, 1900 + t.tm_year);
  }
  dateSpr.drawString(buf, SCR_W / 2, DATE_SPR_H / 2);
  dateSpr.pushSprite(0, DATE_SPR_Y);

  // thin accent rule between time and date
  tft.drawFastHLine(SCR_W / 2 - 60, DATE_SPR_Y - 8, 120, COL_ACCENT);
}

// Small status dot, bottom-right: green = synced, orange = sync overdue,
// red = WiFi down. Drawn directly (tiny, only redrawn on change).
void drawStatusDot() {
  static uint16_t lastCol = 0;
  uint16_t col;
  if (WiFi.status() != WL_CONNECTED) {
    col = COL_ERR;
  } else if (lastSyncTime == 0 ||
             time(nullptr) - lastSyncTime > 2 * NTP_SYNC_INTERVAL_MIN * 60) {
    col = COL_STALE;
  } else {
    col = COL_OK;
  }
  if (col != lastCol) {
    tft.fillCircle(SCR_W - 12, SCR_H - 12, 4, col);
    lastCol = col;
  }
}

// LDR auto-brightness (see "CYD Display Dimming" notes).
// The LDR sits next to the screen and the backlight bleeds into it, so the
// usable range is only 0..SENSOR_MAX_DARK, not 0..4095.
void updateBrightness() {
  if (millis() < boostUntil) {
    analogWrite(CYD_BL, MAX_BRIGHTNESS);
    return;
  }
  int raw = analogRead(CYD_LDR);
  // Smoothing Hysteresis: 95% old value, 5% new value = smooth transition
  smoothedLight = (smoothedLight * 0.95f) + ((float)raw * 0.05f);
  int cleanReading = constrain((int)smoothedLight, 0, SENSOR_MAX_DARK);
  // Map & Invert: 0 (bright room) -> 255, SENSOR_MAX_DARK (dark) -> 20
  int target = map(cleanReading, 0, SENSOR_MAX_DARK, MAX_BRIGHTNESS, MIN_BRIGHTNESS);
  analogWrite(CYD_BL, target);
}

void showMessage(const char *msg) {
  tft.fillScreen(COL_BG);
  tft.setTextFont(4);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_DATE, COL_BG);
  tft.drawString(msg, SCR_W / 2, SCR_H / 2);
}

void setup() {
  Serial.begin(115200);

  // Start the second SPI bus for touch
  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  if (!touch.begin(touchSPI)) {
    Serial.println("Touch Controller failed to start!");
  } else {
    Serial.println("Touch Controller Started.");
  }
  touch.setRotation(1);  // Landscape

  // LDR / backlight
  pinMode(CYD_LDR, INPUT);
  pinMode(CYD_BL, OUTPUT);
  analogWrite(CYD_BL, MAX_BRIGHTNESS);  // Start on

  // Start screen
  tft.init();
  tft.setRotation(1);  // landscape, USB on the right
  tft.fillScreen(COL_BG);

  timeSpr.createSprite(SCR_W, TIME_SPR_H);
  dateSpr.createSprite(SCR_W, DATE_SPR_H);

  showMessage("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }

  // SNTP: sync now and every NTP_SYNC_INTERVAL_MIN minutes thereafter
  sntp_set_sync_interval(NTP_SYNC_INTERVAL_MIN * 60 * 1000UL);
  sntp_set_time_sync_notification_cb(onNtpSync);
  configTzTime(TZ_INFO, NTP_SERVER1);

  showMessage("Waiting for time sync...");
}

void loop() {
  static int lastSec = -1;
  static int lastYday = -1;

  // Tap anywhere -> full brightness for a while
  if (touch.touched()) {
    boostUntil = millis() + TOUCH_BOOST_MS;
  }
  updateBrightness();

  time_t now = time(nullptr);
  tm t;
  localtime_r(&now, &t);

  // Until the first NTP sync the clock reads 1970 — keep the wait screen up.
  if (t.tm_year < 100) {
    delay(50);
    return;
  }

  if (t.tm_sec != lastSec) {
    lastSec = t.tm_sec;
    if (lastYday == -1) tft.fillScreen(COL_BG);  // clear the wait message once
    if (t.tm_yday != lastYday) {
      lastYday = t.tm_yday;
      drawDate(t);
    }
    drawTime(t, t.tm_sec % 2 == 0);  // colon blinks once per second
    drawStatusDot();
  }
  delay(50);
}