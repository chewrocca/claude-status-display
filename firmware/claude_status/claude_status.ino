// Claude Code status display for Waveshare ESP32-C6-LCD-1.47 (172x320 ST7789).
// Host daemon (host/claude_status_daemon.py) sends one JSON line per update over
// USB serial. Shows context / rate-limit gauges, attention state, API health,
// with an RGB LED language and a one-button (BOOT) interaction model.
// Every page has a portrait (172x320) and a landscape (320x172) layout.
//
//   short press : wake screen / acknowledge alert / next page
//   hold 0.8 s  : toggle night mode
//   hold 3 s    : rotate screen 90 degrees (saved)

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include "sprites.h"

#define FW_VERSION "8.4"

// --- board pins (Waveshare wiki: ESP32-C6-LCD-1.47) -----------------------
#define PIN_MOSI 6
#define PIN_SCLK 7
#define PIN_CS   14
#define PIN_DC   15
#define PIN_RST  21
#define PIN_BL   22
#define PIN_RGB  8
#define PIN_BTN  9      // BOOT button, active low
#define PIN_MISO 5

#define LCD_W 172
#define LCD_H 320
#define DEFAULT_ROTATION 1   // 1/3 landscape, 0/2 portrait

// --- timing ------------------------------------------------------------------
#define LINK_TIMEOUT_MS   15000UL
#define STALE_MS          (10UL * 60UL * 1000UL)
#define PAGE_RETURN_MS    20000UL
#define TOAST_MS          1500UL
#define IDLE_DIM_MS       (5UL * 60UL * 1000UL)
#define CYCLE_MS          (1UL * 60UL * 1000UL)   // after 1 min idle: cycle through the pages
#define CYCLE_PAGE_MS     8000UL
#define SAVER_MS          (5UL * 60UL * 1000UL)   // after 5 min idle: space screensaver
#define NOLINK_DIM_MS     (2UL * 60UL * 1000UL)
#define BTN_LONG_MS       800UL
#define BTN_ROTATE_MS     3000UL

// --- palette (RGB565) ----------------------------------------------------------
#define C_BG      0x0000
#define C_PANEL   0x2104
#define C_TXT     0xFFFF
#define C_DIM     0x8410
#define C_GREEN   0x07E6
#define C_AMBER   0xFD20
#define C_RED     0xF800
#define C_BLUE    0x3D7F
#define C_MAGENTA 0xF81F
#define C_ORANGE  0xFC00
#define C_CLAUDE  0xDBAA   // Claude orange #D97757

SPIClass spi(FSPI);
Adafruit_ST7789 tft(&spi, PIN_CS, PIN_DC, PIN_RST);
uint16_t *framebuf = nullptr;      // one 110 KB buffer, allocated once and shared by both orientations
class SharedCanvas : public GFXcanvas16 {   // GFXcanvas16 only allocates or not; this one adopts our buffer
 public:
  SharedCanvas(uint16_t w, uint16_t h, uint16_t *buf) : GFXcanvas16(w, h, false) { buffer = buf; }
  ~SharedCanvas() { buffer = nullptr; }
};
SharedCanvas *cv = nullptr;
Adafruit_NeoPixel led(1, PIN_RGB, NEO_GRB + NEO_KHZ800);
Preferences prefs;

// --- state from host -------------------------------------------------------------
struct Status {
  int ctx = -1, h5 = -1, wk = -1, h5m = -1, wkm = -1, n = 0, age = -1;
  char h5r[12] = "", wkr[16] = "", hm[10] = "";
  char st[16] = "idle";        // working | done | needs_input | idle
  char out[12] = "unknown";    // none | minor | major | critical | unknown
  char inc[128] = "", other[84] = "", comp[8] = "", model[24] = "", dir[24] = "", eff[10] = "";
  float cost = 0;
  bool lim = false, night = false, cw = false;
  int dur = 0, api = 0, la = 0, lr = 0, tin = 0, tout = 0, ch = -1;
  int pace = 999, quiet = 70, nhist = 0;
  uint8_t hist[40];
  char ver[10] = "";
  long ts = 0;
} S;

const char *clockStr();
void netBegin(); void netLoop(); void netForcePoll(); void netCommand(JsonDocument &doc); void netReport(); void bleNotifyState();
extern char netOut[12]; extern char netComp[8]; extern char netOther[84]; extern bool wifiUp; extern char transport[8];

struct BandStyle { const char *l1, *l2; uint16_t bg, fg; const uint16_t *spr; };
enum Headline { H_NOLINK, H_IDLE, H_WORKING, H_DONE, H_NEEDS, H_LIMITED, H_OUTAGE };
enum Page { PG_OVERVIEW, PG_LIMITS, PG_STATS, PG_API, PG_ABOUT, PG_COUNT };

char rx[1024]; uint16_t rxLen = 0;
unsigned long lastRx = 0, rxAt = 0, stateChangedAt = 0;
unsigned long lastDirChange = 0, pageChangedAt = 0, toastAt = 0, lastDraw = 0;
bool haveLink = false, dirty = true, ack = false, manualNight = false, nightOverride = false;
bool hostNightLast = false;
Headline head = H_NOLINK, lastHead = H_NOLINK;
Page page = PG_OVERVIEW;
char toast[16] = "";
uint8_t rotation = DEFAULT_ROTATION, backlight = 0;

// --- helpers ---------------------------------------------------------------------
int ageSec();
bool stale() { return haveLink && millis() - rxAt > STALE_MS; }   // no push in 10 min, not "user is idle"
bool nightMode() { return nightOverride ? manualNight : S.night; }
int ageSec() { return S.age < 0 ? -1 : S.age + (int)((millis() - rxAt) / 1000); }
bool landscape() { return cv->width() > cv->height(); }
int W() { return cv->width(); }
int H() { return cv->height(); }

const char *apiOut() { return haveLink ? S.out : netOut; }
bool outageMajor() { return !strcmp(apiOut(), "major") || !strcmp(apiOut(), "critical"); }
bool apiDegraded() { return !strcmp(apiOut(), "minor"); }

Headline computeHeadline() {
  if (!haveLink) return H_NOLINK;
  if (!strcmp(S.st, "needs_input")) return H_NEEDS;
  if (!strcmp(S.st, "done")) return H_DONE;
  if (outageMajor()) return H_OUTAGE;
  if (S.lim) return H_LIMITED;
  if (!strcmp(S.st, "idle")) return H_IDLE;
  return H_WORKING;
}
bool alertActive() { return (head == H_DONE || head == H_NEEDS) && !ack && !stale(); }

