// Simple 12-hour NTP clock + pomodoro timer for the ESP32 Cheap Yellow
// Display (ESP32-2432S028R).
//
// Hardware setup follows the CYD-Dual-SPI reference project:
//   - Screen on VSPI (TFT_eSPI, pins in platformio.ini)
//   - Touch (XPT2046) on its own HSPI bus
//   - LDR auto-brightness on GPIO 34 -> backlight PWM on GPIO 21
//
// Pages: tap the top-left corner of the screen to switch between the clock
// and the pomodoro timer. The pomodoro keeps running while the clock page
// is shown and pulls the display back to itself when a session ends.
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

// PAGES
enum Page { PAGE_CLOCK, PAGE_POMO };
Page page = PAGE_CLOCK;

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

  if (page == PAGE_CLOCK) {
    clockLoop();
  } else {
    pomoLoop();
  }
  delay(50);
}
