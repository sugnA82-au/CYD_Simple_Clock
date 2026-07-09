// Simple 12-hour NTP clock + pomodoro timer for the ESP32 Cheap Yellow
// Display (ESP32-2432S028R).
//
// Hardware setup follows the CYD-Dual-SPI reference project:
//   - Screen on VSPI (TFT_eSPI, pins in platformio.ini)
//   - Touch (XPT2046) on its own HSPI bus
//   - LDR auto-brightness on GPIO 34 -> backlight PWM on GPIO 21
//
// Pages: tap the top-left corner of the screen to cycle through the clock,
// the weather page and the pomodoro timer. The pomodoro keeps running while
// other pages are shown and pulls the display back to itself when a session
// ends. The weather page (Open-Meteo, no API key) refreshes piggybacked on
// each NTP sync so the device only wakes the network once per interval.
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
#include <HTTPClient.h>
#include <ArduinoJson.h>
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
constexpr uint16_t COL_REST      = 0x3E8B;  // green (pomodoro break)

static const char *DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                             "Thursday", "Friday", "Saturday"};
static const char *DAYS_SHORT[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *MONTHS[] = {"January", "February", "March", "April",
                               "May", "June", "July", "August",
                               "September", "October", "November", "December"};

volatile time_t lastSyncTime = 0;
float smoothedLight = 0;      // For animation/smoothing
uint32_t boostUntil = 0;      // millis() deadline for touch brightness boost

// PAGES (top-left corner tap cycles CLOCK -> WEATHER -> POMO -> CLOCK)
enum Page { PAGE_CLOCK, PAGE_WEATHER, PAGE_POMO };
Page page = PAGE_CLOCK;

// Weather fetch request flag; set by the NTP sync callback so the weather
// refresh rides the same 15-minute network cadence, and by a tap on the
// weather page for a manual refresh (rate-limited in weatherService()).
volatile bool wxFetchPending = false;

// Clock page redraw state (reset on page switch to force a full redraw)
int clockLastSec = -1;
int clockLastYday = -1;
uint16_t dotLastCol = 0;

// POMODORO
enum PomoMode { POMO_WORK, POMO_REST };
enum PomoState { POMO_IDLE, POMO_RUN, POMO_PAUSE };
constexpr int WORK_OPTIONS[] = {15, 25, 45, 60};  // minutes
constexpr int NUM_WORK_OPTIONS = 4;
constexpr int REST_MIN = 5;

PomoMode pomoMode = POMO_WORK;
PomoState pomoState = POMO_IDLE;
int workIdx = 1;  // default 25 min
uint32_t pomoRemainMs = 25 * 60000UL;
uint32_t pomoLastTick = 0;
int pomoLastShownSec = -1;

// Pomodoro page layout
constexpr int POMO_TIMER_Y = 38;             // countdown sprite (reuses timeSpr)
constexpr int CHIP_Y = 150, CHIP_H = 36;     // duration chips row
constexpr int CHIP_W = 68, CHIP_GAP = 8, CHIP_X0 = 16;
constexpr int BTN_Y = 196, BTN_H = 40;       // start/reset buttons
constexpr int BTN_START_X = 16, BTN_RESET_X = 164, BTN_W = 140;

// Countdown grow/shrink animation: while a work/break cycle is running the
// duration chips are hidden and the digits scale up to fill their space.
constexpr int ANIM_Y0 = POMO_TIMER_Y;                          // region top
constexpr int ANIM_Y1 = BTN_Y - 6;                             // region bottom
constexpr int ANIM_CY_SMALL = POMO_TIMER_Y + TIME_SPR_H / 2;   // digits centre, chips shown
constexpr int ANIM_CY_BIG = (ANIM_Y0 + ANIM_Y1) / 2;           // digits centre, chips hidden

bool chipsHidden = false;
int pomoSrcW = 200;        // pixel width of the rendered countdown string
float pomoScaleMax = 1.2f; // fill scale for the current string width

void onNtpSync(struct timeval *) {
  time(const_cast<time_t *>(&lastSyncTime));
  wxFetchPending = true;  // refresh weather on the same 15 min beat
  Serial.println("NTP sync OK");
}

// ---------------------------------------------------------------- clock page

// Clock digits are pushed through the sprite scaler (below) so they render a
// bit larger than font 8's native 75px. The scale is the largest uniform
// factor that still fits the widest possible string ("12:59 PM") on screen.
constexpr int CLOCK_CY = TIME_SPR_Y + TIME_SPR_H / 2;      // digits centre
constexpr int CLOCK_Y0 = TIME_SPR_Y - 10;                  // scaled region top
constexpr int CLOCK_Y1 = TIME_SPR_Y + TIME_SPR_H + 10;     // scaled region bottom
float clockScale = 0;  // computed on first draw (needs font metrics)

void pushTimeSprScaled(float scale, int centerY, int y0, int y1);

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

  if (clockScale == 0) {
    timeSpr.setTextFont(8);
    int worstW = timeSpr.textWidth("12:59") + gap;
    timeSpr.setTextFont(4);
    worstW += timeSpr.textWidth("PM");
    clockScale = fminf((float)(SCR_W - 2) / worstW, 1.3f);
  }

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

  pushTimeSprScaled(clockScale, CLOCK_CY, CLOCK_Y0, CLOCK_Y1);
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
  uint16_t col;
  if (WiFi.status() != WL_CONNECTED) {
    col = COL_ERR;
  } else if (lastSyncTime == 0 ||
             time(nullptr) - lastSyncTime > 2 * NTP_SYNC_INTERVAL_MIN * 60) {
    col = COL_STALE;
  } else {
    col = COL_OK;
  }
  if (col != dotLastCol) {
    tft.fillCircle(SCR_W - 12, SCR_H - 12, 4, col);
    dotLastCol = col;
  }
}

