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

#define FW_VERSION "9.5"

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
#define PIN_SD_CS 4   // TF slot; MOSI/SCLK are shared with the panel

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
#define DONE_REST_MS      (15UL * 60UL * 1000UL)  // an unanswered turn insists this long, then rests
#define SCREEN_OFF_MS     (10UL * 60UL * 1000UL)  // after 10 min at rest the panel goes dark
#define NOLINK_DIM_MS     (2UL * 60UL * 1000UL)
#define CACHE_WARN_MIN    10       // prompt cache this close to expiry is worth saying out loud
#define RL_STALE_S        600      // borrowed limits older than this are drawn as old news
#define HISTORY_POINTS    32       // must match HISTORY_POINTS in claude_status_daemon.py
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
  bool histAnchored = false;       // true: hist[] is the daemon's week buckets, so the diagonal means something
  uint8_t hist[40];
  struct Sess { char name[14]; char st; int wait; float cost; int ctx; } sess[4];
  int nsess = 0, nrows = 0, nblk = 0, blkw = 0, nwork = 0, nrdy = 0;
  int cxm = -1;                    // minutes until the prompt cache expires, -1 unknown
  int ctxt = 0;                    // context tokens (thousands) when there is no percentage
  int rlage = -1;                  // seconds since anyone asked for the limits, -1 = ours
  char ver[10] = "", cwin[8] = "";
  long ts = 0;
} S;

const char *clockStr();
const char *netIp();
extern uint32_t enrollCode;
bool enrollOpen();
void fmtK(int k, char *b, size_t n);
void sdBegin(); void sdLoop(); int sdLoadHistory(uint8_t *out, int maxPoints);
extern bool sdUp; extern uint32_t sdSizeMB;
void netBegin(); void netLoop(); void netForcePoll(); void netCommand(JsonDocument &doc); void netReport(); void bleNotifyState();
extern char netOut[12]; extern char netComp[8]; extern char netOther[84]; extern bool wifiUp; extern char transport[8];

struct BandStyle { const char *l1, *l2; uint16_t bg, fg; const uint16_t *spr; };
enum Headline { H_NOLINK, H_IDLE, H_WORKING, H_DONE, H_NEEDS, H_LIMITED, H_OUTAGE };
enum Page { PG_OVERVIEW, PG_SESSIONS, PG_BURN, PG_LIMITS, PG_STATS, PG_API, PG_ABOUT, PG_COUNT };

char rx[1024]; uint16_t rxLen = 0; bool rxSkip = false;
unsigned long lastRx = 0, rxAt = 0, stateChangedAt = 0;
// "act as though we have been resting long enough", for the sleep and cycle commands. It
// cannot be expressed by winding stateChangedAt back: millis() is under five minutes for
// the first five minutes of a boot, so the subtraction wrapped and the command did nothing.
bool forceSaver = false, forceCycle = false;
unsigned long lastDirChange = 0, pageChangedAt = 0, toastAt = 0, lastDraw = 0;
bool haveLink = false, dirty = true, ack = false, manualNight = false, nightOverride = false;
bool hostNightLast = false;
Headline head = H_NOLINK, lastHead = H_NOLINK;
Page page = PG_OVERVIEW;
char toast[16] = "";
uint8_t rotation = DEFAULT_ROTATION, backlight = 0;

// --- helpers ---------------------------------------------------------------------
int ageSec();
bool stale() { return S.ts != 0 && millis() - rxAt > STALE_MS; }   // data is old, whatever the transport
bool nightMode() { return nightOverride ? manualNight : S.night; }
int ageSec() { return S.age < 0 ? -1 : S.age + (int)((millis() - rxAt) / 1000); }
bool landscape() { return cv->width() > cv->height(); }
int W() { return cv->width(); }
int H() { return cv->height(); }

const char *apiOut() { return haveLink ? S.out : netOut; }
bool outageMajor() { return !strcmp(apiOut(), "major") || !strcmp(apiOut(), "critical"); }
bool apiDegraded() { return !strcmp(apiOut(), "minor"); }