uint16_t rgb565(int r, int g, int b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
uint16_t lerpColor(int r0, int g0, int b0, int r1, int g1, int b1, float t) {
  t = constrain(t, 0.0f, 1.0f);
  return rgb565(r0 + (r1 - r0) * t, g0 + (g1 - g0) * t, b0 + (b1 - b0) * t);
}
// Gauge colour slides green -> yellow -> orange -> red as usage climbs; >= 95 pulses.
uint16_t pctColor(int p) {
  if (p < 0) return C_DIM;
  uint16_t c;
  if (p < 40)      c = C_GREEN;
  else if (p < 70) c = lerpColor(0, 255, 48, 255, 230, 0, (p - 40) / 30.0f);     // green -> yellow
  else if (p < 90) c = lerpColor(255, 230, 0, 255, 120, 0, (p - 70) / 20.0f);    // yellow -> orange
  else             c = lerpColor(255, 120, 0, 255, 0, 0, (p - 90) / 10.0f);      // orange -> red
  return c;
}
uint16_t dimIf(uint16_t c) { return stale() ? C_DIM : c; }

void fmtRemaining(int mins, char *out, size_t n) {
  if (mins < 0) { out[0] = 0; return; }
  if (mins < 60) snprintf(out, n, "%dm", mins);
  else if (mins < 48 * 60) snprintf(out, n, "%dh%02dm", mins / 60, mins % 60);
  else snprintf(out, n, "%dd %dh", mins / 1440, (mins % 1440) / 60);
}
// --- drawing primitives ------------------------------------------------------------
int16_t textW(const char *s, uint8_t size) { return strlen(s) * 6 * size - size; }

void textAt(int x, int y, const char *s, uint8_t size, uint16_t col) {
  cv->setFont(NULL);
  cv->setTextWrap(false);
  cv->setTextSize(size);
  cv->setTextColor(col);
  cv->setCursor(x, y);
  cv->print(s);
}
void textCenteredIn(int x0, int w, int y, const char *s, uint8_t size, uint16_t col) {
  textAt(x0 + (w - textW(s, size)) / 2, y, s, size, col);
}
void textCentered(int y, const char *s, uint8_t size, uint16_t col) { textCenteredIn(0, W(), y, s, size, col); }
void textRight(int y, const char *s, uint8_t size, uint16_t col, int margin = 12) {   // clear of a case bezel
  textAt(W() - margin - textW(s, size), y, s, size, col);
}
void bigNumber(int x0, int w, int yBaseline, const char *s, uint16_t col) {   // FreeSansBold24pt, centred in [x0, x0+w)
  cv->setFont(&FreeSansBold24pt7b);
  cv->setTextSize(1);
  cv->setTextColor(col);
  int16_t bx, by; uint16_t bw, bh;
  cv->getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  cv->setCursor(x0 + (w - bw) / 2 - bx, yBaseline);
  cv->print(s);
  cv->setFont(NULL);
}

void drawSprite(int x, int y, const uint16_t *spr, int scale = 1, bool flip = false) {
  for (int j = 0; j < SPRITE_SZ; j++)
    for (int i = 0; i < SPRITE_SZ; i++) {
      uint16_t c = pgm_read_word(&spr[j * SPRITE_SZ + (flip ? SPRITE_SZ - 1 - i : i)]);
      if (c == SPRITE_KEY) continue;
      if (stale()) c = (c & 0xF7DE) >> 1;   // half brightness when stale
      if (scale == 1) cv->drawPixel(x + i, y + j, c);
      else cv->fillRect(x + i * scale, y + j * scale, scale, scale, c);
    }
}

// Scrolling label: draws text inside [x, x+w); scrolls when it does not fit. Callers that
// have a coloured area to the left must draw that area afterwards (overflow is not clipped).
bool marqueeActive = false;
void marquee(int x, int y, int w, const char *text, uint8_t size, uint16_t col, uint16_t bg = C_BG, int clipTo = -1) {
  int tw = textW(text, size);
  if (tw <= w) { textAt(x, y, text, size, col); return; }
  marqueeActive = true;
  int gap = 36, span = tw + gap;
  int off = (millis() / 30) % span;
  textAt(x - off, y, text, size, col);
  textAt(x - off + span, y, text, size, col);
  if (clipTo < 0) clipTo = W();
  cv->fillRect(x + w, y, clipTo - (x + w), 8 * size, bg);   // clip only within the caller's region
}

// Claude mark: twelve tapered rays in Claude orange, fits a size x size box
void drawClaudeMark(int cx, int cy, int size, uint16_t col = C_CLAUDE) {
  float R = size / 2.0f;
  for (int i = 0; i < 12; i++) {
    float a = i * 0.5235988f + 0.26f;                    // 30 degrees apart, slight rotation
    float len = R * ((i % 3 == 0) ? 1.0f : (i % 3 == 1) ? 0.82f : 0.9f);
    float w = R * 0.16f;
    float dx = cosf(a), dy = sinf(a);
    int tx = cx + dx * len, ty = cy + dy * len;
    int bx = cx + dx * R * 0.12f, by = cy + dy * R * 0.12f;
    cv->fillTriangle(bx - dy * w, by + dx * w, bx + dy * w, by - dx * w, tx, ty, col);
    cv->fillTriangle(bx - dy * w * 0.6f, by + dx * w * 0.6f, tx - dy * w * 0.35f, ty + dx * w * 0.35f, tx + dy * w * 0.35f, ty - dx * w * 0.35f, col);
  }
  cv->fillCircle(cx, cy, R * 0.18f, col);
}

// word-wrap size-2 text at `chars` per line; returns lines drawn
int wrapText(int x, int y, const char *s, int chars, uint16_t col, int maxLines) {
  char line[48]; int lines = 0;
  const char *p = s;
  while (*p && lines < maxLines) {
    const char *e = p, *lastSpace = NULL; int len = 0;
    while (*e && len < chars) { if (*e == ' ') lastSpace = e; e++; len++; }
    if (*e && lastSpace && *e != ' ') e = lastSpace;
    int n = min((int)(e - p), 47);
    memcpy(line, p, n); line[n] = 0;
    textAt(x, y + lines * 18, line, 2, col);
    lines++;
    p = e; while (*p == ' ') p++;
  }
  return lines;
}

void bar(int x, int y, int w, int h, int pct, uint16_t col, const char *inside) {
  cv->fillRoundRect(x, y, w, h, 4, C_PANEL);
  int fw = pct > 0 ? w * min(pct, 100) / 100 : 0;
  if (fw) cv->fillRoundRect(x, y, max(fw, 6), h, 4, col);
  if (inside && inside[0]) {
    int tx = x + w - 4 - textW(inside, 2);
    uint16_t tc = (x + fw > tx + 4) ? 0x0000 : C_DIM;
    textAt(tx, y + (h - 16) / 2 + 1, inside, 2, tc);
  }
}

// --- band (state block) ----------------------------------------------------------
// Returns the state's colour as 8-bit RGB. The band and the LED both derive from this,
// so they are the same colour by construction rather than by two lists kept in step.
void stateRGB(uint8_t &r, uint8_t &g, uint8_t &b) {
  switch (head) {
    case H_WORKING: {                                   // rainbow: one lap every 6 s
      uint32_t c = led.ColorHSV((uint16_t)((millis() % 6000UL) * 65535UL / 6000UL), 255, 255);
      r = c >> 16; g = c >> 8; b = c; return;
    }
    case H_DONE:    r = 255; g = 165; b = 0;   return;  // amber
    case H_NEEDS:   r = 255; g = 129; b = 0;   return;  // orange
    case H_LIMITED: r = 255; g = 0;   b = 255; return;  // magenta
    case H_OUTAGE:  r = 255; g = 0;   b = 0;   return;  // red
    default:        r = 32;  g = 32;  b = 32;  return;  // idle / no link: neutral grey
  }
}
uint16_t stateColor565() {
  uint8_t r, g, b; stateRGB(r, g, b);
  return rgb565(r, g, b);
}

BandStyle bandStyle() {
  BandStyle b; b.fg = 0x0000;
  switch (head) {
    case H_NOLINK:  b = {"NO",    "LINK",  C_PANEL,   C_DIM,  sprite_bot_nolink}; break;   // API row still works from the board's own poll
    case H_IDLE:    b = {"IDLE",  "",      C_PANEL,   C_DIM,  sprite_bot_idle}; break;
    case H_WORKING: b = {"BUSY",  "",      0,         0x0000, sprite_bot_working}; break;
    case H_DONE:    b = {"READY", "",      C_AMBER,   0x0000, sprite_bot_done}; break;
    case H_NEEDS:   b = {"NEEDS", "YOU",   C_ORANGE,  0x0000, sprite_bot_ask}; break;
    case H_LIMITED: b = {"RATE",  "LIMIT", C_MAGENTA, 0x0000, sprite_bot_limited}; break;
    default:        b = {"OUT",   "AGE",   C_RED,     C_TXT,  sprite_bot_outage}; break;
  }
  if (head == H_WORKING) b.bg = stateColor565();       // follows the LED hue exactly
  if (stale()) { b.bg = C_PANEL; b.fg = C_DIM; }
  if (ack && (head == H_DONE || head == H_NEEDS)) { b.bg = C_PANEL; b.fg = head == H_DONE ? C_AMBER : C_ORANGE; }
  return b;
}

void bandPortrait() {                      // 172 x 60 across the top
  BandStyle b = bandStyle();
  cv->fillRect(0, 0, W(), 76, b.bg);
  if (outageMajor() && head != H_OUTAGE) cv->fillRect(0, 70, W(), 6, C_RED);
  {
    const uint16_t *spr = b.spr; int sy = 4;
    if (head == H_WORKING) {
      spr = ((millis() / 380) & 1) ? sprite_bot_working2 : sprite_bot_working;
      sy += (int)(1.5f + 1.5f * sinf((millis() % 2400) / 2400.0f * 6.2831853f));
    }
    drawSprite(4, sy, spr);
  }
  if (b.l2[0]) { textAt(74, 14, b.l1, 3, b.fg); textAt(74, 40, b.l2, 3, b.fg); }
  else         { textAt(74, 26, b.l1, 3, b.fg); }
  if (stale()) textAt(W() - 20, 4, "?", 2, C_DIM);
  if (S.n > 1) { char s[6]; snprintf(s, sizeof s, "x%d", S.n); textRight(56, s, 2, b.fg, 4); }
}

#define LB_W 112                           // landscape band width
void bandLandscape() {                     // 112 x 172 down the left side
  BandStyle b = bandStyle();
  cv->fillRect(0, 0, LB_W, H(), b.bg);
  if (outageMajor() && head != H_OUTAGE) cv->fillRect(0, H() - 6, LB_W, 6, C_RED);
  int spriteY = 2;
  const uint16_t *spr = b.spr;
  if (head == H_WORKING) {                              // two-frame keystroke plus a slow bob
    spr = ((millis() / 380) & 1) ? sprite_bot_working2 : sprite_bot_working;
    spriteY += (int)(1.5f + 1.5f * sinf((millis() % 2400) / 2400.0f * 6.2831853f));
  }
  drawSprite((LB_W - SPRITE_SZ) / 2, spriteY, spr);
  if (b.l2[0]) { textCenteredIn(0, LB_W, 70, b.l1, 3, b.fg); textCenteredIn(0, LB_W, 96, b.l2, 3, b.fg); }
  else         { textCenteredIn(0, LB_W, 82, b.l1, 3, b.fg); }
  if (stale()) textAt(LB_W - 18, 4, "?", 2, C_DIM);
  if (S.n > 1) { char c[6]; snprintf(c, sizeof c, "x%d", S.n); textAt(4, 4, c, 2, b.fg); }
  uint16_t sub = b.fg == 0x0000 ? 0x0000 : C_DIM;
  char s[16];
  if (S.model[0]) marquee(2, H() - 44, LB_W - 4, S.model, 2, sub, b.bg, LB_W);
  int a = ageSec();
  bool showAge = a >= 60;                       // fresh data needs no timestamp
  s[0] = 0;
  if (showAge) {
    if (a < 3600) snprintf(s, sizeof s, "%dm", a / 60); else snprintf(s, sizeof s, "%dh", a / 3600);
    textAt(LB_W - 4 - textW(s, 2), H() - 22, s, 2, sub);
  }
  int effW = showAge ? LB_W - 8 - textW(s, 2) : LB_W - 4;
  if (S.eff[0]) marquee(2, H() - 22, effW, S.eff, 2, sub, b.bg, LB_W);

}

// --- gauges ----------------------------------------------------------------------------
void gaugePortrait(int y, const char *label, int pct, const char *inside) {   // 58 px tall
  uint16_t col = dimIf(pctColor(pct));
  char num[8];
  textAt(6, y + 12, label, 2, C_DIM);
  if (pct >= 0) snprintf(num, sizeof num, "%d%%", pct); else strcpy(num, "--");
  textRight(y, num, 4, col);
  bar(6, y + 34, W() - 12, 20, pct, col, inside);
}
void gaugeLandscape(int y, const char *label, int pct, const char *inside) {  // 46 px tall, right column
  int x = LB_W + 8, w = W() - x - 6;
  uint16_t col = dimIf(pctColor(pct));
  char num[8];
  textAt(x, y + 4, label, 2, C_DIM);
  if (pct >= 0) snprintf(num, sizeof num, "%d%%", pct); else strcpy(num, "--");
  textRight(y, num, 3, col);
  bar(x, y + 26, w, 16, pct, col, inside);
}

const char *compName() { return haveLink && S.comp[0] ? S.comp : (netComp[0] ? netComp : "API"); }
const char *healthText(uint16_t *col) {
  static char b[20];
  const char *lvl;
  if (!strcmp(apiOut(), "none"))          { *col = C_GREEN;  return NULL; }
  else if (!strcmp(apiOut(), "minor"))    { *col = C_ORANGE; lvl = "DEGRADED"; }
  else if (!strcmp(apiOut(), "major"))    { *col = C_RED;    lvl = "OUTAGE"; }
  else if (!strcmp(apiOut(), "critical")) { *col = C_RED;    lvl = "CRITICAL"; }
  else if (haveLink)                      { *col = C_DIM;    lvl = "UNKNOWN"; }
  else                                    { *col = C_DIM;    return NULL; }
  snprintf(b, sizeof b, "%s %s", compName(), lvl);
  return b;
}
// One dim line for the gauges that are not worth the space. Nothing is decided at 31%.
void gaugesCompact(int x, int y, bool skipCtx, bool skipH5, bool skipWk) {
  char b[40]; b[0] = 0;
  char part[14];
  if (!skipCtx && S.ctx >= 0) { snprintf(part, sizeof part, "ctx%d ", S.ctx); strlcat(b, part, sizeof b); }
  if (!skipH5 && S.h5 >= 0)   { snprintf(part, sizeof part, "5h%d ", S.h5);    strlcat(b, part, sizeof b); }
  if (!skipWk && S.wk >= 0)   { snprintf(part, sizeof part, "wk%d", S.wk);     strlcat(b, part, sizeof b); }
  if (b[0]) textAt(x, y, b, 2, C_DIM);
}

// Weekly burn against a linear pace. The diagonal is where you would be if you spent the
// week evenly; the line is where you actually are. Above the diagonal means burning fast.
void sparkline(int x, int y, int w, int h) {
  cv->drawFastHLine(x, y + h, w, C_PANEL);
  for (int i = 0; i <= w; i += 6)                       // the even-burn reference
    cv->drawPixel(x + i, y + h - (i * h) / w, C_PANEL);
  if (S.nhist < 2) {
    textAt(x, y + h / 2 - 8, "collecting...", 2, C_PANEL);
    return;
  }
  int prevX = x, prevY = y + h - (S.hist[0] * h) / 100;
  for (int i = 1; i < S.nhist; i++) {
    int px = x + (i * w) / (S.nhist - 1);
    int py = y + h - (S.hist[i] * h) / 100;
    cv->drawLine(prevX, prevY, px, py, dimIf(pctColor(S.hist[i])));
    prevX = px; prevY = py;
  }
  cv->fillCircle(prevX, prevY, 2, dimIf(pctColor(S.hist[S.nhist - 1])));
}

void healthRow(int x, int y) {
  uint16_t ac; const char *api = healthText(&ac);
  if (!api) return;
  cv->fillTriangle(x + 2, y + 14, x + 8, y + 2, x + 14, y + 14, ac);
  textAt(x + 22, y, api, 2, ac);
}

// --- pages --------------------------------------------------------------------------------
void pageOverview() {
  char r5[12], rw[12];
  fmtRemaining(S.h5m, r5, sizeof r5);
  fmtRemaining(S.wkm, rw, sizeof rw);
  if (landscape()) {
    bool hotCtx = S.ctx >= S.quiet, hotH5 = S.h5 >= S.quiet, hotWk = S.wk >= S.quiet;
    int y = 4;
    if (hotCtx) { gaugeLandscape(y, "CTX",  S.ctx, "");  y += 46; }
    if (hotH5)  { gaugeLandscape(y, "5HR",  S.h5,  r5);  y += 46; }
    if (hotWk)  { gaugeLandscape(y, "WEEK", S.wk,  rw);  y += 46; }
    if (hotCtx || hotH5 || hotWk) {
      gaugesCompact(LB_W + 8, y + 2, hotCtx, hotH5, hotWk);
    } else {
      // Nothing needs a decision, so spend the space on the one thing that might:
      // whether this week is running ahead of an even burn.
      gaugesCompact(LB_W + 8, 6, false, false, false);
      char b[20];
      if (S.pace != 999) {
        const char *word = S.pace > 8 ? "ahead" : S.pace < -8 ? "behind" : "on pace";
        uint16_t pc = S.pace > 20 ? C_ORANGE : S.pace > 8 ? C_AMBER : C_GREEN;
        if (S.pace > 8 || S.pace < -8) snprintf(b, sizeof b, "%+d%% %s", S.pace, word);
        else                            snprintf(b, sizeof b, "%s", word);
        textAt(LB_W + 8, 28, "WEEK", 2, C_DIM);
        textAt(LB_W + 8 + textW("WEEK ", 2), 28, b, 2, pc);
      }
      sparkline(LB_W + 8, 52, W() - LB_W - 16, 70);
    }
    uint16_t c;
    if (healthText(&c)) healthRow(LB_W + 6, 148);
    else {
      marquee(LB_W + 8, 148, W() - LB_W - 14, S.dir, 2, C_DIM);
    }
    bandLandscape();
  } else {
    gaugePortrait(84,  "CTX",  S.ctx, "");
    gaugePortrait(142, "5HR",  S.h5,  r5);
    gaugePortrait(200, "WEEK", S.wk,  rw);
    healthRow(6, 258);
    char b[16];
    marquee(6, H() - 42, W() - 12, S.dir, 2, C_DIM);
    bandPortrait();
    snprintf(b, sizeof b, "%.13s", S.model); textAt(6, H() - 20, b, 2, C_DIM);
    textRight(H() - 20, S.eff, 2, C_DIM);
  }
}

void limitBlock(int x0, int w, int y, const char *title, int pct, const char *reset, int mins, bool twoLine) {
  char b[24], r[12];
  fmtRemaining(mins, r, sizeof r);
  textAt(x0 + 6, y, title, 2, C_DIM);
  snprintf(b, sizeof b, "%d%%", pct < 0 ? 0 : pct);
  bigNumber(x0, w, y + 56, b, dimIf(pctColor(pct)));
  if (twoLine) {
    textCenteredIn(x0, w, y + 68, reset[0] ? reset : "--", 2, C_TXT);
    textCenteredIn(x0, w, y + 88, r, 2, C_DIM);
    bar(x0 + 6, y + 110, w - 12, 16, pct, dimIf(pctColor(pct)), "");
  } else {
    snprintf(b, sizeof b, "%s %s", reset[0] ? reset : "--", r);
    textCenteredIn(x0, w, y + 68, b, 2, C_TXT);
    bar(x0 + 6, y + 92, w - 12, 16, pct, dimIf(pctColor(pct)), "");
  }
}

void pageLimits() {
  char b[28];
  if (landscape()) {
    int half = W() / 2;
    limitBlock(0, half, 4, "5-HOUR", S.h5, S.h5r, S.h5m, true);
    limitBlock(half, half, 4, "WEEKLY", S.wk, S.wkr, S.wkm, true);
    cv->drawFastVLine(half, 4, 126, C_PANEL);
    cv->drawFastHLine(6, 138, W() - 12, C_PANEL);
    snprintf(b, sizeof b, "$%.2f ctx %d%%", S.cost, S.ctx < 0 ? 0 : S.ctx);
    textAt(6, 150, b, 2, C_DIM);
    snprintf(b, sizeof b, "%.10s", S.dir[0] ? S.dir : S.model);
    textRight(150, b, 2, C_DIM);
  } else {
    limitBlock(0, W(), 6, "5-HOUR", S.h5, S.h5r, S.h5m, false);
    cv->drawFastHLine(6, 126, W() - 12, C_PANEL);
    limitBlock(0, W(), 134, "WEEKLY", S.wk, S.wkr, S.wkm, true);
    cv->drawFastHLine(6, 266, W() - 12, C_PANEL);
    snprintf(b, sizeof b, "$%.2f  ctx %d%%", S.cost, S.ctx < 0 ? 0 : S.ctx);
    textAt(6, 276, b, 2, C_DIM);
    snprintf(b, sizeof b, "%.14s", S.dir[0] ? S.dir : S.model);
    textAt(6, 298, b, 2, C_DIM);
  }
}

void fmtK(int k, char *b, size_t n) {
  if (k >= 1000) snprintf(b, n, "%.1fM", k / 1000.0f);
  else if (k > 0) snprintf(b, n, "%dk", k);
  else snprintf(b, n, "<1k");            // never report a real count as "0k"
}

void pageStats() {                                   // the useful part of /usage
  char b[28], t1[12], t2[12];
  fmtK(S.tin, t1, sizeof t1); fmtK(S.tout, t2, sizeof t2);
  if (landscape()) {
    int half = W() / 2;
    textAt(6, 6, "SESSION", 2, C_DIM);
    snprintf(b, sizeof b, "$%.2f", S.cost);                     textAt(6, 28, b, 3, C_TXT);
    snprintf(b, sizeof b, "%dh%02dm wall", S.dur / 60, S.dur % 60); textAt(6, 60, b, 2, C_DIM);
    snprintf(b, sizeof b, "%dh%02dm api", S.api / 60, S.api % 60);  textAt(6, 80, b, 2, C_DIM);
    snprintf(b, sizeof b, "+%d", S.la);  textAt(6, 106, b, 2, C_GREEN);
    snprintf(b, sizeof b, "-%d", S.lr);  textAt(6 + textW("+99999", 2) + 6, 106, b, 2, C_RED);
    textAt(6, 126, "lines", 2, C_DIM);
    cv->drawFastVLine(half, 6, 140, C_PANEL);
    int x = half + 8;
    textAt(x, 6, "TOKENS", 2, C_DIM);
    snprintf(b, sizeof b, "in  %s", t1);  textAt(x, 28, b, 2, C_TXT);
    snprintf(b, sizeof b, "out %s", t2);  textAt(x, 48, b, 2, C_TXT);
    textAt(x, 78, "CACHE", 2, C_DIM);
    if (S.ch >= 0) snprintf(b, sizeof b, "%d%% hits", S.ch); else strcpy(b, "no data");
    textAt(x, 98, b, 2, S.ch >= 70 ? C_GREEN : S.ch >= 0 ? C_AMBER : C_DIM);
    textAt(x, 118, S.cw ? "warm" : "cold", 2, S.cw ? C_GREEN : C_DIM);
    snprintf(b, sizeof b, "ctx %d%%  cc %s", S.ctx < 0 ? 0 : S.ctx, S.ver); textAt(6, H() - 20, b, 2, C_DIM);
  } else {
    textAt(6, 6, "SESSION", 2, C_DIM);
    snprintf(b, sizeof b, "$%.2f", S.cost);                     textAt(6, 26, b, 3, C_TXT);
    snprintf(b, sizeof b, "%dh%02dm wall", S.dur / 60, S.dur % 60); textAt(6, 56, b, 2, C_DIM);
    snprintf(b, sizeof b, "%dh%02dm api", S.api / 60, S.api % 60);  textAt(6, 76, b, 2, C_DIM);
    snprintf(b, sizeof b, "+%d", S.la);  textAt(6, 100, b, 2, C_GREEN);
    snprintf(b, sizeof b, "-%d", S.lr);  textAt(6 + textW("+99999", 2) + 6, 100, b, 2, C_RED);
    textAt(6, 120, "lines changed", 2, C_DIM);
    cv->drawFastHLine(6, 140, W() - 12, C_PANEL);
    textAt(6, 148, "TOKENS", 2, C_DIM);
    snprintf(b, sizeof b, "in  %s", t1);  textAt(6, 168, b, 2, C_TXT);
    snprintf(b, sizeof b, "out %s", t2);  textAt(6, 188, b, 2, C_TXT);
    cv->drawFastHLine(6, 212, W() - 12, C_PANEL);
    textAt(6, 220, "CACHE", 2, C_DIM);
    if (S.ch >= 0) snprintf(b, sizeof b, "%d%% hits", S.ch); else strcpy(b, "no data");
    textAt(6, 240, b, 2, S.ch >= 70 ? C_GREEN : S.ch >= 0 ? C_AMBER : C_DIM);
    textAt(6, 260, S.cw ? "warm" : "cold", 2, S.cw ? C_GREEN : C_DIM);
    snprintf(b, sizeof b, "ctx %d%%  v%s", S.ctx < 0 ? 0 : S.ctx, S.ver); textAt(6, H() - 20, b, 2, C_DIM);
  }
}

void pageApi() {
  const char *t; uint16_t c;
  if (!strcmp(apiOut(), "none"))          { t = "ALL GOOD";  c = C_GREEN; }
  else if (!strcmp(apiOut(), "minor"))    { t = "DEGRADED";  c = C_ORANGE; }
  else if (!strcmp(apiOut(), "major"))    { t = "OUTAGE";    c = C_RED; }
  else if (!strcmp(apiOut(), "critical")) { t = "CRITICAL";  c = C_RED; }
  else                                 { t = "UNKNOWN";   c = C_DIM; }
  bool bad = strcmp(apiOut(), "none") && strcmp(apiOut(), "unknown");
  drawClaudeMark(30, 30, 46, bad ? c : C_CLAUDE);
  char title[20]; snprintf(title, sizeof title, "CLAUDE %s", bad ? compName() : "API+CODE");
  const char *other = haveLink ? S.other : netOther;
  char o[92]; snprintf(o, sizeof o, "Other: %s", other);
  if (landscape()) {
    textAt(60, 8, title, 2, C_DIM);
    textAt(60, 30, t, 3, c);
    cv->drawFastHLine(6, 62, W() - 12, C_PANEL);
    int lines = 1;
    if (S.inc[0]) lines = wrapText(6, 70, S.inc, 25, C_AMBER, 3); else textAt(6, 70, "No incidents", 2, C_DIM);
    if (other[0]) wrapText(6, 70 + lines * 18 + 6, o, 25, C_AMBER, 2);
    textRight(H() - 20, "BOOT: next", 2, C_DIM);
  } else {
    textAt(60, 8, "CLAUDE", 2, C_DIM);
    textAt(60, 30, bad ? compName() : "API+CODE", 2, C_DIM);
    textAt(6, 66, t, 3, c);
    cv->drawFastHLine(6, 96, W() - 12, C_PANEL);
    int lines = 1;
    if (S.inc[0]) lines = wrapText(6, 106, S.inc, 14, C_AMBER, 5); else textAt(6, 106, "No incidents", 2, C_DIM);
    if (other[0]) wrapText(6, 106 + lines * 18 + 6, o, 14, C_AMBER, 3);
    textAt(6, H() - 20, "BOOT: next", 2, C_DIM);
  }
}

void pageAbout() {
  char b[28];
  int lx = 6, rx0 = landscape() ? 160 : 6;
  int ly = landscape() ? 40 : 64, ry = landscape() ? 40 : 180;
  if (landscape()) { textAt(6, 6, "CLAUDE STATUS", 3, C_TXT); drawClaudeMark(W() - 24, H() - 24, 36); }
  else { textAt(6, 6, "CLAUDE", 3, C_TXT); textAt(6, 32, "STATUS", 3, C_TXT); drawClaudeMark(W() - 26, 28, 40); }
  snprintf(b, sizeof b, "fw %s", FW_VERSION);                textAt(lx, ly, b, 2, C_DIM);
  snprintf(b, sizeof b, "%s %s", clockStr(), wifiUp ? "wifi" : "usb"); textAt(lx, ly + 20, b, 2, C_DIM);
  snprintf(b, sizeof b, "up %lum", millis() / 60000UL);       textAt(lx, ly + 40, b, 2, C_DIM);
  textAt(lx, ly + 60, nightMode() ? "night mode" : "day mode", 2, C_DIM);
  snprintf(b, sizeof b, "sessions %d", S.n);                  textAt(lx, ly + 80, b, 2, C_DIM);
  if (!landscape()) cv->drawFastHLine(6, 170, W() - 12, C_PANEL);
  textAt(rx0, ry, "BOOT BUTTON", 2, C_TXT);
  textAt(rx0, ry + 22, "press: next", 2, C_DIM);
  textAt(rx0, ry + 42, "hold: night", 2, C_DIM);
  textAt(rx0, ry + 62, "hold 3s: turn", 2, C_DIM);
  textAt(6, H() - 20, S.model, 2, C_DIM);
}

// --- screensaver: starfield + bouncing mascot + clock ---------------------------------------
#define NSTARS 48
struct Star { int16_t x, y; uint8_t speed, bright; };
Star stars[NSTARS];
bool starsInit = false;
float botX = 20, botY = 20, botVX = 1.3f, botVY = 0.9f;
unsigned long lastSaverFrame = 0;

bool restingState() { return head == H_IDLE || head == H_DONE || head == H_NOLINK; }
bool saverActive() { return restingState() && !toast[0] && millis() - stateChangedAt > SAVER_MS; }
bool cycling() { return restingState() && !toast[0] && !saverActive() && millis() - stateChangedAt > CYCLE_MS; }

void initStars() {
  for (int i = 0; i < NSTARS; i++) {
    stars[i] = { (int16_t)random(W()), (int16_t)random(H()), (uint8_t)random(1, 4), (uint8_t)random(60, 255) };
  }
  botX = random(W() - SPRITE_SZ); botY = random(32, H() - 26 - SPRITE_SZ);
  starsInit = true;
}

void pageSaver() {
  if (!starsInit) initStars();
  bool land = landscape();
  // stars drift toward the bottom-left corner in landscape, straight down in portrait
  for (int i = 0; i < NSTARS; i++) {
    Star &st = stars[i];
    if (land) st.x -= st.speed; else st.y += st.speed;
    if (st.x < 0 || st.y >= H()) {
      st.x = land ? W() - 1 : random(W()); st.y = land ? random(H()) : 0;
      st.speed = random(1, 4); st.bright = random(60, 255);
    }
    uint8_t twinkle = st.bright - (uint8_t)random(0, 40);
    uint16_t c = ((twinkle & 0xF8) << 8) | ((twinkle & 0xFC) << 3) | (twinkle >> 3);
    if (st.speed == 3) cv->drawPixel(st.x + 1, st.y, c);   // fast stars get a 2px streak
    cv->drawPixel(st.x, st.y, c);
  }
  // mascot bounces off the walls
  botX += botVX; botY += botVY;                        // keep clear of the top and bottom text strips
  int top = 32, bottom = H() - 26 - SPRITE_SZ;
  if (top >= bottom) { top = 0; bottom = H() - SPRITE_SZ; }
  if (botX <= 0 || botX >= W() - SPRITE_SZ) { botVX = -botVX; botX = constrain(botX, 0, W() - SPRITE_SZ); }
  if (botY <= top || botY >= bottom) { botVY = -botVY; botY = constrain(botY, top, bottom); }
  // Every ~11 s he stops tumbling and waves at you for a moment.
  unsigned long cyc = millis() % 11000UL;
  bool waving = cyc < 1800UL;
  const uint16_t *spr;
  if (head == H_NOLINK)      spr = sprite_bot_nolink;
  else if (head == H_DONE)   spr = sprite_bot_done;
  else if (waving)           spr = sprite_bot_done;
  else                       spr = sprite_bot_float;
  int sy = (int)botY + (int)(2.0f * sinf((millis() % 3000) / 3000.0f * 6.2831853f));
  drawSprite((int)botX, sy, spr, 1, botVX < 0);          // face the way he is drifting
  if (head == H_IDLE && !waving && ((millis() / 700) & 1))
    textAt((int)botX + SPRITE_SZ - 6, sy - 8, "z", 2, C_BLUE);

  // clock + status strip
  { const char *ck = clockStr(); textAt(W() - 6 - textW(ck, 3), 6, ck, 3, C_TXT); }
  const char *word = head == H_DONE ? "ready" : head == H_NOLINK ? "no link" : "idle";
  textAt(6, 6, word, 2, head == H_DONE ? C_AMBER : C_DIM);
  char b[40];
  snprintf(b, sizeof b, "WEEK %d%%   5HR %d%%", S.wk < 0 ? 0 : S.wk, S.h5 < 0 ? 0 : S.h5);
  textAt(6, H() - 20, b, 2, C_DIM);
  if (S.model[0]) textAt(W() - 6 - textW(S.model, 2), 32, S.model, 2, C_DIM);
}

void drawToast() {
  if (!toast[0] || millis() - toastAt > TOAST_MS) { toast[0] = 0; return; }
  uint8_t sz = textW(toast, 3) + 24 <= W() ? 3 : 2;
  int tw = textW(toast, sz) + 24, y = H() / 2 - 22;
  cv->fillRoundRect((W() - tw) / 2, y, tw, 44, 8, C_TXT);
  textCentered(y + (sz == 3 ? 10 : 14), toast, sz, 0x0000);
}

void render() {
  marqueeActive = false;
  cv->fillScreen(C_BG);
  if (saverActive()) pageSaver();
  else switch (page) {
    case PG_LIMITS: pageLimits(); break;
    case PG_STATS:  pageStats();  break;
    case PG_API:    pageApi();    break;
    case PG_ABOUT:  pageAbout();  break;
    default:        pageOverview();
  }
  drawToast();
  tft.drawRGBBitmap(0, 0, cv->getBuffer(), W(), H());
}

// --- LED language ------------------------------------------------------------------------------
// Rainbow = Claude is working. Amber glow = waiting on you (steady for "your turn",
// insistent pulse for a permission/question). Magenta = throttled, red = outage,
// warm white = asleep. Night cap keeps it calm.
void setLed(uint8_t r, uint8_t g, uint8_t b, float level) {
  led.setBrightness(nightMode() ? 30 : 140);
  level = constrain(level, 0.0f, 1.0f);
  led.setPixelColor(0, led.Color(led.gamma8(r * level), led.gamma8(g * level), led.gamma8(b * level)));
  led.show();
}
float breathe(unsigned long period, float lo, float hi) {
  float t = (millis() % period) / (float)period;
  return lo + (hi - lo) * (0.5f - 0.5f * cosf(t * 6.2831853f));
}

void updateLed() {
  unsigned long t = millis() - stateChangedAt, now = millis();
  bool night = nightMode(), quiet = ack || stale();
  uint8_t r, g, b; stateRGB(r, g, b);                   // same colour the band is using
  float lv = 0;
  switch (head) {
    case H_WORKING: lv = 0.6f; break;                   // rainbow, hue from stateRGB
    case H_DONE:                                        // amber glow: brief hello, then steady
      if (quiet) lv = 0.15f;
      else if (t < 720) lv = ((t / 120) % 2 == 0) ? 1.0f : 0.1f;
      else if (t < 120000UL) lv = 0.5f;
      else lv = 0.25f;
      break;
    case H_NEEDS:                                       // orange, insistent breathe
      if (quiet) lv = 0.15f;
      else lv = breathe(night ? 1500 : 1000, 0.15f, 1.0f);
      break;
    case H_LIMITED: lv = breathe(4000, 0.1f, 0.4f); break;
    case H_IDLE:
      if (saverActive() && !night) {                    // slow dim aurora while asleep
        uint32_t c = led.ColorHSV((uint16_t)((now % 20000UL) * 65535UL / 20000UL), 255, 255);
        r = c >> 16; g = c >> 8; b = c;
        lv = 0.12f;
        setLed(r, g, b, lv);
        return;
      }
      lv = night ? 0.0f : 0.25f;                        // dim grey pilot light, matches the band
      break;
    case H_OUTAGE:  lv = breathe(2000, 0.1f, 0.5f); break;
    default:        lv = (!night && now % 5000 < 2000) ? 0.5f : 0.0f; break;   // grey, slow blink
  }
  // Overlay the API's health on whatever the session state is doing, matching the colours
  // the screen uses: orange for degraded, red for an outage. Degraded ticks half as often
  // and dimmer, so it reads as news rather than an alarm.
  if (outageMajor() && head != H_OUTAGE && now % 3000 < 200) { r = 255; g = 0;   b = 0; lv = 0.6f; }
  else if (apiDegraded() && now % 6000 < 150)                { r = 255; g = 129; b = 0; lv = 0.35f; }
  setLed(r, g, b, lv);
}

// --- backlight / screensaver ---------------------------------------------------------------------
void updateBacklight() {
  unsigned long t = millis() - stateChangedAt;
  uint8_t full = nightMode() ? 60 : 180, target = full;
  if (saverActive()) target = full * 6 / 10;
  if (head == H_IDLE && t > IDLE_DIM_MS && !saverActive()) target = 30;
  if (head == H_NOLINK && t > NOLINK_DIM_MS) target = 20;
  if (page != PG_OVERVIEW || cycling() || (toast[0] && millis() - toastAt < TOAST_MS)) target = full;
  if (target != backlight) { backlight = target; analogWrite(PIN_BL, backlight); }
}
bool screenDimmed() { return backlight <= 30; }

// --- button ------------------------------------------------------------------------------------------
unsigned long btnDownAt = 0, btnLastChange = 0;
bool btnDown = false, longFired = false, rotateFired = false, nightOvSnap = false, manualNightSnap = false;

void showToast(const char *s) { strlcpy(toast, s, sizeof toast); toastAt = millis(); dirty = true; }

void applyRotation() {
  tft.setRotation(rotation);
  if (!framebuf) {
    framebuf = (uint16_t *)malloc((size_t)LCD_W * LCD_H * 2);
    if (!framebuf) {                                   // nothing sane to draw into; restart clean
      Serial.println("{\"fatal\":\"framebuffer alloc failed\"}");
      Serial.flush(); delay(200); ESP.restart();
    }
  }
  if (cv) delete cv;                                   // the canvas object only; the buffer is ours
  cv = new SharedCanvas(tft.width(), tft.height(), framebuf);
  Serial.printf("{\"rotation\":%d,\"heap\":%u}\n", rotation, (unsigned)ESP.getFreeHeap());
}

void shortPress() {
  if (screenDimmed() || saverActive() || cycling()) {                     // wake only
    stateChangedAt = millis(); starsInit = false; page = PG_OVERVIEW; dirty = true; return;
  }
  if (alertActive()) { ack = true; showToast("OK"); return; }
  page = (Page)((page + 1) % PG_COUNT);
  if (page == PG_API) Serial.println("{\"cmd\":\"poll\"}");
  pageChangedAt = millis(); dirty = true;
}

void pollButton() {
  bool down = digitalRead(PIN_BTN) == LOW;
  unsigned long now = millis();
  if (down != btnDown && now - btnLastChange > 30) {
    btnDown = down; btnLastChange = now;
    if (down) { btnDownAt = now; longFired = rotateFired = false; nightOvSnap = nightOverride; manualNightSnap = manualNight; }
    else if (!longFired && !rotateFired) shortPress();
  }
  if (btnDown && !longFired && now - btnDownAt >= BTN_LONG_MS) {
    longFired = true;
    nightOverride = true; manualNight = !nightMode();
    showToast(manualNight ? "NIGHT" : "DAY");
  }
  if (btnDown && !rotateFired && now - btnDownAt >= BTN_ROTATE_MS) {
    rotateFired = true;
    nightOverride = nightOvSnap; manualNight = manualNightSnap;   // undo the night toggle that fired on the way here
    rotation = (rotation + 1) % 4;
    prefs.putUChar("rot2", rotation);
    applyRotation();
    showToast("TURN");
  }
}

// --- serial protocol -----------------------------------------------------------------------------------
void copyStr(char *dst, size_t n, JsonVariantConst v, const char *dflt) {
  const char *s = v.as<const char *>();
  strlcpy(dst, s ? s : dflt, n);
}

// host commands: shot (dump framebuffer), tap / hold / rotate (simulate the button)
void handleCommand(const char *cmd) {
  if (!strcmp(cmd, "net")) { netReport(); return; }
  if (!strcmp(cmd, "apipoll")) { netForcePoll(); return; }
  if (!strcmp(cmd, "shot")) {
    render();
    size_t len = (size_t)W() * H() * 2;
    Serial.printf("{\"shot\":%d,\"h\":%d,\"len\":%u,\"head\":%d,\"page\":%d,\"ack\":%d}\n", W(), H(), (unsigned)len, (int)head, (int)page, (int)ack);
    Serial.flush();
    const uint8_t *buf = (const uint8_t *)cv->getBuffer();
    for (size_t off = 0; off < len; off += 1024) Serial.write(buf + off, min((size_t)1024, len - off));
    Serial.flush();
  } else if (!strcmp(cmd, "tap")) { shortPress(); }
  else if (!strcmp(cmd, "hold")) { nightOverride = true; manualNight = !nightMode(); showToast(manualNight ? "NIGHT" : "DAY"); }
  else if (!strcmp(cmd, "rotate")) { rotation = (rotation + 1) % 4; prefs.putUChar("rot2", rotation); applyRotation(); showToast("TURN"); }
  else if (!strcmp(cmd, "sleep")) { stateChangedAt = millis() - SAVER_MS - 1000UL; }
  else if (!strcmp(cmd, "cycle")) { stateChangedAt = millis() - CYCLE_MS - 1000UL; pageChangedAt = 0; }
  dirty = true;
}

void handleLine(const char *line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) { Serial.println("{\"err\":\"json\"}"); return; }
  const char *cmd = doc["cmd"] | (const char *)NULL;
  if (cmd && !strcmp(cmd, "wifi")) { netCommand(doc); return; }
  if (cmd) { handleCommand(cmd); return; }
  long ts = doc["ts"] | 0L;
  if (!ts) { Serial.printf("{\"err\":\"no ts\",\"keys\":%u}\n", (unsigned)doc.size()); return; }
  S.ctx = doc["ctx"] | -1; S.h5 = doc["h5"] | -1; S.wk = doc["wk"] | -1;
  S.h5m = doc["h5m"] | -1; S.wkm = doc["wkm"] | -1;
  S.n = doc["n"] | 0; S.age = doc["age"] | -1;
  S.cost = doc["cost"] | 0.0f;
  S.lim = doc["lim"] | false;
  S.dur = doc["dur"] | 0; S.api = doc["api"] | 0; S.la = doc["la"] | 0; S.lr = doc["lr"] | 0;
  S.tin = doc["tin"] | 0; S.tout = doc["tout"] | 0; S.ch = doc["ch"] | -1; S.cw = doc["cw"] | false;
  S.pace = doc["pace"] | 999;
  S.quiet = doc["quiet"] | 70;
  S.nhist = 0;
  for (JsonVariantConst v : doc["hist"].as<JsonArrayConst>()) {
    if (S.nhist >= (int)sizeof S.hist) break;
    S.hist[S.nhist++] = (uint8_t)constrain(v.as<int>(), 0, 100);
  }
  copyStr(S.ver, sizeof S.ver, doc["ver"], "");
  bool hostNight = doc["night"] | false;
  if (hostNight != hostNightLast) { hostNightLast = hostNight; nightOverride = false; }   // window boundary clears manual override
  S.night = hostNight;
  S.ts = ts;
  copyStr(S.h5r, sizeof S.h5r, doc["h5r"], "");
  copyStr(S.wkr, sizeof S.wkr, doc["wkr"], "");
  copyStr(S.hm, sizeof S.hm, doc["hm"], "");
  copyStr(S.st, sizeof S.st, doc["st"], "idle");
  copyStr(S.out, sizeof S.out, doc["out"], "unknown");
  copyStr(S.inc, sizeof S.inc, doc["inc"], "");
  copyStr(S.other, sizeof S.other, doc["other"], "");
  copyStr(S.comp, sizeof S.comp, doc["comp"], "");
  char prevDir[24]; strlcpy(prevDir, S.dir, sizeof prevDir);
  copyStr(S.model, sizeof S.model, doc["model"], "");
  copyStr(S.eff, sizeof S.eff, doc["eff"], "");
  copyStr(S.dir, sizeof S.dir, doc["dir"], "");
  // Announce a session switch, but only once things settle: with two busy sessions the
  // followed one can alternate every few seconds, and a toast each time is unreadable.
  if (prevDir[0] && S.dir[0] && strcmp(prevDir, S.dir)) {
    if (millis() - lastDirChange > 15000UL) { char t[16]; snprintf(t, sizeof t, "%.13s", S.dir); showToast(t); }
    lastDirChange = millis();
  }
  lastRx = rxAt = millis();
  haveLink = true;
  dirty = true;
  bleNotifyState();
  Serial.printf("{\"ok\":%ld,\"st\":\"%s\"}\n", S.ts, S.st);
}