// The original clock loop, unchanged; runs only while the clock page is shown.
void clockLoop() {
  time_t now = time(nullptr);
  tm t;
  localtime_r(&now, &t);

  // Until the first NTP sync the clock reads 1970 — keep the wait screen up,
  // with a live status line so WiFi/NTP trouble is visible on-device.
  if (t.tm_year < 100) {
    static uint32_t lastStatusMs = 0;
    if (millis() - lastStatusMs >= 1000) {
      lastStatusMs = millis();
      char line[64];
      if (WiFi.status() == WL_CONNECTED) {
        snprintf(line, sizeof(line), "WiFi OK  %s   waiting %lus",
                 WiFi.localIP().toString().c_str(), millis() / 1000UL);
      } else {
        snprintf(line, sizeof(line), "WiFi NOT connected (status %d)",
                 (int)WiFi.status());
      }
      tft.fillRect(0, SCR_H / 2 + 24, SCR_W, 20, COL_BG);
      tft.setTextFont(2);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(COL_DATE, COL_BG);
      tft.drawString(line, SCR_W / 2, SCR_H / 2 + 34);
    }
    return;
  }

  if (t.tm_sec != clockLastSec) {
    clockLastSec = t.tm_sec;
    if (clockLastYday == -1) tft.fillScreen(COL_BG);  // clear the wait message once
    if (t.tm_yday != clockLastYday) {
      clockLastYday = t.tm_yday;
      drawDate(t);
    }
    drawTime(t, t.tm_sec % 2 == 0);  // colon blinks once per second
    drawStatusDot();
  }
}

// -------------------------------------------------------------- weather page
//
// Data comes from Open-Meteo (free, no API key) over plain HTTP for
// Kootingal NSW 2352. One request returns the current conditions, a 12-hour
// rain-probability window and a 4-day daily forecast (~2 KB of JSON).
// Icons are drawn from graphics primitives so they scale between the hero
// (64 px) and the forecast strip (26 px).

constexpr float WX_LAT = -31.06f;
constexpr float WX_LON = 151.05f;
static const char *WX_NAME = "KOOTINGAL";

// Extra colours for the weather art
constexpr uint16_t COL_SUN      = 0xFEA0;  // warm yellow
constexpr uint16_t COL_MOON     = 0xCE9C;  // pale silver
constexpr uint16_t COL_CLOUD    = 0xB5F9;  // light grey-blue cloud
constexpr uint16_t COL_CLOUD_DK = 0x5B2E;  // dark storm cloud
constexpr uint16_t COL_RAIN     = 0x3D1F;  // rain blue
constexpr uint16_t COL_SNOWF    = 0xE73C;  // snowflake white
constexpr uint16_t COL_BOLT     = 0xFFE0;  // lightning yellow
constexpr uint16_t COL_WXTEXT   = 0xE71C;  // bright grey: readable when dimmed

struct WxData {
  bool valid = false;
  time_t fetchedAt = 0;
  float temp = 0, feels = 0, wind = 0, uvMax = 0;
  int hum = 0, code = 0, windDir = 0;
  bool isDay = true;
  float hiT[4], loT[4];        // daily max/min, [0] = today
  int dCode[4], dPop[4];       // daily weather code / max rain probability
  char dayLbl[4][6];           // "TODAY", "FRI", ...
  int rainHour = -1;           // first hour (0-23) with pop >= 40% in next 12 h
  int rainPop = 0;
};
WxData wx;

int wxLastMin = -1;  // header clock minute currently on screen

enum WxIcon { WI_SUN, WI_PARTLY, WI_CLOUD, WI_FOG, WI_DRIZZLE, WI_RAIN,
              WI_SNOW, WI_STORM };

// WMO weather interpretation codes -> icon / text
WxIcon wxIconFor(int code) {
  if (code == 0) return WI_SUN;
  if (code <= 2) return WI_PARTLY;
  if (code == 3) return WI_CLOUD;
  if (code == 45 || code == 48) return WI_FOG;
  if (code >= 51 && code <= 57) return WI_DRIZZLE;
  if (code >= 61 && code <= 67) return WI_RAIN;
  if (code >= 71 && code <= 77) return WI_SNOW;
  if (code >= 80 && code <= 82) return WI_RAIN;
  if (code == 85 || code == 86) return WI_SNOW;
  if (code >= 95) return WI_STORM;
  return WI_CLOUD;
}

const char *wxTextFor(int code) {
  switch (code) {
    case 0:  return "Clear";
    case 1:  return "Mostly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Storm with hail";
    default: return "Unknown";
  }
}