Headline computeHeadline() {
  if (!haveLink && S.ts == 0) return H_NOLINK;        // nothing was ever received
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
    // The reset time has to read on the dark track, on the coloured fill, and when the
    // fill ends halfway through it. White on the track, black once the fill has fully
    // passed it, and a dark backing while the fill edge is somewhere inside the text.
    int tx = x + w - 4 - textW(inside, 2);
    int fillEnd = x + fw;
    uint16_t tc = C_TXT;
    if (fillEnd >= x + w - 2)  tc = 0x0000;
    else if (fillEnd > tx - 4) cv->fillRoundRect(tx - 4, y, x + w - (tx - 4), h, 4, C_PANEL);
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
  if (!haveLink && S.ts) {                             // last known state, not a live one
    uint8_t r, g, bl; stateRGB(r, g, bl);
    b.bg = rgb565(r / 3, g / 3, bl / 3);
    b.fg = C_DIM;
  }
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
  // No spare row at 76 px tall: the session badge goes amber instead, meaning one of
  // these sessions has finished and is waiting on you.
  if (S.n > 1) { char s[6]; snprintf(s, sizeof s, "x%d", S.n);
                 textRight(56, s, 2, (S.nrdy > 0 && head != H_DONE) ? C_AMBER : b.fg, 4); }
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
  // A finished session no longer takes the band from a working one, so say here that one
  // is waiting. Only under a one-word state: a two-line state already reaches this row.
  if (!b.l2[0] && S.nrdy > 0 && head != H_DONE) {
    char r[14]; snprintf(r, sizeof r, "%d ready", S.nrdy);
    textCenteredIn(0, LB_W, 108, r, 2, C_AMBER);
  }
  bool feedGone = !haveLink && S.ts;                  // the feed is gone; the numbers are not
  if (!feedGone) {
    if (stale()) textAt(LB_W - 18, 4, "?", 2, C_DIM);
    if (S.n > 1) { char c[6]; snprintf(c, sizeof c, "x%d", S.n); textAt(4, 4, c, 2, b.fg); }
  }
  uint16_t sub = b.fg == 0x0000 ? 0x0000 : C_DIM;
  char s[16];
  if (feedGone) {
    // The age takes the model row rather than a strip across the top: the sprite is
    // opaque from its first row, so anything drawn above it cut the helmet off.
    char w[16]; int a = ageSec();
    if (a < 60)        snprintf(w, sizeof w, "%ds old", a);
    else if (a < 3600) snprintf(w, sizeof w, "%dm old", a / 60);
    else               snprintf(w, sizeof w, "%dh old", a / 3600);
    textCenteredIn(0, LB_W, H() - 44, w, 2, C_AMBER);
  } else if (S.model[0]) {
    marquee(2, H() - 44, LB_W - 4, S.model, 2, sub, b.bg, LB_W);
  }
  int a = ageSec();
  bool showAge = a >= 60 && !feedGone;          // fresh data needs no timestamp; the amber row already carries it
  s[0] = 0;
  if (showAge) {
    if (a < 3600) snprintf(s, sizeof s, "%dm", a / 60); else snprintf(s, sizeof s, "%dh", a / 3600);
    textAt(LB_W - 4 - textW(s, 2), H() - 22, s, 2, sub);
  }
  int effW = showAge ? LB_W - 8 - textW(s, 2) : LB_W - 4;
  if (S.eff[0]) marquee(2, H() - 22, effW, S.eff, 2, sub, b.bg, LB_W);

}

