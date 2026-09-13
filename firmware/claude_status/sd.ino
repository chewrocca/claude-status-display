// microSD: on-board history so the weekly curve survives the Mac being off.
//
// The card shares the LCD's SPI bus (MOSI=GPIO6/LCD_DIN, SCLK=GPIO7/LCD_CLK); only CS and
// MISO are its own. Everything here runs from loop(), on the same thread as the renderer,
// so the two never overlap a transaction.

#include <FS.h>
#include <SD.h>

#define SD_HZ            4000000UL              // conservative: these lines also drive the panel
#define SD_SAMPLE_MS     (5UL * 60UL * 1000UL)  // matches the host sampler
#define SD_RETRY_MS      (5UL * 60UL * 1000UL)
#define SD_HIST_PATH     "/hist.csv"
#define SD_HIST_MAX      262144UL               // rotate at 256 KB; ~8000 samples, a month

bool sdUp = false;
uint32_t sdSizeMB = 0;
static unsigned long sdLastSample = 0, sdLastTry = 0;

// Mount, or quietly stay unmounted. There is no card-detect line on this board, so the only
// way to know a card is present is to talk to it.
void sdBegin() {
  sdLastTry = millis();
  static const uint32_t ladder[] = { 20000000UL, 4000000UL, 1000000UL, 400000UL };
  for (unsigned i = 0; i < sizeof ladder / sizeof ladder[0]; i++) {
    esp_task_wdt_reset();
    SD.end();
    digitalWrite(PIN_CS, HIGH);            // park the panel before touching the shared bus
    bool ok = SD.begin(PIN_SD_CS, spi, ladder[i]);
    uint8_t type = ok ? SD.cardType() : CARD_NONE;
    if (ok && type != CARD_NONE) {
      sdSizeMB = (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL));
      sdUp = true;
      Serial.printf("{\"sd\":{\"mb\":%lu,\"hz\":%lu}}\n", (unsigned long)sdSizeMB, (unsigned long)ladder[i]);
      esp_task_wdt_reset();
      return;
    }
  }
  SD.end();
  if (sdUp) Serial.println("{\"sd\":{\"mb\":0}}");   // it was there and now is not
  sdUp = false; sdSizeMB = 0;
  esp_task_wdt_reset();
}

// Append one sample. Same shape as the host's history.json rows, in a format that needs no
// parser on the other end.
void sdSample() {
  if (!sdUp || S.ts == 0 || stale()) return;
  if (millis() - sdLastSample < SD_SAMPLE_MS) return;
  sdLastSample = millis();

  esp_task_wdt_reset();
  File f = SD.open(SD_HIST_PATH, FILE_APPEND);
  if (!f) { sdUp = false; return; }            // card pulled mid-run
  if (f.size() > SD_HIST_MAX) { f.close(); SD.remove(SD_HIST_PATH); f = SD.open(SD_HIST_PATH, FILE_APPEND); }
  if (f) {
    char line[64];
    snprintf(line, sizeof line, "%lu,%d,%d,%d,%d\n",
             (unsigned long)S.ts, S.wk, S.h5, S.ctx, (int)head);
    f.print(line);
    f.close();
  }
  esp_task_wdt_reset();
}

// Seed the burn curve from the card at boot, so the page is populated before the daemon
// ever connects. Host-supplied history overwrites this the moment a payload arrives; it is
// sampled the same way and it knows the week boundary.
int sdLoadHistory(uint8_t *out, int maxPoints) {
  if (!sdUp) return 0;
  File f = SD.open(SD_HIST_PATH, FILE_READ);
  if (!f) return 0;

  // A sample is under 32 bytes, so the last 4 KB always covers more rows than we can draw.
  size_t sz = f.size();
  if (sz > 4096) f.seek(sz - 4096);

  int n = 0;
  char line[64];
  bool first = true;
  while (f.available()) {
    esp_task_wdt_reset();
    int len = f.readBytesUntil('\n', line, sizeof line - 1);
    if (len <= 0) continue;
    line[len] = 0;
    if (first) { first = false; if (sz > 4096) continue; }   // the seek landed mid-row
    const char *c = strchr(line, ',');
    if (!c) continue;
    int wk = atoi(c + 1);
    if (wk < 0 || wk > 100) continue;
    if (n == maxPoints) { memmove(out, out + 1, maxPoints - 1); n--; }
    out[n++] = (uint8_t)wk;
  }
  f.close();
  return n;
}

// Retried on a slow timer: a card inserted after boot should start working without a reset.
void sdLoop() {
  if (!sdUp && millis() - sdLastTry > SD_RETRY_MS) sdBegin();
  sdSample();
}