const char *wxCompass(int deg) {
  static const char *DIRS[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return DIRS[((deg + 23) / 45) & 7];
}

uint16_t wxUvColour(float uv) {  // standard UV index bands
  if (uv < 3)  return COL_OK;
  if (uv < 6)  return 0xFFE0;    // yellow
  if (uv < 8)  return COL_ACCENT;
  if (uv < 11) return COL_ERR;
  return 0xB81F;                 // extreme: violet
}

// ---- icon drawing primitives (all sized as fractions of s) ----

void drawThickLine(int x0, int y0, int x1, int y1, uint16_t col) {
  tft.drawLine(x0, y0, x1, y1, col);
  tft.drawLine(x0 + 1, y0, x1 + 1, y1, col);
  tft.drawLine(x0, y0 + 1, x1, y1 + 1, col);
}

void drawSunShape(int cx, int cy, int s) {
  int r = s * 28 / 100;
  tft.fillCircle(cx, cy, r, COL_SUN);
  for (int i = 0; i < 8; i++) {
    float a = i * (PI / 4.0f);
    float ca = cosf(a), sa = sinf(a);
    int r0 = r + s * 7 / 100, r1 = r + s * 17 / 100;
    drawThickLine(cx + (int)(ca * r0), cy + (int)(sa * r0),
                  cx + (int)(ca * r1), cy + (int)(sa * r1), COL_SUN);
  }
}

void drawMoonShape(int cx, int cy, int s) {
  int r = s * 30 / 100;
  tft.fillCircle(cx, cy, r, COL_MOON);
  tft.fillCircle(cx + r * 5 / 8, cy - r * 3 / 8, r * 7 / 8, COL_BG);  // bite
}

void drawCloudShape(int cx, int cy, int s, uint16_t col) {
  tft.fillCircle(cx - s * 18 / 100, cy - s * 2 / 100, s * 20 / 100, col);
  tft.fillCircle(cx + s * 10 / 100, cy - s * 12 / 100, s * 26 / 100, col);
  tft.fillRoundRect(cx - s * 38 / 100, cy - s * 2 / 100,
                    s * 76 / 100, s * 18 / 100, s * 9 / 100, col);
}

void drawFlakeShape(int x, int y, int r, uint16_t col) {
  tft.drawLine(x - r, y, x + r, y, col);
  tft.drawLine(x, y - r, x, y + r, col);
  tft.drawLine(x - r * 7 / 10, y - r * 7 / 10, x + r * 7 / 10, y + r * 7 / 10, col);
  tft.drawLine(x - r * 7 / 10, y + r * 7 / 10, x + r * 7 / 10, y - r * 7 / 10, col);
}

void drawWxIcon(int cx, int cy, int s, WxIcon k, bool day) {
  switch (k) {
    case WI_SUN:
      if (day) drawSunShape(cx, cy, s);
      else     drawMoonShape(cx, cy, s);
      break;
    case WI_PARTLY:
      if (day) drawSunShape(cx + s * 16 / 100, cy - s * 14 / 100, s * 62 / 100);
      else     drawMoonShape(cx + s * 16 / 100, cy - s * 14 / 100, s * 62 / 100);
      drawCloudShape(cx - s * 4 / 100, cy + s * 8 / 100, s * 88 / 100, COL_CLOUD);
      break;
    case WI_CLOUD:
      drawCloudShape(cx + s * 12 / 100, cy - s * 12 / 100, s * 78 / 100, COL_CLOUD_DK);
      drawCloudShape(cx - s * 4 / 100, cy + s * 6 / 100, s * 92 / 100, COL_CLOUD);
      break;
    case WI_FOG:
      drawCloudShape(cx, cy - s * 12 / 100, s * 80 / 100, COL_CLOUD_DK);
      for (int i = 0; i < 3; i++) {
        int w = s * (64 - i * 14) / 100;
        int lx = cx - w / 2 + (i % 2 ? s * 6 / 100 : -(s * 4 / 100));
        int ly = cy + s * (10 + i * 10) / 100;
        tft.drawFastHLine(lx, ly, w, COL_CLOUD);
        tft.drawFastHLine(lx, ly + 1, w, COL_CLOUD);
      }
      break;
    case WI_DRIZZLE:
      drawCloudShape(cx, cy - s * 10 / 100, s * 85 / 100, COL_CLOUD);
      for (int i = 0; i < 3; i++)
        tft.fillCircle(cx - s * 16 / 100 + i * s * 16 / 100,
                       cy + s * (16 + (i % 2) * 8) / 100,
                       s > 40 ? 2 : 1, COL_RAIN);
      break;
    case WI_RAIN:
      drawCloudShape(cx, cy - s * 10 / 100, s * 85 / 100, COL_CLOUD);
      for (int i = 0; i < 3; i++) {
        int bx = cx - s * 16 / 100 + i * s * 16 / 100;
        tft.drawLine(bx, cy + s * 14 / 100,
                     bx - s * 6 / 100, cy + s * 30 / 100, COL_RAIN);
        tft.drawLine(bx + 1, cy + s * 14 / 100,
                     bx + 1 - s * 6 / 100, cy + s * 30 / 100, COL_RAIN);
      }
      break;
    case WI_SNOW: {
      drawCloudShape(cx, cy - s * 10 / 100, s * 85 / 100, COL_CLOUD);
      int fr = s * 6 / 100; if (fr < 2) fr = 2;
      for (int i = 0; i < 3; i++)
        drawFlakeShape(cx - s * 18 / 100 + i * s * 18 / 100,
                       cy + s * (20 + (i % 2) * 6) / 100, fr, COL_SNOWF);
      break;
    }
    case WI_STORM: {
      drawCloudShape(cx, cy - s * 10 / 100, s * 88 / 100, COL_CLOUD_DK);
      int u = s / 32; if (u < 1) u = 1;
      drawThickLine(cx + 3 * u, cy + 2 * u, cx - u, cy + 6 * u, COL_BOLT);
      drawThickLine(cx - u, cy + 6 * u, cx + 2 * u, cy + 6 * u, COL_BOLT);
      drawThickLine(cx + 2 * u, cy + 6 * u, cx - 3 * u, cy + 11 * u, COL_BOLT);
      break;
    }
  }
}

// Hand-drawn degree symbol (the built-in fonts have no reliable glyph)
void drawDegreeMark(int x, int y, int r, uint16_t col) {
  tft.drawCircle(x, y, r, col);
  if (r > 2) tft.drawCircle(x, y, r - 1, col);
}

// ---- fetch & parse ----

bool fetchWeather() {
  char url[560];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
           "is_day,weather_code,wind_speed_10m,wind_direction_10m"
           "&hourly=precipitation_probability&forecast_hours=12"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
           "precipitation_probability_max,uv_index_max"
           "&forecast_days=4&timezone=auto",
           WX_LAT, WX_LON);

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(7000);
  if (!http.begin(url)) return false;
  int rc = http.GET();
  if (rc != HTTP_CODE_OK) {
    Serial.printf("wx: HTTP %d\n", rc);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("wx: JSON %s\n", err.c_str());
    return false;
  }

  JsonObject cur = doc["current"];
  wx.temp    = cur["temperature_2m"] | 0.0f;
  wx.feels   = cur["apparent_temperature"] | 0.0f;
  wx.hum     = cur["relative_humidity_2m"] | 0;
  wx.code    = cur["weather_code"] | 0;
  wx.isDay   = (cur["is_day"] | 1) != 0;
  wx.wind    = cur["wind_speed_10m"] | 0.0f;
  wx.windDir = cur["wind_direction_10m"] | 0;

  JsonObject d = doc["daily"];
  wx.uvMax = d["uv_index_max"][0] | 0.0f;
  static const char *DAYS_UP[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  for (int i = 0; i < 4; i++) {
    wx.hiT[i]   = d["temperature_2m_max"][i] | 0.0f;
    wx.loT[i]   = d["temperature_2m_min"][i] | 0.0f;
    wx.dCode[i] = d["weather_code"][i] | 0;
    wx.dPop[i]  = d["precipitation_probability_max"][i] | 0;
    if (i == 0) {
      strcpy(wx.dayLbl[0], "TODAY");
    } else {
      int yy, mo, dd;
      const char *ds = d["time"][i] | "";
      if (sscanf(ds, "%d-%d-%d", &yy, &mo, &dd) == 3) {
        tm td = {};
        td.tm_year = yy - 1900; td.tm_mon = mo - 1; td.tm_mday = dd;
        td.tm_hour = 12;
        mktime(&td);  // fills tm_wday
        strcpy(wx.dayLbl[i], DAYS_UP[td.tm_wday]);
      } else {
        strcpy(wx.dayLbl[i], "---");
      }
    }
  }

  // first hour in the next 12 with a real chance of rain
  wx.rainHour = -1;
  wx.rainPop = 0;
  JsonArray hPop = doc["hourly"]["precipitation_probability"];
  JsonArray hTime = doc["hourly"]["time"];
  for (size_t i = 0; i < hPop.size() && i < hTime.size(); i++) {
    int p = hPop[i] | 0;
    if (p >= 40) {
      const char *ts = hTime[i] | "";        // "2026-07-09T14:00"
      if (strlen(ts) >= 13) wx.rainHour = atoi(ts + 11);
      wx.rainPop = p;
      break;
    }
  }

  wx.valid = true;
  wx.fetchedAt = time(nullptr);
  Serial.printf("wx: %.1fC code %d pop %d%%\n", wx.temp, wx.code, wx.dPop[0]);
  return true;
}

// ---- page drawing ----

void drawWxHeaderClock() {
  time_t now = time(nullptr);
  tm t;
  localtime_r(&now, &t);
  if (t.tm_year < 100) return;
  wxLastMin = t.tm_min;
  int h12 = t.tm_hour % 12;
  if (h12 == 0) h12 = 12;
  char buf[12];
  snprintf(buf, sizeof(buf), "%d:%02d %s", h12, t.tm_min,
           t.tm_hour < 12 ? "AM" : "PM");
  tft.fillRect(240, 0, 80, 20, COL_BG);
  tft.setTextFont(2);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_WXTEXT, COL_BG);
  tft.drawString(buf, 312, 6);
}