// --- gauges ----------------------------------------------------------------------------
// The limits are account wide and may come from whichever window last asked. That is still
// this account's allowance, but it is not necessarily this minute's, so an old reading is
// drawn dim: visible, and visibly not fresh.
bool limitsOld(const char *label) {
  return S.rlage > RL_STALE_S && (!strcmp(label, "5HR") || !strcmp(label, "WEEK"));
}
void gaugePortrait(int y, const char *label, int pct, const char *inside) {   // 58 px tall
  uint16_t col = limitsOld(label) ? C_DIM : dimIf(pctColor(pct));
  char num[8];
  textAt(6, y + 12, label, 2, C_DIM);
  if (!strcmp(label, "CTX") && S.cwin[0])
    textAt(6 + textW(label, 2) + 8, y + 12, S.cwin, 2, C_DIM);
  if (pct >= 0)                                    snprintf(num, sizeof num, "%d%%", pct);
  else if (!strcmp(label, "CTX") && S.ctxt > 0)    fmtK(S.ctxt, num, sizeof num);
  else                                             strcpy(num, "--");
  textRight(y, num, 4, col);
  bar(6, y + 34, W() - 12, 20, pct, col, inside);
}
void gaugeLandscape(int y, const char *label, int pct, const char *inside) {  // 46 px tall, right column
  int x = LB_W + 8, w = W() - x - 6;
  uint16_t col = limitsOld(label) ? C_DIM : dimIf(pctColor(pct));
  char num[8];
  textAt(x, y + 4, label, 2, C_DIM);
  // A context percentage means nothing without the size of the window it is a percentage of.
  if (!strcmp(label, "CTX") && S.cwin[0])
    textAt(x + textW(label, 2) + 8, y + 4, S.cwin, 2, C_PANEL == 0 ? C_DIM : C_DIM);
  if (pct >= 0)                                    snprintf(num, sizeof num, "%d%%", pct);
  else if (!strcmp(label, "CTX") && S.ctxt > 0)    fmtK(S.ctxt, num, sizeof num);
  else                                             strcpy(num, "--");
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
//
// The x axis is the week, not the sample count. The daemon buckets the week into
// HISTORY_POINTS slots and stops at now, so nhist is how far into the week we are, and a
// point belongs at i/HISTORY_POINTS across. Dividing by nhist instead stretched a partial
// week across the full width, which put every point's x where a later point belonged and
// left the line below the diagonal no matter how hot the week was actually running.
//
// Samples read off the card are raw five-minute rows with no week anchor, because the
// board does not know where the week starts until a payload arrives. Those get the old
// stretched drawing and no diagonal, since there is nothing to compare them against.
void sparkline(int x, int y, int w, int h) {
  cv->drawFastHLine(x, y + h, w, C_PANEL);
  if (S.histAnchored)
    for (int i = 0; i <= w; i += 6)                     // the even-burn reference
      cv->drawPixel(x + i, y + h - (i * h) / w, C_DIM);
  if (S.nhist < 2) {
    textAt(x, y + h / 2 - 8, "collecting...", 2, C_PANEL);
    return;
  }
  int span = (S.histAnchored ? HISTORY_POINTS : S.nhist) - 1;
  if (span < 1) span = 1;
  int prevX = x, prevY = y + h - (S.hist[0] * h) / 100;
  for (int i = 1; i < S.nhist; i++) {
    int px = x + (i * w) / span;
    if (px > x + w) px = x + w;                         // a longer series than the grid cannot run off the page
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
    gaugeLandscape(4,  "CTX",  S.ctx, "");
    gaugeLandscape(50, "5HR",  S.h5,  r5);
    gaugeLandscape(96, "WEEK", S.wk,  rw);
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
    snprintf(b, sizeof b, "$%.2f", S.cost);
    textAt(6, 150, b, 2, C_DIM);
    snprintf(b, sizeof b, "%.12s", S.dir[0] ? S.dir : S.model);
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

// Weekly burn against an even spend. The dotted diagonal is where you would be if the
// week were spent evenly; above it means running hot. The percentage is the gap.
void pageBurn() {
  char b[28];
  bool land = landscape();
  textAt(6, 6, "WEEKLY BURN", 2, C_DIM);
  if (S.pace == 999) {
    textAt(6, 34, "no data yet", 2, C_DIM);
    return;
  }
  const char *word = S.pace > 8 ? "AHEAD OF PACE" : S.pace < -8 ? "UNDER PACE" : "ON PACE";
  uint16_t pc = S.pace > 20 ? C_RED : S.pace > 8 ? C_ORANGE : S.pace < -8 ? C_GREEN : C_GREEN;
  snprintf(b, sizeof b, "%+d%%", S.pace);
  textAt(6, 28, b, 4, pc);
  textAt(6 + textW(b, 4) + 10, 40, word, 2, pc);
  char r[12]; fmtRemaining(S.wkm, r, sizeof r);
  snprintf(b, sizeof b, "%d%% used   %s left", S.wk < 0 ? 0 : S.wk, r);
  textAt(6, land ? 66 : 74, b, 2, C_TXT);
  int y = land ? 88 : 100;
  int h = land ? H() - y - 26 : 120;
  sparkline(6, y, W() - 12, h);
  textAt(6, H() - 20, S.nhist < 2 ? "sampling every 5 min"
                      : S.histAnchored ? "dotted = even spend"
                                       : "recent trend, no week anchor", 2, C_DIM);
}

uint16_t sessColor(char st) {
  switch (st) {
    case 'n': return C_ORANGE;     // blocked on you
    case 'd': return C_AMBER;      // finished, your turn
    case 'w': return C_BLUE;       // running
    default:  return C_DIM;        // over, or idle
  }
}
const char *sessWord(char st) {
  // The colour bar already carries the state, so these only have to disambiguate.
  switch (st) {
    case 'n': return "YOU";
    case 'd': return "ready";
    case 'w': return "run";
    case 'o': return "over";
    default:  return "idle";
  }
}
void fmtWait(int sec, char *b, size_t n) {
  if (sec < 60)        snprintf(b, n, "%ds", sec);
  else if (sec < 3600) snprintf(b, n, "%dm", sec / 60);
  else if (sec < 86400)snprintf(b, n, "%dh", sec / 3600);
  else                 snprintf(b, n, "%dd", sec / 86400);
}

// Ranked by who is blocked and for how long: the top row is the thing to do next.
void pageSessions() {
  char b[32];
  if (S.nsess > S.nrows) snprintf(b, sizeof b, "SESSIONS %d  (%d shown)", S.nsess, S.nrows);
  else                   snprintf(b, sizeof b, "SESSIONS %d", S.nsess);
  textAt(6, 6, b, 2, C_DIM);
  if (S.nrows == 0) { textAt(6, 34, "none active", 2, C_DIM); return; }

  bool land = landscape();
  int y = 28, rowH = land ? 30 : 34;
  for (int i = 0; i < S.nrows && y + rowH <= H() - 22; i++) {
    Status::Sess &e = S.sess[i];
    uint16_t c = sessColor(e.st);
    cv->fillRect(6, y + 2, 6, rowH - 8, c);                 // state as a colour bar
    char w[10]; fmtWait(e.wait, w, sizeof w);
    if (land) {
      snprintf(b, sizeof b, "%.12s", e.name);
      textAt(18, y + 2, b, 2, e.st == 'o' || e.st == 'i' ? C_DIM : C_TXT);
      textAt(176, y + 2, sessWord(e.st), 2, c);         // colour carries the state; this disambiguates
      textRight(y + 2, w, 2, C_DIM);                    // how long it has been like that
    } else {
      textAt(18, y + 2, e.name, 2, e.st == 'o' || e.st == 'i' ? C_DIM : C_TXT);
      snprintf(b, sizeof b, "%s %s", sessWord(e.st), w);
      textAt(18, y + 18, b, 2, c);
    }
    y += rowH;
  }
  textAt(6, H() - 20, S.nblk ? "top row is next" : "nothing blocked", 2,
         S.nblk ? C_AMBER : C_DIM);
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
    // "warm" on its own says nothing about how long it stays that way. A rebuild is the
    // avoidable cost here, so show the countdown and go amber when it is close.
    if (S.cw && S.cxm >= 0) {
      snprintf(b, sizeof b, "warm %dm", S.cxm);
      textAt(x, 118, b, 2, S.cxm <= CACHE_WARN_MIN ? C_AMBER : C_GREEN);
    } else {
      textAt(x, 118, S.cw ? "warm" : "cold", 2, S.cw ? C_GREEN : C_DIM);
    }
    if (S.ctx < 0 && S.ctxt > 0) { char k[10]; fmtK(S.ctxt, k, sizeof k);
                                   snprintf(b, sizeof b, "ctx %s  cc %s", k, S.ver); }
    else if (S.cwin[0]) snprintf(b, sizeof b, "ctx %d%% of %s  cc %s", S.ctx < 0 ? 0 : S.ctx, S.cwin, S.ver);
    else                snprintf(b, sizeof b, "ctx %d%%  cc %s", S.ctx < 0 ? 0 : S.ctx, S.ver);
    textAt(6, H() - 20, b, 2, C_DIM);
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
  if (sdUp) snprintf(b, sizeof b, "fw %s  sd %luG", FW_VERSION, (unsigned long)((sdSizeMB + 512) / 1024));
  else      snprintf(b, sizeof b, "fw %s", FW_VERSION);
  textAt(lx, ly, b, 2, C_DIM);
  // Clock and uptime share a row so the address below can have one to itself.
  snprintf(b, sizeof b, "%s up %lum", clockStr(), millis() / 60000UL);
  textAt(lx, ly + 20, b, 2, C_DIM);
  // For ten minutes after power-up the board will enroll another Mac, and the command that
  // does it is the useful thing to be showing. It is too wide for this panel on one line, so
  // it wraps; it is one command. The code in it is the whole security story: the only way to
  // know this URL is to be standing here looking at it.
  if (wifiUp && enrollOpen()) {
    textAt(lx, ly + 40, "ENROLL", 2, C_AMBER);
    snprintf(b, sizeof b, "curl -fsSL %s", netIp());          textAt(lx, ly + 60, b, 2, C_TXT);
    textAt(lx, ly + 80, "/install.sh | sh", 2, C_TXT);
    snprintf(b, sizeof b, "code %lu", (unsigned long)enrollCode);
    textAt(lx, ly + 100, b, 2, C_AMBER);                      // it will ask for this
    return;                                  // the rest keeps for the other 23h50m
  }
  textAt(lx, ly + 40, nightMode() ? "night mode" : "day mode", 2, C_DIM);
  snprintf(b, sizeof b, "sessions %d", S.n);                  textAt(lx, ly + 60, b, 2, C_DIM);
  // Last row on purpose: an address is the widest thing on this page, and the button column
  // beside it stops higher up, so even a 15-character one has the width to itself here.
  // Drawn bright rather than dim because it is the line you read off the screen and type.
  textAt(lx, ly + 80, wifiUp ? netIp() : "no wifi", 2, wifiUp ? C_TXT : C_DIM);
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

// "Resting" means nothing is asking for you. An unanswered YOUR TURN is the opposite of
// that: the longer it waits the more it matters, so it must never fall behind a screensaver.
// Neither may a live session elsewhere: if anything is still working, the desk is not idle.
bool restingState() {
  if (head == H_NEEDS) return false;                 // blocked on you, never rest
  // A finished turn insists for a quarter of an hour and then stops shouting. It is not
  // forgotten: the screensaver still says "ready" in amber, still draws the done mascot, and
  // the LED stays amber. Refusing to rest at all just pinned the panel on until someone
  // walked over and pressed a button, which is not what the button is for.
  if (head == H_DONE)  return ack || millis() - stateChangedAt > DONE_REST_MS;
  if (S.nwork > 0)     return false;                 // another window is still running
  return head == H_IDLE || head == H_NOLINK;
}
bool saverActive() { return restingState() && !toast[0] && !(!haveLink && S.ts) && (forceSaver || millis() - stateChangedAt > SAVER_MS); }
// Dark, not asleep. The LED keeps carrying the state, which is the part that reads across a
// room anyway; the panel is only worth lighting for someone standing in front of it.
bool screenOff() { return restingState() && !toast[0] && millis() - stateChangedAt > SCREEN_OFF_MS; }
bool cycling() { return restingState() && !toast[0] && !saverActive() && (forceCycle || millis() - stateChangedAt > CYCLE_MS); }

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
  // Only while it is about to cost something. The limits line closes up to make room,
  // because at full spacing the two run into each other with no gap at all.
  bool cacheWarn = S.cw && S.cxm >= 0 && S.cxm <= CACHE_WARN_MIN;
  if (cacheWarn) {
    char c[14]; snprintf(c, sizeof c, "cache %dm", S.cxm);
    textRight(H() - 20, c, 2, C_AMBER);
  }
  snprintf(b, sizeof b, cacheWarn ? "WEEK %d%% 5HR %d%%" : "WEEK %d%%   5HR %d%%",
           S.wk < 0 ? 0 : S.wk, S.h5 < 0 ? 0 : S.h5);
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
    case PG_SESSIONS: pageSessions(); break;
    case PG_BURN:   pageBurn();   break;
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
  if (!haveLink && S.ts) {                             // feed gone: hold a dim pilot light
    setLed(r, g, b, night ? 0.0f : 0.08f);
    if (outageMajor() && now % 3000 < 200) setLed(255, 0, 0, 0.6f);
    return;
  }
  switch (head) {
    case H_WORKING: lv = 0.6f; break;                   // rainbow, hue from stateRGB
    case H_DONE:                                        // amber glow: brief hello, then steady
      if (quiet) lv = 0.15f;
      else if (t < 720) lv = ((t / 120) % 2 == 0) ? 1.0f : 0.1f;
      else if (t < 120000UL) lv = 0.5f;
      else lv = 0.25f;
      break;
    case H_NEEDS:                                       // orange; pulse once per blocked session
      if (quiet) { lv = 0.15f; break; }
      if (S.nblk > 1) {
        unsigned long period = (unsigned long)S.nblk * 300UL + 1400UL;
        unsigned long c = now % period;
        lv = (c < (unsigned long)S.nblk * 300UL && (c % 300UL) < 160UL) ? 1.0f : 0.0f;
      } else {
        lv = breathe(night ? 1500 : 1000, 0.15f, 1.0f);
      }
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
  if (screenOff()) target = 0;                        // last word: nothing wants you, so go dark
  if (target != backlight) { backlight = target; analogWrite(PIN_BL, backlight); }
}
bool screenDimmed() { return backlight <= 30; }   // includes fully off

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
    stateChangedAt = millis(); forceSaver = forceCycle = false; starsInit = false; page = PG_OVERVIEW; dirty = true; return;
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
// The board is the only always-on part of this and the only one every machine talks to, so
// it is where the things the display learns belong. Model window sizes are one of those: a
// transcript never states them, so they are learned from a status line and would otherwise
// sit in a file on whichever laptop happened to see one first. Kept here, a machine that has
// never run that model in a terminal still gets a real percentage on its first payload.
// The weekly curve is the board's too, for the same reason: it is the one participant that
// is always on. The host used to keep the samples in a file and send a finished curve, which
// meant the curve belonged to whichever laptop had been awake. Kept here it survives that
// laptop sleeping, being closed, or being a different laptop.
//
// No clock is needed. The payload says how many minutes are left in the week, which places a
// reading on the week's axis exactly, and a jump upward in that number is the week rolling
// over. Saved on bucket changes, so a seven-day week costs 32 writes.
#define WEEK_MINUTES (7L * 24 * 60)
int histIdx = -1;
unsigned long histSavedAt = 0;

void histLoad() {
  if (prefs.getBytesLength("hist") != HISTORY_POINTS) return;
  prefs.getBytes("hist", S.hist, HISTORY_POINTS);
  histIdx = prefs.getChar("histi", -1);
  if (histIdx >= 0) { S.nhist = histIdx + 1; S.histAnchored = true; }
}

void histSave() {
  prefs.putBytes("hist", S.hist, HISTORY_POINTS);
  prefs.putChar("histi", (int8_t)histIdx);
  histSavedAt = millis();
}

// Returns the pace: how far above an even spend the week is running.
int histUpdate(int wk, int wkm) {
  if (wk < 0 || wkm < 0) return 999;
  long left = wkm > WEEK_MINUTES ? WEEK_MINUTES : wkm;
  long elapsed = WEEK_MINUTES - left;
  int idx = (int)(elapsed * HISTORY_POINTS / WEEK_MINUTES);
  if (idx >= HISTORY_POINTS) idx = HISTORY_POINTS - 1;
  if (idx < histIdx) { memset(S.hist, 0, sizeof S.hist); histIdx = -1; }   // week rolled over
  for (int i = histIdx + 1; i <= idx; i++) S.hist[i] = (uint8_t)wk;        // carry any gap
  if (wk > S.hist[idx]) S.hist[idx] = (uint8_t)wk;                         // usage only rises
  bool moved = idx != histIdx;
  histIdx = idx;
  S.nhist = idx + 1;
  S.histAnchored = true;
  if (moved || millis() - histSavedAt > 300000UL) histSave();
  return wk - (int)(elapsed * 100 / WEEK_MINUTES);
}

void sendWindows() {
  String w = prefs.getString("win", "{}");
  Serial.printf("{\"win\":%s}\n", w.c_str());
}

void handleCommand(const char *cmd) {
  if (!strcmp(cmd, "net")) { netReport(); return; }
  if (!strcmp(cmd, "win")) { sendWindows(); return; }
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
  else if (!strcmp(cmd, "sleep")) { forceSaver = true; }
  else if (!strcmp(cmd, "cycle")) { forceCycle = true; pageChangedAt = 0; }
  dirty = true;
}

void handleLine(const char *line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) { Serial.println("{\"err\":\"json\"}"); return; }
  const char *cmd = doc["cmd"] | (const char *)NULL;
  if (cmd && !strcmp(cmd, "wifi")) { netCommand(doc); return; }
  if (cmd) { handleCommand(cmd); return; }
  // A payload may carry model window sizes the host has learned. Store them and they
  // outlive that host: this is the board's memory, not the laptop's.
  JsonVariantConst win = doc["win"];
  if (!win.isNull() && win.is<JsonObjectConst>()) {
    // Merge, never replace. A host that has just started knows nothing yet, and its first
    // payload would otherwise erase what every other machine has taught this board.
    JsonDocument cur;
    deserializeJson(cur, prefs.getString("win", "{}"));
    bool changed = false;
    for (JsonPairConst kv : win.as<JsonObjectConst>()) {
      if (cur[kv.key()].isNull() || cur[kv.key()] != kv.value()) {
        cur[kv.key()] = kv.value();
        changed = true;
      }
    }
    if (changed) {
      String out;
      serializeJson(cur, out);
      if (out.length() < 1024) {
        prefs.putString("win", out);
        Serial.printf("{\"win_saved\":%u}\n", (unsigned)cur.size());
      }
    }
    if (doc.size() == 1) return;                 // a windows-only push carries no state
  }
  long ts = doc["ts"] | 0L;
  if (!ts) { Serial.printf("{\"err\":\"no ts\",\"keys\":%u}\n", (unsigned)doc.size()); return; }
  S.ctx = doc["ctx"] | -1; S.h5 = doc["h5"] | -1; S.wk = doc["wk"] | -1;
  S.h5m = doc["h5m"] | -1; S.wkm = doc["wkm"] | -1;
  S.n = doc["n"] | 0; S.age = doc["age"] | -1;
  S.cost = doc["cost"] | 0.0f;
  S.lim = doc["lim"] | false;
  S.dur = doc["dur"] | 0; S.api = doc["api"] | 0; S.la = doc["la"] | 0; S.lr = doc["lr"] | 0;
  S.tin = doc["tin"] | 0; S.tout = doc["tout"] | 0; S.ch = doc["ch"] | -1; S.cw = doc["cw"] | false;
  S.nsess = doc["nsess"] | 0;
  S.nblk  = doc["nblk"]  | 0;
  S.nwork = doc["nwork"] | 0;
  S.nrdy  = doc["nrdy"]  | 0;
  S.cxm   = doc["cxm"]   | -1;
  S.ctxt  = doc["ctxt"]  | 0;
  S.rlage = doc["rlage"] | -1;
  S.blkw  = doc["blkw"]  | 0;
  S.nrows = 0;
  for (JsonObjectConst r : doc["sess"].as<JsonArrayConst>()) {
    if (S.nrows >= 4) break;
    Status::Sess &e = S.sess[S.nrows++];
    strlcpy(e.name, r["n"] | "", sizeof e.name);
    const char *st = r["s"] | "i";
    e.st = st[0];
    e.wait = r["w"] | 0;
    e.cost = r["c"] | 0.0f;
    e.ctx = r["x"] | -1;
  }
  S.quiet = doc["quiet"] | 70;
  // The curve lives here now, so a payload only has to say where the week stands.
  S.pace = histUpdate(S.wk, S.wkm);
  copyStr(S.ver, sizeof S.ver, doc["ver"], "");
  copyStr(S.cwin, sizeof S.cwin, doc["cwin"], "");
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
//
// Every line the host sends is a JSON object, so a line that does not open with '{' is one
// we joined partway through and there is no point collecting it. That happens on every
// boot: the host keeps writing while the board comes up, so whatever was in flight when
// the app started reads as a fragment, and parsing it was the source of a spurious
// {"err":"json"} after each connect. Skip to the next newline and pick up from there.
void drainSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (rxLen) {
        rx[rxLen] = 0;
        strlcpy(transport, "usb", sizeof transport);
        handleLine(rx);
      }
      rxLen = 0; rxSkip = false;
      continue;
    }
    if (rxSkip || c == '\r') continue;
    if (rxLen == 0 && c != '{') { rxSkip = true; continue; }   // joined mid-line
    if (rxLen < sizeof rx - 1) rx[rxLen++] = c;
    else { rxLen = 0; rxSkip = true; }                         // too long: drop at the newline
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
  sdBegin();
  histLoad();                                          // the board's own weekly curve
  if (S.nhist == 0 && sdUp) { S.nhist = sdLoadHistory(S.hist, sizeof S.hist); S.histAnchored = false; }
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
    lastHead = head; stateChangedAt = millis(); forceSaver = forceCycle = false; ack = false; dirty = true;
    if (head == H_DONE || head == H_NEEDS || head == H_LIMITED || head == H_OUTAGE) page = PG_OVERVIEW;
  }
  if (cycling()) {
    if (millis() - pageChangedAt > CYCLE_PAGE_MS) { page = (Page)((page + 1) % PG_COUNT); pageChangedAt = millis(); dirty = true; }
  } else if (page != PG_OVERVIEW && millis() - pageChangedAt > PAGE_RETURN_MS) { page = PG_OVERVIEW; dirty = true; }

  pollButton();
  sdLoop();
  updateBacklight();
  updateLed();

  drainSerial();
  unsigned long frame = (saverActive() || marqueeActive || head == H_WORKING) ? 80UL : (toast[0] || (head == H_IDLE && page == PG_OVERVIEW)) ? 500UL : 10000UL;
  if (screenOff()) { dirty = true; }                  // nothing to draw; redraw on the way back up
  else if (dirty || millis() - lastDraw > frame) { render(); lastDraw = millis(); dirty = false; }
  delay(10);
}