// Reads whole lines into a fixed buffer, so there is no heap churn over weeks of uptime.
// An over-long line is dropped at the newline rather than truncated, so a partial payload
// can never be parsed as if it were complete.
void drainSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (rxLen && rxLen < sizeof rx) {
        rx[rxLen] = 0;
        strlcpy(transport, "usb", sizeof transport);
        handleLine(rx);
      }
      rxLen = 0;
    } else if (rxLen < sizeof rx - 1) {
      rx[rxLen++] = c;
    } else {
      rxLen = sizeof rx;           // too long: mark and discard at the newline
    }
  }
}

void setup() {
  Serial.setRxBufferSize(4096);
  Serial.setTxTimeoutMs(50);              // never block forever if the host stops reading
  Serial.begin(115200);
  esp_task_wdt_config_t wdt = { .timeout_ms = 30000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_reconfigure(&wdt);         // reboot if loop() stalls for 30 s
  esp_task_wdt_add(NULL);
  pinMode(PIN_BTN, INPUT_PULLUP);
  led.begin(); led.setBrightness(255); led.show();

  prefs.begin("claude", false);
  rotation = prefs.getUChar("rot2", DEFAULT_ROTATION) % 4;

  pinMode(PIN_BL, OUTPUT);
  analogWrite(PIN_BL, 180); backlight = 180;

  spi.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, PIN_CS);
  tft.init(LCD_W, LCD_H);
  tft.setSPISpeed(40000000);
  tft.invertDisplay(true);
  applyRotation();
  netBegin();
  stateChangedAt = millis();
  render();
  Serial.println("{\"hello\":\"claude_status\",\"fw\":\"" FW_VERSION "\"}");
}

void loop() {
  esp_task_wdt_reset();
  drainSerial();
  netLoop();
  if (haveLink && millis() - lastRx > LINK_TIMEOUT_MS) { haveLink = false; dirty = true; }

  head = computeHeadline();
  if (head != lastHead) {
    lastHead = head; stateChangedAt = millis(); ack = false; dirty = true;
    if (head == H_DONE || head == H_NEEDS || head == H_LIMITED || head == H_OUTAGE) page = PG_OVERVIEW;
  }
  if (cycling()) {
    if (millis() - pageChangedAt > CYCLE_PAGE_MS) { page = (Page)((page + 1) % PG_COUNT); pageChangedAt = millis(); dirty = true; }
  } else if (page != PG_OVERVIEW && millis() - pageChangedAt > PAGE_RETURN_MS) { page = PG_OVERVIEW; dirty = true; }

  pollButton();
  updateBacklight();
  updateLed();

  drainSerial();
  unsigned long frame = (saverActive() || marqueeActive || head == H_WORKING) ? 80UL : (toast[0] || (head == H_IDLE && page == PG_OVERVIEW)) ? 500UL : 10000UL;
  if (dirty || millis() - lastDraw > frame) { render(); lastDraw = millis(); dirty = false; }
  delay(10);
}