// Centred row of small stats: feels-like / humidity / wind / UV (colour-coded)
void drawWxStatsRow(int y) {
  char fs[16], hs[16], ws[24], us[12];
  snprintf(fs, sizeof(fs), "Feels %.0f", wx.feels);
  snprintf(hs, sizeof(hs), "Hum %d%%", wx.hum);
  snprintf(ws, sizeof(ws), "%s %.0f km/h", wxCompass(wx.windDir), wx.wind);
  snprintf(us, sizeof(us), "UV %.0f", wx.uvMax);
  const char *txt[4] = {fs, hs, ws, us};
  uint16_t col[4] = {COL_WXTEXT, COL_WXTEXT, COL_WXTEXT, wxUvColour(wx.uvMax)};

  tft.setTextFont(2);
  tft.setTextDatum(ML_DATUM);
  const int gap = 14, degW = 5;
  int w[4], total = degW + gap * 3;
  for (int i = 0; i < 4; i++) { w[i] = tft.textWidth(txt[i]); total += w[i]; }

  int x = (SCR_W - total) / 2;
  for (int i = 0; i < 4; i++) {
    tft.setTextColor(col[i], COL_BG);
    tft.drawString(txt[i], x, y);
    x += w[i];
    if (i == 0) {  // degree mark after the feels-like value
      drawDegreeMark(x + 2, y - 5, 2, COL_WXTEXT);
      x += degW;
    }
    if (i < 3) {
      tft.fillCircle(x + gap / 2, y, 1, COL_COLON_OFF);
      x += gap;
    }
  }
}

