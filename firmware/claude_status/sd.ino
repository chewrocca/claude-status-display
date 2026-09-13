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
  esp_task_wdt_reset();
  if (!SD.begin(PIN_SD_CS, spi, SD_HZ)) { sdUp = false; sdSizeMB = 0; return; }
  if (SD.cardType() == CARD_NONE) { SD.end(); sdUp = false; sdSizeMB = 0; return; }
  sdSizeMB = (uint32_t)(SD.cardSize() / (1024ULL * 1024ULL));
  sdUp = true;
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