// One row of the right-hand column: direction triangle + value + degree mark,
// centred as a unit on cx.
void drawWxHiLoRow(int cx, int y, float v, bool isHi) {
  char b[8];
  snprintf(b, sizeof(b), "%.0f", v);
  tft.setTextFont(4);
  int tw = tft.textWidth(b);
  int x = cx - (14 + tw + 8) / 2;
  uint16_t col = isHi ? COL_TIME : COL_WXTEXT;
  if (isHi) tft.fillTriangle(x + 5, y - 6, x, y + 4, x + 10, y + 4, COL_ACCENT);
  else      tft.fillTriangle(x + 5, y + 4, x, y - 6, x + 10, y - 6, COL_RAIN);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(col, COL_BG);
  tft.drawString(b, x + 14, y);
  drawDegreeMark(x + 14 + tw + 4, y - 7, 3, col);
}

void drawWeatherPage() {
  tft.fillScreen(COL_BG);

  // header: nav hint, location, live clock
  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_COLON_OFF, COL_BG);
  tft.drawString("< TIMER", 8, 6);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_WXTEXT, COL_BG);
  tft.drawString(WX_NAME, SCR_W / 2, 6);
  drawWxHeaderClock();

  if (!wx.valid) {
    tft.setTextFont(4);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_DATE, COL_BG);
    tft.drawString("Fetching weather...", SCR_W / 2, SCR_H / 2 - 10);
    tft.setTextFont(2);
    tft.setTextColor(COL_COLON_OFF, COL_BG);
    tft.drawString("tap to retry", SCR_W / 2, SCR_H / 2 + 16);
    return;
  }

  // hero band: big icon and big temperature (font 8 at y=64 spans y 27-102)
  drawWxIcon(48, 64, 64, wxIconFor(wx.code), wx.isDay);

  char buf[16];
  snprintf(buf, sizeof(buf), "%.0f", wx.temp);
  tft.setTextFont(8);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_TIME, COL_BG);
  int tw = tft.drawString(buf, 98, 64);
  drawDegreeMark(98 + tw + 10, 34, 7, COL_TIME);

  // right-hand column, everything centred on one axis: today's hi/lo,
  // the rain outlook (two short lines) and the UV index
  constexpr int RCOL_X = 282;
  drawWxHiLoRow(RCOL_X, 42, wx.hiT[0], true);
  drawWxHiLoRow(RCOL_X, 72, wx.loT[0], false);

  char l1[16], l2[16];
  uint16_t rainCol = COL_WXTEXT;
  if (wx.rainHour >= 0) {
    int h12 = wx.rainHour % 12;
    if (h12 == 0) h12 = 12;
    snprintf(l1, sizeof(l1), "Rain %d%%", wx.rainPop);
    snprintf(l2, sizeof(l2), "at %d %s", h12, wx.rainHour < 12 ? "AM" : "PM");
    rainCol = COL_RAIN;
  } else if (wx.dPop[0] >= 20) {
    snprintf(l1, sizeof(l1), "Rain %d%%", wx.dPop[0]);
    snprintf(l2, sizeof(l2), "today");
  } else {
    snprintf(l1, sizeof(l1), "No rain");
    snprintf(l2, sizeof(l2), "expected");
  }
  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(rainCol, COL_BG);
  tft.drawString(l1, RCOL_X, 102);
  tft.drawString(l2, RCOL_X, 118);

  // condition text (font 4, 26 px: y 108-134, clear of the right column)
  tft.setTextFont(4);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_TIME, COL_BG);
  tft.drawString(wxTextFor(wx.code), SCR_W / 2, 121);

  drawWxStatsRow(148);                   // y 140-156

  tft.drawFastHLine(12, 161, SCR_W - 24, COL_COLON_OFF);

  // 4-day forecast strip (labels y 163-179, icons to ~207, temps y 209-225,
  // rain odds y 223-239)
  tft.setTextFont(2);
  for (int i = 0; i < 4; i++) {
    int cx = 40 + i * 80;
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(i == 0 ? COL_ACCENT : COL_WXTEXT, COL_BG);
    tft.drawString(wx.dayLbl[i], cx, 171);
    drawWxIcon(cx, 195, 26, wxIconFor(wx.dCode[i]), true);

    char hb[8], lb[8];
    snprintf(hb, sizeof(hb), "%.0f", wx.hiT[i]);
    snprintf(lb, sizeof(lb), "%.0f", wx.loT[i]);
    int w1 = tft.textWidth(hb), w2 = tft.textWidth(lb);
    int x0 = cx - (w1 + 8 + w2) / 2;
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(COL_TIME, COL_BG);
    tft.drawString(hb, x0, 217);
    tft.setTextColor(COL_WXTEXT, COL_BG);
    tft.drawString(lb, x0 + w1 + 8, 217);

    snprintf(hb, sizeof(hb), "%d%%", wx.dPop[i]);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(wx.dPop[i] >= 20 ? COL_RAIN : COL_WXTEXT, COL_BG);
    tft.drawString(hb, cx, 231);
  }
}

// Keep the header clock ticking while the weather page is shown.
void weatherLoop() {
  time_t now = time(nullptr);
  tm t;
  localtime_r(&now, &t);
  if (t.tm_year >= 100 && t.tm_min != wxLastMin) drawWxHeaderClock();
}

// Runs every loop pass: performs a pending fetch (set by the NTP sync
// callback or a tap), retries until the first success, and re-fetches if the
// data somehow goes stale. Attempts are spaced at least 60 s apart.
void weatherService() {
  static uint32_t lastAttemptMs = 0;
  bool retryFirst = !wx.valid && lastSyncTime != 0;
  bool stale = wx.valid && time(nullptr) - wx.fetchedAt > 40 * 60;
  if (!wxFetchPending && !retryFirst && !stale) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (lastAttemptMs != 0 && millis() - lastAttemptMs < 60000UL) return;
  lastAttemptMs = millis();
  wxFetchPending = false;
  if (fetchWeather() && page == PAGE_WEATHER) drawWeatherPage();
}

// ------------------------------------------------------------- pomodoro page

uint32_t pomoWorkMs() { return WORK_OPTIONS[workIdx] * 60000UL; }

uint16_t pomoAccent() { return pomoMode == POMO_WORK ? COL_ACCENT : COL_REST; }

void drawPomoHeader() {
  tft.fillRect(80, 4, 160, 30, COL_BG);
  tft.setTextFont(4);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(pomoAccent(), COL_BG);
  tft.drawString(pomoMode == POMO_WORK ? "WORK" : "BREAK", SCR_W / 2, 20);
}

// Render the countdown MM:SS into the clock's time sprite buffer (only one
// page is ever drawn at a time, so reuse is safe).
void renderPomoDigits() {
  int totalSec = (pomoRemainMs + 999) / 1000;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", totalSec / 60, totalSec % 60);

  timeSpr.fillSprite(COL_BG);
  timeSpr.setTextFont(8);
  timeSpr.setTextDatum(MC_DATUM);
  timeSpr.setTextColor(pomoState == POMO_PAUSE ? COL_DATE : COL_TIME, COL_BG);
  timeSpr.drawString(buf, SCR_W / 2, TIME_SPR_H / 2);
  pomoSrcW = timeSpr.textWidth(buf);
  pomoScaleMax = fminf((float)(SCR_W - 8) / pomoSrcW, 1.6f);
}

// Push the sprite's digits to screen rows [y0, y1) scaled about their centre.
// Output is built one row at a time in a line buffer, so there is no flicker.
void pushTimeSprScaled(float scale, int centerY, int y0, int y1) {
  static uint16_t line[SCR_W];
  static int sxMap[SCR_W];
  for (int dx = 0; dx < SCR_W; dx++) {
    int sxi = (int)(SCR_W / 2 + (dx - SCR_W / 2) / scale + 0.5f);
    sxMap[dx] = (sxi >= 0 && sxi < SCR_W) ? sxi : -1;
  }
  for (int dy = y0; dy < y1; dy++) {
    int syi = (int)(TIME_SPR_H / 2 + (dy - centerY) / scale + 0.5f);
    if (syi < 0 || syi >= TIME_SPR_H) {
      for (int dx = 0; dx < SCR_W; dx++) line[dx] = COL_BG;
    } else {
      for (int dx = 0; dx < SCR_W; dx++)
        line[dx] = (sxMap[dx] < 0) ? COL_BG : timeSpr.readPixel(sxMap[dx], syi);
    }
    tft.pushImage(0, dy, SCR_W, 1, line);
  }
}

void drawPomoTimerFrame(float scale, int centerY) {
  pushTimeSprScaled(scale, centerY, ANIM_Y0, ANIM_Y1);
}

// Smoothly grow the digits into the chip row's space (or shrink back out of
// it). Blocking for ~350 ms; the timer tick is millis()-based so no time is
// lost. The first frame overwrites the chips, which is what hides them.
void animatePomoResize(bool grow) {
  uint32_t t0 = millis();
  const uint32_t DUR_MS = 350;
  float p;
  do {
    p = fminf(1.0f, (millis() - t0) / (float)DUR_MS);
    float e = p * p * (3.0f - 2.0f * p);  // smoothstep easing
    if (!grow) e = 1.0f - e;
    float scale = 1.0f + (pomoScaleMax - 1.0f) * e;
    int cy = ANIM_CY_SMALL + (int)((ANIM_CY_BIG - ANIM_CY_SMALL) * e + 0.5f);
    drawPomoTimerFrame(scale, cy);
  } while (p < 1.0f);
}

void drawPomoTimer() {
  renderPomoDigits();
  if (chipsHidden) {
    drawPomoTimerFrame(pomoScaleMax, ANIM_CY_BIG);
  } else {
    timeSpr.pushSprite(0, POMO_TIMER_Y);
  }
}

void drawPomoChips() {
  tft.setTextFont(4);
  tft.setTextDatum(MC_DATUM);
  for (int i = 0; i < NUM_WORK_OPTIONS; i++) {
    int x = CHIP_X0 + i * (CHIP_W + CHIP_GAP);
    bool sel = (i == workIdx);
    tft.fillRoundRect(x, CHIP_Y, CHIP_W, CHIP_H, 8, sel ? COL_ACCENT : COL_BG);
    tft.drawRoundRect(x, CHIP_Y, CHIP_W, CHIP_H, 8, sel ? COL_ACCENT : COL_COLON_OFF);
    tft.setTextColor(sel ? COL_BG : COL_DATE, sel ? COL_ACCENT : COL_BG);
    char lbl[6];
    snprintf(lbl, sizeof(lbl), "%dm", WORK_OPTIONS[i]);
    tft.drawString(lbl, x + CHIP_W / 2, CHIP_Y + CHIP_H / 2);
  }
}

void drawPomoButtons() {
  tft.setTextFont(4);
  tft.setTextDatum(MC_DATUM);

  const char *startLbl = (pomoState == POMO_RUN) ? "PAUSE"
                       : (pomoState == POMO_PAUSE) ? "RESUME" : "START";
  uint16_t startCol = (pomoState == POMO_RUN) ? COL_DATE : pomoAccent();
  tft.fillRoundRect(BTN_START_X, BTN_Y, BTN_W, BTN_H, 8, COL_BG);
  tft.drawRoundRect(BTN_START_X, BTN_Y, BTN_W, BTN_H, 8, startCol);
  tft.setTextColor(startCol, COL_BG);
  tft.drawString(startLbl, BTN_START_X + BTN_W / 2, BTN_Y + BTN_H / 2);

  tft.fillRoundRect(BTN_RESET_X, BTN_Y, BTN_W, BTN_H, 8, COL_BG);
  tft.drawRoundRect(BTN_RESET_X, BTN_Y, BTN_W, BTN_H, 8, COL_COLON_OFF);
  tft.setTextColor(COL_DATE, COL_BG);
  tft.drawString("RESET", BTN_RESET_X + BTN_W / 2, BTN_Y + BTN_H / 2);
}

void drawPomoPage() {
  tft.fillScreen(COL_BG);
  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_COLON_OFF, COL_BG);
  tft.drawString("< CLOCK", 8, 8);
  drawPomoHeader();
  if (!chipsHidden) drawPomoChips();
  drawPomoButtons();
  pomoLastShownSec = -1;  // force countdown redraw
}

// Session finished: get attention, then move to the next phase.
void pomoFinish() {
  boostUntil = millis() + TOUCH_BOOST_MS;  // full brightness
  for (int i = 0; i < 3; i++) {
    tft.fillScreen(pomoAccent());
    delay(140);
    tft.fillScreen(COL_BG);
    delay(140);
  }
  if (pomoMode == POMO_WORK) {
    // work done -> break starts automatically
    pomoMode = POMO_REST;
    pomoRemainMs = REST_MIN * 60000UL;
    pomoState = POMO_RUN;
    pomoLastTick = millis();
  } else {
    // break done -> full cycle complete: chips come back, wait for START
    pomoMode = POMO_WORK;
    pomoRemainMs = pomoWorkMs();
    pomoState = POMO_IDLE;
    chipsHidden = false;
  }
  page = PAGE_POMO;  // pull the display here even if the clock was showing
  drawPomoPage();
}

// Runs every loop pass regardless of page, so the timer keeps counting
// while the clock is displayed.
void tickPomodoro() {
  if (pomoState != POMO_RUN) return;
  uint32_t now = millis();
  uint32_t dt = now - pomoLastTick;
  pomoLastTick = now;
  if (pomoRemainMs > dt) {
    pomoRemainMs -= dt;
  } else {
    pomoRemainMs = 0;
    pomoFinish();
  }
}

void pomoResetTo(int idx) {
  workIdx = idx;
  pomoMode = POMO_WORK;
  pomoState = POMO_IDLE;
  pomoRemainMs = pomoWorkMs();
  drawPomoHeader();
  drawPomoButtons();
  if (chipsHidden) {
    chipsHidden = false;
    renderPomoDigits();
    animatePomoResize(false);  // shrink digits back out of the chip row
    drawPomoChips();
  } else {
    drawPomoChips();
  }
  pomoLastShownSec = -1;
}

void handlePomoTap(int x, int y) {
  // duration chips (only active while visible)
  if (!chipsHidden && y >= CHIP_Y - 4 && y <= CHIP_Y + CHIP_H + 4) {
    for (int i = 0; i < NUM_WORK_OPTIONS; i++) {
      int cx = CHIP_X0 + i * (CHIP_W + CHIP_GAP);
      if (x >= cx && x <= cx + CHIP_W) {
        pomoResetTo(i);
        return;
      }
    }
    return;
  }
  // start / pause / resume
  if (y >= BTN_Y - 4 && x >= BTN_START_X && x <= BTN_START_X + BTN_W) {
    if (pomoState == POMO_RUN) {
      pomoState = POMO_PAUSE;
    } else {
      bool freshStart = (pomoState == POMO_IDLE);
      pomoState = POMO_RUN;
      pomoLastTick = millis();
      if (freshStart && !chipsHidden) {
        // hide the chips and grow the digits into their space
        chipsHidden = true;
        drawPomoButtons();
        renderPomoDigits();
        animatePomoResize(true);
        pomoLastShownSec = -1;
        return;
      }
    }
    drawPomoButtons();
    pomoLastShownSec = -1;  // repaint countdown (colour changes when paused)
    return;
  }
  // reset
  if (y >= BTN_Y - 4 && x >= BTN_RESET_X && x <= BTN_RESET_X + BTN_W) {
    pomoResetTo(workIdx);
  }
}

// Redraw the countdown when the displayed second changes.
void pomoLoop() {
  int sec = (pomoRemainMs + 999) / 1000;
  if (sec != pomoLastShownSec) {
    pomoLastShownSec = sec;
    drawPomoTimer();
  }
}

// ------------------------------------------------------------ pages & input

void switchPage() {
  if (page == PAGE_CLOCK) {
    page = PAGE_WEATHER;
    drawWeatherPage();
  } else if (page == PAGE_WEATHER) {
    page = PAGE_POMO;
    drawPomoPage();
  } else {
    page = PAGE_CLOCK;
    tft.fillScreen(COL_BG);
    clockLastSec = -1;   // force full clock redraw
    clockLastYday = -1;
    dotLastCol = 0;
  }
}

void handleTouch() {
  static bool wasTouched = false;
  bool isTouched = touch.touched();
  if (!isTouched) {
    wasTouched = false;
    return;
  }
  boostUntil = millis() + TOUCH_BOOST_MS;  // any touch -> full brightness
  if (wasTouched) return;                  // act on press edge only
  wasTouched = true;

  TS_Point p = touch.getPoint();
  // CALIBRATION MAPPING (typical values for this screen)
  int x = map(p.x, 200, 3700, 0, 320);
  int y = map(p.y, 240, 3800, 0, 240);
  x = constrain(x, 0, 320);
  y = constrain(y, 0, 240);

  // top-left corner switches pages (both pages)
  if (x < 80 && y < 48) {
    switchPage();
    return;
  }
  if (page == PAGE_POMO) handlePomoTap(x, y);
  // tap anywhere on the weather page = manual refresh (rate-limited)
  else if (page == PAGE_WEATHER) wxFetchPending = true;
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
  tft.setSwapBytes(true);  // pushImage takes normal RGB565 values
  tft.fillScreen(COL_BG);

  timeSpr.createSprite(SCR_W, TIME_SPR_H);
  dateSpr.createSprite(SCR_W, DATE_SPR_H);

  showMessage("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
#if USE_STATIC_IP
  if (!WiFi.config(STATIC_IP, STATIC_GATEWAY, STATIC_SUBNET,
                   STATIC_DNS1, STATIC_DNS2)) {
    Serial.println("WiFi.config (static IP) FAILED");
  }
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }

  // Connection diagnostics
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected. IP: %s  GW: %s  DNS: %s\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.dnsIP().toString().c_str());
    IPAddress resolved;
    if (WiFi.hostByName(NTP_SERVER1, resolved)) {
      Serial.printf("DNS test: %s -> %s\n", NTP_SERVER1,
                    resolved.toString().c_str());
    } else {
      Serial.printf("DNS test: %s FAILED to resolve\n", NTP_SERVER1);
    }
  } else {
    Serial.printf("WiFi connect FAILED (status %d) - will keep retrying\n",
                  (int)WiFi.status());
    // Scan and list what the board can actually see (2.4 GHz only)
    Serial.println("Scanning for networks...");
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
      Serial.printf("  %2d: %-24s ch%2d %4d dBm %s\n", i + 1,
                    WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i),
                    WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured");
    }
    WiFi.scanDelete();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // resume retrying after the scan
  }

  // SNTP: sync now and every NTP_SYNC_INTERVAL_MIN minutes thereafter.
  // NTP_SERVER1 is typically the router (LAN, no DNS/WAN needed); NTP_SERVER2
  // is an internet fallback; 216.239.35.4 is a Google time server by raw IP so
  // time still syncs even if DNS is broken on this network.
  sntp_set_sync_interval(NTP_SYNC_INTERVAL_MIN * 60 * 1000UL);
  sntp_set_time_sync_notification_cb(onNtpSync);
  configTzTime(TZ_INFO, NTP_SERVER1, NTP_SERVER2, "216.239.35.4");

  showMessage("Waiting for time sync...");
}

void loop() {
  handleTouch();
  updateBrightness();
  tickPomodoro();
  weatherService();

  if (page == PAGE_CLOCK) {
    clockLoop();
  } else if (page == PAGE_WEATHER) {
    weatherLoop();
  } else {
    pomoLoop();
  }
  delay(50);
}
