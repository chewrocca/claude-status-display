// Wi-Fi, HTTP, NTP, own status polling, and BLE for the Claude status display.
// Credentials live only in NVS (Preferences) and are set over serial or BLE:
//   {"cmd":"wifi","ssid":"...","pass":"...","token":"..."}   {"cmd":"wifi","forget":true}
// The host daemon POSTs the same JSON payload to http://claude-status.local/status
// (header X-Token if a token is set). GET /shot dumps the framebuffer, GET /cmd?c=tap.

#include "payload.h"
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define NET_HOSTNAME "claude-status"
#define NET_POLL_MS  90000UL
#define TZ_CENTRAL   "CST6CDT,M3.2.0,M11.1.0"     // Central with US daylight-saving rules
#define BLE_SERVICE  "c1a0de00-5a1e-4f2b-9c3d-000000000001"
#define BLE_STATE    "c1a0de00-5a1e-4f2b-9c3d-000000000002"
#define BLE_CMD      "c1a0de00-5a1e-4f2b-9c3d-000000000003"

WebServer http(80);
volatile bool dumpInFlight = false;   // suppress other tasks logging into a binary dump
String netSsid, netPass, netToken;
bool wifiUp = false, mdnsUp = false, ntpSet = false, bleUp = false;
uint32_t enrollCode = 0;                               // see httpEnroll, below
bool enrollOpen() { return millis() < ENROLL_WINDOW_MS; }
unsigned long lastWifiTry = 0;
char netOut[12] = "unknown";       // worst level among watched components, fetched by the board
char netComp[8] = "";              // which watched component (API / CODE)
char netOther[84] = "";            // unwatched components that are not operational
char transport[8] = "none";        // usb | wifi | ble
BLECharacteristic *bleState = nullptr;
TaskHandle_t pollTask = nullptr;

// --- credentials ---------------------------------------------------------------------
void netLoadCreds() {
  netSsid = prefs.getString("ssid", "");
  netPass = prefs.getString("pass", "");
  netToken = prefs.getString("token", "");
  // Kept, not regenerated. Opening the serial port resets this board, so a code minted at
  // boot changed every time the daemon reconnected: you would read six digits off the screen
  // and they were already stale by the time you typed them. The window still starts at
  // power-up, so this is only usable while someone is standing here, which was the point.
  enrollCode = prefs.getUInt("enroll", 0);
  if (enrollCode < 100000 || enrollCode > 999999) {
    enrollCode = 100000 + (esp_random() % 900000);
    prefs.putUInt("enroll", enrollCode);
  }
}
void netSaveCreds(const char *ssid, const char *pass, const char *token) {
  prefs.putString("ssid", ssid ? ssid : "");
  prefs.putString("pass", pass ? pass : "");
  if (token) prefs.putString("token", token);
  netLoadCreds();
}
bool tokenOk(const String &given) { return netToken.length() == 0 || given == netToken; }

// --- clock ------------------------------------------------------------------------------
// Host time wins while it is fresh; otherwise the board's own NTP clock.
const char *clockStr() {
  static char b[12];
  if (S.hm[0] && haveLink && millis() - rxAt < 60000UL) return S.hm;
  time_t now = time(nullptr);
  if (now < 1700000000) return "--";
  struct tm t; localtime_r(&now, &t);
  int h = t.tm_hour % 12; if (!h) h = 12;
  snprintf(b, sizeof b, "%d:%02d%s", h, t.tm_min, t.tm_hour < 12 ? "am" : "pm");
  return b;
}

// The address the router handed us, for the About page. Static buffer: no heap churn on a
// page that redraws continuously, and 16 bytes holds the longest dotted quad plus its NUL.
const char *netIp() {
  static char b[16];
  strlcpy(b, WiFi.localIP().toString().c_str(), sizeof b);
  return b;
}

// --- HTTP server -------------------------------------------------------------------------
void httpStatus() {
  if (!tokenOk(http.header("X-Token"))) { http.send(401, "application/json", "{\"err\":\"token\"}"); return; }
  strlcpy(transport, "wifi", sizeof transport);
  String body = http.arg("plain");   // keep the String alive for the call
  handleLine(body.c_str());
  // Hand back what this board knows about model window sizes. A host on Wi-Fi never sees the
  // serial hello, so this reply is its only chance to learn them, and without it a machine
  // with no cable would count tokens forever while a cabled one showed percentages.
  http.send(200, "application/json", "{\"ok\":true,\"win\":" + prefs.getString("win", "{}") + "}");
}
void httpShot() {
  if (!tokenOk(http.header("X-Token"))) { http.send(401, "text/plain", "token"); return; }
  render();
  size_t len = (size_t)W() * H() * 2;
  http.sendHeader("X-Width", String(W())); http.sendHeader("X-Height", String(H()));
  http.setContentLength(len);
  http.send(200, "application/octet-stream", "");
  const uint8_t *buf = (const uint8_t *)cv->getBuffer();
  dumpInFlight = true;
  for (size_t off = 0; off < len; off += 2048) {
    size_t chunk = min((size_t)2048, len - off);
    // A client that stops reading stalls this write. Feed the watchdog each chunk and
    // bail on a short write rather than blocking loop() until the panic timer fires.
    if (http.client().write(buf + off, chunk) != chunk) break;
    esp_task_wdt_reset();
    if (!http.client().connected()) break;
  }
  dumpInFlight = false;
}
void httpCmd() {
  if (!tokenOk(http.header("X-Token"))) { http.send(401, "text/plain", "token"); return; }
  String c = http.arg("c");
  // Allowlist, not blocklist: only the harmless UI verbs are reachable over the network.
  if (!(c == "tap" || c == "hold" || c == "sleep" || c == "cycle" || c == "apipoll")) {
    http.send(400, "text/plain", "unknown command");
    return;
  }
  handleCommand(c.c_str());
  http.send(200, "application/json", "{\"ok\":true}");
}
// --- enrolling another Mac ------------------------------------------------------------
// Adding a second machine meant cloning the repo, running the installer, and copying the
// shared token by hand off whichever Mac provisioned the board. The board is on the network
// and already serves HTTP, so it can hand over the one command that does all of it.
//
// The token is what makes that worth doing and also what makes it dangerous: it is the only
// thing stopping anyone on the network writing to this display, so an endpoint that gives it
// to whoever asks would be worse than the inconvenience it removes. This is a pairing flow
// instead. The board picks a code at power-up and prints it as part of a URL on its own
// screen; that URL is the only one that returns the token, and the only way to know it is to
// be standing in front of the device. The window closes ten minutes after power-up, so an
// unattended board on a shared network is not handing anything to anyone.
// The script itself carries no secret, so this URL needs nothing in it and can be the plain
// /install.sh everyone expects. The code moves into the conversation instead: the script asks
// for it over /dev/tty, which a pipeline leaves free, and trades it for the token. So the
// address is public and the secret is still gated on having read six digits off the screen.
void httpEnroll() {
  String sh =
    "#!/bin/sh\n"
    "set -e\n"
    "S=\"$HOME/.claude/esp32-status\"; mkdir -p \"$S\"\n"
    "B=\"http://" + WiFi.localIP().toString() + "\"\n"
    // The code can come from the environment, for anywhere there is no one to ask: another
    // script, a provisioning run, or a shell that is not attached to a terminal.
    "C=\"${CLAUDE_STATUS_CODE:-}\"\n"
    // Testing /dev/tty with -r is not enough. The device node exists whether or not this
    // process has a controlling terminal, so the test passes and the open then fails with
    // "Device not configured", which under set -e kills the install before it starts. Open
    // it and see.
    "if [ -z \"$C\" ] && { exec 3<>/dev/tty; } 2>/dev/null; then\n"
    "  printf 'Code on the board: ' >&3\n"
    "  read C <&3 || C=\"\"\n"
    "  exec 3>&-\n"
    "fi\n"
    "GOT=\"\"\n"
    "if [ -n \"$C\" ]; then\n"
    // -S here would print curl's own "error: 403" a line before the explanation below, which
    // says the same thing better. Keep -f so it still fails; drop -S so it fails quietly.
    "  if T=$(curl -fs \"$B/t/$C\"); then\n"
    "    printf '%s' \"$T\" > \"$S/token\"; chmod 600 \"$S/token\"; GOT=1\n"
    "  else\n"
    // -f hides the body, and the body is the only thing that says which of the two
    // refusals this was. Ask again without it purely to report the reason.
    "    echo \"No token written: $(curl -sS \"$B/t/$C\")\" >&2\n"
    "  fi\n"
    "else\n"
    // Deliberately no nested quoting here: telling someone to type sh -c "$(curl ...)" means
    // escaping quotes inside quotes inside a C string, and the last version printed a literal
    // $B for exactly that reason. Exporting the variable needs none of it.
    "  echo 'No code, and no terminal to ask on. Export the code and re-run:' >&2\n"
    "  echo '  export CLAUDE_STATUS_CODE=<the six digits on the board>' >&2\n"
    "fi\n"
    // Re-run where a checkout already exists and it should use that, not leave a second copy
    // behind and quietly repoint the agent at it.
    "P=\"$HOME/Library/LaunchAgents/com.claude-status.display.plist\"\n"
    "U=\"$HOME/.config/systemd/user/claude-status-display.service\"\n"
    "D=\"\"\n"
    "if [ -f \"$P\" ]; then\n"
    "  E=$(sed -n 's|.*<string>\\(/.*\\)/host/claude_status_daemon.py</string>.*|\\1|p' \"$P\" | head -1)\n"
    "  [ -n \"$E\" ] && [ -f \"$E/host/install.sh\" ] && D=\"$E\"\n"
    "fi\n"
    // The same question asked of the other supervisor. Looking only for a plist meant a Linux
    // box that was already enrolled looked brand new every time, and got a second copy of the
    // host files in a different directory with the service still pointing at the first.
    "if [ -z \"$D\" ] && [ -f \"$U\" ]; then\n"
    "  E=$(sed -n 's|.*--script \\(/.*\\)/host/claude_status_daemon.py.*|\\1|p' \"$U\" | head -1)\n"
    "  [ -n \"$E\" ] && [ -f \"$E/host/install.sh\" ] && D=\"$E\"\n"
    "fi\n"
    // Otherwise take the files from the board. It is carrying them, so there is nothing to
    // clone and nothing to fetch from the internet: curl and a shell are the whole list.
    "if [ -z \"$D\" ]; then\n"
    "  D=\"${CLAUDE_STATUS_DIR:-$HOME/.claude-status-display}\"\n"
    "  mkdir -p \"$D/host\"\n"
    "  curl -fsSL \"$B/setup.sh\"  > \"$D/host/install.sh\"\n"
    "  curl -fsSL \"$B/hook.sh\"   > \"$D/host/esp32-status-hook.sh\"\n"
    "  curl -fsSL \"$B/daemon.py\" > \"$D/host/claude_status_daemon.py\"\n"
    "  chmod 755 \"$D/host/install.sh\" \"$D/host/esp32-status-hook.sh\"\n"
    "  echo \"Installed from the board into $D\"\n"
    "fi\n"
    // Only claim to have enrolled the machine if it actually got a token. Without one the
    // installer should say what is still missing, not congratulate anybody.
    "CLAUDE_STATUS_ENROLLED=\"$GOT\" \"$D/host/install.sh\"\n";
  http.send(200, "text/plain", sh);
}

// /t/<code> hands over the token, and is the only route that does. Refusals are HTTP errors
// rather than a 200 explaining itself, because the caller pipes this into a shell variable.
void httpNotFound() {
  String u = http.uri();
  if (u.startsWith("/t/")) {
    if (!enrollOpen()) {
      http.send(403, "text/plain", "enrollment closed: power-cycle the board and read its screen\n");
      return;
    }
    long given = u.substring(3).toInt();
    if (given && given == (long)enrollCode) {
      http.send(200, "text/plain", netToken);
      return;
    }
    http.send(403, "text/plain", "wrong code\n");
    return;
  }
  http.send(404, "text/plain", "not found\n");
}

void httpRoot() {
  char page[1000];
  snprintf(page, sizeof page,
    "<!doctype html><meta name=viewport content='width=device-width'><title>Claude Status</title>"
    "<body style='font-family:system-ui;background:#111;color:#eee;padding:24px'>"
    "<h2>Claude Status</h2><p>state <b>%s</b> &middot; api <b>%s</b></p>"
    "<p>ctx %d%% &middot; 5h %d%% (%s) &middot; week %d%% (%s)</p>"
    "<p>model %s &middot; %s &middot; fw " FW_VERSION " &middot; via %s</p>"
    "<p><a href='/dashboard' style='color:#ffa500; font-weight:bold; text-decoration:none;'>→ View Dashboard</a> &middot; "
    "<a href='/cmd?c=tap'>tap</a> &middot; <a href='/cmd?c=rotate'>rotate</a> &middot; <a href='/cmd?c=hold'>night</a></p>",
    S.st, S.out, S.ctx, S.h5, S.h5r, S.wk, S.wkr, S.model, S.dir, transport);
  http.send(200, "text/html", page);
}
void httpInfo() {
  char b[320];
  snprintf(b, sizeof b, "{\"fw\":\"" FW_VERSION "\",\"ip\":\"%s\",\"rssi\":%d,\"ntp\":%s,\"ble\":%s,"
           "\"transport\":\"%s\",\"own_out\":\"%s\",\"sd\":%lu,\"heap\":%u,\"minheap\":%u,\"uptime_s\":%lu}",
           WiFi.localIP().toString().c_str(), WiFi.RSSI(), ntpSet ? "true" : "false", bleUp ? "true" : "false",
           transport, netOut, (unsigned long)(sdUp ? sdSizeMB : 0), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), millis() / 1000UL);
  http.send(200, "application/json", b);
}

// --- live status API (JSON) ------------------------------------------------------------------
// A session name is a directory basename, so it can hold characters that would end the string
// early. Copy it a byte at a time and drop what JSON cannot carry bare.
static void jsonStr(char *out, size_t n, const char *in) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < n; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '"' || c == '\\' || c < 0x20) continue;
    out[o++] = (char)c;
  }
  out[o] = 0;
}

void httpApiStatus() {
  // The same rows the board's SESSIONS page draws, so the dashboard's list is not a second
  // answer to the question of what is running.
  char sess[600]; size_t so = 0;
  sess[so++] = '[';
  for (int i = 0; i < S.nrows && i < (int)(sizeof S.sess / sizeof S.sess[0]); i++) {
    Status::Sess &e = S.sess[i];
    char name[sizeof e.name]; jsonStr(name, sizeof name, e.name);
    char host[2] = {e.h ? e.h : ' ', 0};
    int w = snprintf(sess + so, sizeof sess - so,
                     "%s{\"name\":\"%s\",\"st\":\"%c\",\"wait\":%d,\"cost\":%.2f,\"ctx\":%d,\"h\":\"%s\"}",
                     i ? "," : "", name, e.st ? e.st : 'i', e.wait, e.cost, e.ctx, e.h ? host : "");
    if (w < 0 || so + (size_t)w >= sizeof sess - 2) break;   // leave room for the closing bracket
    so += (size_t)w;
  }
  sess[so++] = ']'; sess[so] = 0;

  // The weekly-burn series the board sparklines, so the page can draw the same curve rather
  // than invent a trend of its own.
  char hist[220]; size_t ho = 0;
  hist[ho++] = '[';
  for (int i = 0; i < S.nhist && i < (int)sizeof S.hist; i++) {
    int w = snprintf(hist + ho, sizeof hist - ho, "%s%d", i ? "," : "", S.hist[i]);
    if (w < 0 || ho + (size_t)w >= sizeof hist - 2) break;
    ho += (size_t)w;
  }
  hist[ho++] = ']'; hist[ho] = 0;

  char b[2800];
  snprintf(b, sizeof b,
    "{\"ctx\":%d,\"h5\":%d,\"wk\":%d,\"h5m\":%d,\"wkm\":%d,\"n\":%d,\"age\":%d,"
    "\"h5r\":\"%s\",\"wkr\":\"%s\",\"hm\":\"%s\",\"st\":\"%s\",\"out\":\"%s\","
    "\"inc\":\"%s\",\"comp\":\"%s\",\"model\":\"%s\",\"dir\":\"%s\",\"eff\":\"%s\","
    "\"cost\":%.2f,\"lim\":%s,\"night\":%s,\"cw\":%s,"
    "\"dur\":%d,\"api\":%d,\"la\":%d,\"lr\":%d,\"tin\":%d,\"tout\":%d,\"ch\":%d,\"cxm\":%d,"
    "\"fw\":\"" FW_VERSION "\",\"clock\":\"%s\","
    "\"nsess\":%d,\"nblk\":%d,\"nrdy\":%d,\"nwork\":%d,\"hosts\":%d,\"sess\":%s,"
    "\"pace\":%d,\"anch\":%s,\"hpts\":%d,\"hist\":%s}",
    S.ctx, S.h5, S.wk, S.h5m, S.wkm, S.n, S.age,
    S.h5r, S.wkr, S.hm, S.st, S.out,
    S.inc, S.comp, S.model, S.dir, S.eff,
    S.cost, S.lim ? "true" : "false", S.night ? "true" : "false", S.cw ? "true" : "false",
    S.dur, S.api, S.la, S.lr, S.tin, S.tout, S.ch, S.cxm,
    clockStr(),
    S.nsess, S.nblk, S.nrdy, S.nwork, liveHosts, sess,
    S.pace, S.histAnchored ? "true" : "false", HISTORY_POINTS, hist);
  http.send(200, "application/json", b);
}

// --- web dashboard -----------------------------------------------------------------------
void httpDashboard() {
  const char *page =
    "<!DOCTYPE html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Claude Status Dashboard</title>"
    "<style>"
    "* { margin:0; padding:0; box-sizing:border-box; }"
    "body { font-family:system-ui,-apple-system,sans-serif; background:#0a0a0a; color:#eee; padding:20px; }"
    // Five panels never divide evenly into a grid's columns, so auto-fit kept stranding one
    // of them alone on a second row beside a column of empty space. Flex lets whatever lands
    // on the last row grow to fill it, so the page is balanced at every window width.
    ".grid { display:flex; flex-wrap:wrap; gap:16px; margin-bottom:20px; align-items:stretch; }"
    ".panel { background:#1a1a1a; border:1px solid #333; border-radius:8px; padding:20px; }"
    // 230px is the narrowest a panel gets before "Opus 5 medium" wraps into its own label,
    // and five of them at that width fit any window from about 1220px up.
    ".grid > .panel { flex:1 1 230px; min-width:0; }"
    ".panel h3 { color:#ffc800; margin-bottom:12px; font-size:14px; text-transform:uppercase; }"
    // The gap and the right alignment are what keep a long value off its own label when the
    // panel is at its narrowest: it wraps under itself instead of colliding.
    ".stat { display:flex; justify-content:space-between; align-items:center; margin:8px 0; gap:12px; }"
    ".stat-label { color:#999; font-size:13px; }"
    ".stat-value { font-weight:bold; color:#fff; font-size:16px; text-align:right; }"
    "#dir { text-align:right; }"
    ".gauge { width:100%; height:20px; background:#333; border-radius:4px; overflow:hidden; margin-top:4px; }"
    ".gauge-fill { height:100%; background:linear-gradient(90deg,#2ecc71,#ffc800); transition:width 0.3s; }"
    ".status-working { color:#1a9eff; font-weight:bold; }"
    ".status-done { color:#ffc800; font-weight:bold; }"
    ".status-needs { color:#ff6b6b; font-weight:bold; }"
    ".status-idle { color:#666; font-weight:bold; }"
    // Added and removed, in the dashboard's own softened versions of the board's C_GREEN and
    // C_RED. The separator stays dim so the two numbers are what the eye lands on.
    ".lines-add { color:#2ecc71; }"
    ".lines-del { color:#ff6b6b; }"
    ".lines-sep { color:#666; font-weight:normal; margin:0 2px; }"
    ".health-none { color:#2ecc71; }"
    ".health-minor { color:#ffc800; }"
    ".health-major { color:#ff9500; }"
    ".health-critical { color:#ff3333; }"
    ".session-row { background:#0a0a0a; border:1px solid #222; border-radius:4px; padding:12px; margin:8px 0; display:flex; justify-content:space-between; align-items:center; }"
    ".session-name { font-weight:bold; color:#fff; }"
    ".session-time { color:#999; font-size:12px; }"
    ".session-state { padding:4px 8px; border-radius:3px; font-size:11px; font-weight:bold; }"
    ".state-n { background:#ff6b6b; color:#fff; }"
    ".state-d { background:#ffc800; color:#000; }"
    ".state-w { background:#1a9eff; color:#fff; }"
    ".state-o { background:#666; color:#fff; }"
    ".timestamp { color:#666; font-size:12px; margin-top:20px; text-align:center; }"
    "</style>"
    "<h1 style='margin-bottom:30px; font-size:24px;'>Claude Status <span id=clock style='color:#999;'></span></h1>"
    "<div class=grid>"
    "  <div class=panel>"
    "    <h3>Current Session</h3>"
    "    <div class=stat><span class=stat-label>State</span><span id=st class='stat-value status-idle'>–</span></div>"
    "    <div class=stat><span class=stat-label>Context</span><span id=ctx class=stat-value>–</span></div>"
    "    <div class=gauge><div class='gauge-fill' id=ctx-bar style='width:0%'></div></div>"
    "    <div class=stat><span class=stat-label>Model</span><span id=model class=stat-value>–</span></div>"
    "    <div class=stat><span class=stat-label>Directory</span><span id=dir style='font-size:12px; color:#999;'>–</span></div>"
    "  </div>"
    "  <div class=panel>"
    "    <h3>Rate Limits</h3>"
    "    <div style='margin-bottom:16px;'>"
    "      <div class=stat><span class=stat-label>5-Hour</span><span id=h5 class=stat-value>–</span></div>"
    "      <div class=gauge><div class='gauge-fill' id=h5-bar style='width:0%'></div></div>"
    "      <div style='font-size:11px; color:#999; margin-top:4px;'>Resets: <span id=h5r>–</span></div>"
    "    </div>"
    "    <div>"
    "      <div class=stat><span class=stat-label>7-Day</span><span id=wk class=stat-value>–</span></div>"
    "      <div class=gauge><div class='gauge-fill' id=wk-bar style='width:0%'></div></div>"
    "      <div style='font-size:11px; color:#999; margin-top:4px;'>Resets: <span id=wkr>–</span></div>"
    "    </div>"
    "  </div>"
    "  <div class=panel>"
    "    <h3>Weekly Burn</h3>"
    "    <div class=stat><span id=pace style='font-size:28px; font-weight:bold;'>–</span>"
    "      <span id=pacew style='font-size:13px; font-weight:bold;'></span></div>"
    "    <div style='font-size:12px; color:#999; margin-top:4px;'><span id=burnused>–</span> used &middot; <span id=burnleft>–</span> left</div>"
    "    <div id=proj style='font-size:13px; font-weight:bold; margin-top:8px;'>–</div>"
    "    <div id=advice style='font-size:11px; color:#999; line-height:1.4; margin-top:2px;'>aim to finish the week near 100%</div>"
    "    <svg id=spark viewBox='0 0 300 80' preserveAspectRatio=none style='width:100%; height:80px; margin-top:10px;'></svg>"
    "    <div id=sparknote style='font-size:11px; color:#666; margin-top:4px;'></div>"
    "  </div>"
    "  <div class=panel>"
    "    <h3>API Health</h3>"
    "    <div class=stat><span class=stat-label>Status</span><span id=out class='stat-value health-none'>–</span></div>"
    "    <div id=comp-row style='display:none;'>"
    "      <div class=stat><span class=stat-label>Affected</span><span id=comp class=stat-value>–</span></div>"
    "    </div>"
    "    <div id=inc-row style='display:none;'>"
    "      <div style='font-size:12px; color:#ffc800; margin-top:12px;'><strong>Incident:</strong> <span id=inc></span></div>"
    "    </div>"
    "  </div>"
    "  <div class=panel>"
    "    <h3>Session Stats</h3>"
    "    <div class=stat><span class=stat-label>Cost</span><span id=cost class=stat-value>$–</span></div>"
    "    <div class=stat><span class=stat-label>Duration</span><span id=dur class=stat-value>–</span></div>"
    "    <div class=stat><span class=stat-label>API Time</span><span id=api class=stat-value>–</span></div>"
    "    <div class=stat><span class=stat-label>Cache Hit</span><span id=ch class=stat-value>–</span></div>"
    // Split into two spans so each side carries its own colour, the way the board draws it.
    // The signs are not decoration: they are what keeps the row readable when the colours are
    // the one thing a reader cannot tell apart.
    "    <div class=stat><span class=stat-label>Lines ±</span><span class=stat-value>"
    "<span id=la class=lines-add>–</span><span class=lines-sep>/</span>"
    "<span id=lr class=lines-del>–</span></span></div>"
    "  </div>"
    "</div>"
    "<div class=panel style='margin-bottom:20px;'>"
    "  <h3>Active Sessions <span id=nsess style='color:#999; font-weight:normal;'></span></h3>"
    "  <div id=sessions></div>"
    "</div>"
    "<div class=timestamp>Last update: <span id=age>–</span> &middot; FW <span id=fw>–</span></div>"
    "<script>"
    "const API_PATH='/api/status';"
    "let lastUpdate=0;"
    // The same words the board's own band shows, so the page and the screen never disagree.
    "const STATE={working:['BUSY','working'],done:['READY','done'],needs_input:['NEEDS YOU','needs'],"
    "idle:['IDLE','idle'],over:['CLOSED','idle']};"
    "const ROW={n:['NEEDS YOU','state-n'],d:['READY','state-d'],w:['BUSY','state-w'],"
    "o:['CLOSED','state-o'],i:['IDLE','state-o']};"
    "function wait(s){if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m';"
    "if(s<86400)return Math.floor(s/3600)+'h';return Math.floor(s/86400)+'d';}"
    "function esc(s){return String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}"
    // What to do about the projection, in terms of the two dials that actually move it: the
    // model and the effort level. The advice names the current setting, so it never suggests
    // raising an effort already at the top or dropping one already at the bottom.
    "const EFF=['low','medium','high'];"
    "function advise(d,end){"
    "  const e=EFF.indexOf((d.eff||'').toLowerCase()),m=(d.model||'').split(' ')[0].toLowerCase();"
    "  const up=e>=0&&e<EFF.length-1?EFF[e+1]:'',dn=e>0?EFF[e-1]:'';"
    "  const big=m==='opus',small=m==='haiku';"
    // A spent 5-hour window blocks you within the hour, whatever the week says, so it is the
    // advice that matters until it resets.
    "  if(d.lim)return 'Rate limited now. Nothing to tune until it clears.';"
    "  if(d.h5>=85)return '5-hour window nearly spent — it resets at '+(d.h5r||'the top of the window')"
    "    +'. Lighter work until then.';"
    "  if(end===null)return 'aim to finish the week near 100%';"
    // Both dials can already be at the bottom: say so rather than recommending the model
    // you are on, or an effort level you cannot go below.
    "  const cheaper=big?'Move routine work to Sonnet':small?'':'Move routine work to Haiku';"
    "  if(end>130)return 'Too fast to last the week. '+(cheaper?cheaper+(dn?', or drop effort to '+dn:'')+'.'"
    "    :dn?'Drop effort to '+dn+'.':'Shorter sessions are the only dial left.');"
    "  if(end>105)return 'Running hot. '+(dn?'Drop to '+dn+' effort':cheaper||'Fewer long sessions')"
    "    +' to land near 100%.';"
    "  if(end>=85)return 'On target. Keep '+(d.model||'the current model')+(d.eff?' '+d.eff:'')+'.';"
    "  if(end>=60)return 'A little under. '+(up?up.charAt(0).toUpperCase()+up.slice(1)+' effort is affordable'"
    "    :'Room to spare')+' on the hard ones.';"
    "  return 'Well under — unspent allowance does not carry over. '"
    "    +(up?'Try '+up+' effort':big?'No reason to hold back':small?'Opus is affordable at this rate':'Opus is affordable')+'.';"
    "}"
    "function left(m){if(m<0)return '–';if(m<60)return m+'m';"
    "if(m<2880)return Math.floor(m/60)+'h'+String(m%60).padStart(2,'0')+'m';"
    "return Math.floor(m/1440)+'d '+Math.floor((m%1440)/60)+'h';}"
    // The board's own percentage ramp: green below 40, through yellow and orange to red.
    "function pct(p){if(p<0)return '#666';const mix=(a,b,t)=>a.map((v,i)=>Math.round(v+(b[i]-v)*t));"
    "let c;if(p<40)c=[0,255,48];else if(p<70)c=mix([0,255,48],[255,230,0],(p-40)/30);"
    "else if(p<90)c=mix([255,230,0],[255,120,0],(p-70)/20);else c=mix([255,120,0],[255,0,0],Math.min(1,(p-90)/10));"
    "return 'rgb('+c.join(',')+')';}"
    "function drawSpark(h,anch,hpts){"
    "  const el=document.getElementById('spark'),W=300,H=80;"
    "  if(!h||h.length<2){el.innerHTML='<text x=6 y=44 fill=#444 font-size=12>collecting…</text>';return;}"
    "  const span=Math.max(1,(anch?hpts:h.length)-1);"
    "  const pts=h.map((v,i)=>[Math.min(W,i*W/span),H-v*H/100]);"
    "  const d=pts.map((p,i)=>(i?'L':'M')+p[0].toFixed(1)+' '+p[1].toFixed(1)).join(' ');"
    "  const last=pts[pts.length-1],col=pct(h[h.length-1]);"
    // Early in the week the line sits on the baseline and reads as nothing. Filling under it
    // gives the spend a shape to see against the even-burn diagonal.
    "  const area=d+' L'+last[0].toFixed(1)+' '+H+' L0 '+H+' Z';"
    // Every value quoted: an unquoted one swallows the closing slash, and the elements after
    // it end up nested inside the tag instead of drawn beside it.
    "  el.innerHTML=(anch?'<line x1=\"0\" y1=\"'+H+'\" x2=\"'+W+'\" y2=\"0\" stroke=\"#555\" stroke-width=\"1\" stroke-dasharray=\"3 5\"/>':'')"
    "    +'<line x1=\"0\" y1=\"'+H+'\" x2=\"'+W+'\" y2=\"'+H+'\" stroke=\"#333\"/>'"
    "    +'<path d=\"'+area+'\" fill=\"'+col+'\" fill-opacity=\"0.22\" stroke=\"none\"/>'"
    "    +'<path d=\"'+d+'\" fill=\"none\" stroke=\"'+col+'\" stroke-width=\"2\" vector-effect=\"non-scaling-stroke\"/>'"
    "    +'<circle cx=\"'+last[0].toFixed(1)+'\" cy=\"'+last[1].toFixed(1)+'\" r=\"3\" fill=\"'+col+'\"/>';}"
    "async function updateDashboard(){try{"
    "  const r=await fetch(API_PATH);if(!r.ok)return;"
    "  const d=await r.json();"
    "  lastUpdate=Date.now();"
    "  const s=STATE[d.st]||[(d.st||'–').toUpperCase(),'idle'];"
    "  document.getElementById('st').textContent=s[0];"
    "  document.getElementById('st').className='stat-value status-'+s[1];"
    "  document.getElementById('ctx').textContent=d.ctx>=0?d.ctx+'%':'–';"
    "  document.getElementById('ctx-bar').style.width=(d.ctx>=0?d.ctx:0)+'%';"
    // Effort rides with the model: on its own row it reads like a separate setting, and it
    // is not one - it qualifies which model you are talking to.
    "  document.getElementById('model').textContent=(d.model||'–')+(d.eff?' '+d.eff:'');"
    "  document.getElementById('dir').textContent=d.dir||'–';"
    "  document.getElementById('h5').textContent=d.h5>=0?d.h5+'%':'–';document.getElementById('h5-bar').style.width=(d.h5>=0?d.h5:0)+'%';"
    "  document.getElementById('h5r').textContent=d.h5r||'–';"
    "  document.getElementById('wk').textContent=d.wk>=0?d.wk+'%':'–';document.getElementById('wk-bar').style.width=(d.wk>=0?d.wk:0)+'%';"
    "  document.getElementById('wkr').textContent=d.wkr||'–';"
    "  const outMap={none:'✓ All Good',minor:'⚠ Degraded',major:'⚠ Outage',critical:'✘ Critical',unknown:'?'};"
    "  document.getElementById('out').textContent=outMap[d.out]||'–';document.getElementById('out').className='stat-value health-'+d.out;"
    "  if(d.comp){document.getElementById('comp-row').style.display='block';document.getElementById('comp').textContent=d.comp;}else{document.getElementById('comp-row').style.display='none';}"
    "  if(d.inc){document.getElementById('inc-row').style.display='block';document.getElementById('inc').textContent=d.inc;}else{document.getElementById('inc-row').style.display='none';}"
    "  document.getElementById('cost').textContent='$'+(d.cost||0).toFixed(2);"
    "  document.getElementById('dur').textContent=(d.dur||0)+'m';"
    "  document.getElementById('api').textContent=(d.api||0)+'m';"
    "  document.getElementById('ch').textContent=d.ch>=0?d.ch+'%':'–';"
    "  document.getElementById('la').textContent='+'+(d.la||0);"
    "  document.getElementById('lr').textContent='-'+(d.lr||0);"
    "  document.getElementById('clock').textContent=d.hm||'';"
    "  document.getElementById('fw').textContent=d.fw||'–';"
    "  const age=d.age>=0?d.age+'s':'–';document.getElementById('age').textContent=age;"
    // Weekly burn: the pace number is the whole point, the curve is how it got there.
    "  const pc=d.pace,known=pc!==undefined&&pc!==999;"
    "  const pcol=!known?'#666':pc>20?'#ff3333':pc>8?'#ff9500':'#2ecc71';"
    "  document.getElementById('pace').textContent=known?(pc>0?'+':'')+pc+'%':'–';"
    "  document.getElementById('pace').style.color=pcol;"
    "  document.getElementById('pacew').textContent=known?(pc>8?'AHEAD OF PACE':pc<-8?'UNDER PACE':'ON PACE'):'no data yet';"
    "  document.getElementById('pacew').style.color=pcol;"
    "  document.getElementById('burnused').textContent=(d.wk>=0?d.wk:0)+'%';"
    "  document.getElementById('burnleft').textContent=left(d.wkm>=0?d.wkm:-1);"
    "  drawSpark(d.hist,d.anch,d.hpts||32);"
    "  document.getElementById('sparknote').textContent=(!d.hist||d.hist.length<2)?'sampling every 5 min'"
    "    :d.anch?'dotted = on track to finish at 100%':'recent trend, no week anchor';"
    // Pace says how this week compares to spending evenly; it does not say where the week
    // lands. Allowance left unspent at the reset is gone, so the number worth watching is
    // the one the current rate projects onto, and 100% is the target it should approach.
    "  const gone=10080-(d.wkm>=0?d.wkm:10080),el=document.getElementById('proj');"
    "  let end=null;"
    "  if(d.wk<0||gone<360){el.textContent='projecting…';el.style.color='#666';}"
    "  else{end=Math.round(d.wk*10080/gone);"
    "    el.textContent='on track for '+end+'% by '+(d.wkr||'reset');"
    "    el.style.color=end>105?'#ff9500':end>=85?'#2ecc71':end>=60?'#ffc800':'#888';}"
    "  document.getElementById('advice').textContent=advise(d,end);"
    // The board ranks these rows by who is blocked and for how long, so render them in the
    // order they arrive: the top one is the thing to do next.
    "  const rows=d.sess||[];"
    "  document.getElementById('nsess').textContent=d.nsess?'('+d.nsess+(rows.length<d.nsess?', '+rows.length+' shown':'')+')':'';"
    "  document.getElementById('sessions').innerHTML=rows.length?rows.map(e=>{"
    "    const r=ROW[e.st]||ROW.i;"
    "    const host=(d.hosts>1&&e.h)?esc(e.h)+':':'';"
    "    const ctx=e.ctx>=0?' &middot; ctx '+e.ctx+'%':'';"
    "    return '<div class=session-row><div><div class=session-name>'+host+esc(e.name)+'</div>'"
    "      +'<div class=session-time>'+wait(e.wait||0)+ctx+' &middot; $'+(e.cost||0).toFixed(2)+'</div></div>'"
    "      +'<span class=\"session-state '+r[1]+'\">'+r[0]+'</span></div>';"
    "  }).join(''):'<div style=\"color:#666; font-size:13px;\">none active</div>';"
    "}catch(e){console.error('Update failed:',e);}}"
    "updateDashboard();setInterval(updateDashboard,2000);"
    "</script>";
  http.send(200, "text/html", page);
}

// --- own status polling (FreeRTOS task so TLS never stalls the LED/animation) ------------------
void pollStatusTask(void *) {
  for (;;) {
    if (wifiUp && ESP.getFreeHeap() > 60000) {        // a TLS handshake needs real headroom
      NetworkClientSecure client; client.setInsecure();   // public status page; integrity only matters for the indicator
      client.setHandshakeTimeout(20);
      HTTPClient h; h.setTimeout(12000); h.setConnectTimeout(12000);
      int code = 0;
      if (h.begin(client, "https://status.claude.com/api/v2/summary.json") && (code = h.GET()) == 200) {
        JsonDocument filter, d;
        filter["components"][0]["name"] = true; filter["components"][0]["status"] = true;
        filter["incidents"][0]["status"] = true; filter["incidents"][0]["impact"] = true;
        filter["incidents"][0]["components"][0]["name"] = true;
        if (!deserializeJson(d, h.getStream(), DeserializationOption::Filter(filter))) {
          int worst = 0; const char *comp = ""; char others[sizeof netOther]; others[0] = 0;
          for (JsonObjectConst c : d["components"].as<JsonArrayConst>()) {
            const char *name = c["name"] | "", *st = c["status"] | "";
            int lvl = !strcmp(st, "operational") ? 0 : (!strcmp(st, "partial_outage") ? 2 : (!strcmp(st, "major_outage") ? 3 : 1));
            bool api = !strncmp(name, "Claude API", 10), code = !strcmp(name, "Claude Code");
            if (api || code) { if (lvl > worst) { worst = lvl; comp = api ? "API" : "CODE"; } }
            else if (lvl) {
              char shortName[40];
              strlcpy(shortName, name, sizeof shortName);
              char *paren = strstr(shortName, " (");
              if (paren) *paren = 0;
              if (others[0]) strlcat(others, "; ", sizeof others);
              strlcat(others, shortName, sizeof others);
              strlcat(others, " ", sizeof others);
              strlcat(others, st, sizeof others);
            }
          }
          // Statuspage can leave a component green while an incident against it is still
          // open. An open incident naming a watched component is at least a degradation.
          for (JsonObjectConst inc : d["incidents"].as<JsonArrayConst>()) {
            const char *st = inc["status"] | "";
            if (!strcmp(st, "resolved") || !strcmp(st, "postmortem")) continue;
            const char *hit = nullptr;
            for (JsonObjectConst c : inc["components"].as<JsonArrayConst>()) {
              const char *n = c["name"] | "";
              if (!strncmp(n, "Claude API", 10)) { hit = "API"; break; }
              if (!strcmp(n, "Claude Code"))     { hit = "CODE"; break; }
            }
            if (!hit) continue;
            const char *im = inc["impact"] | "";
            int lvl = !strcmp(im, "critical") ? 3 : !strcmp(im, "major") ? 2 : 1;
            if (lvl > worst) { worst = lvl; comp = hit; }
            break;
          }
          strlcpy(netOut, worst == 0 ? "none" : worst == 1 ? "minor" : worst == 2 ? "major" : "critical", sizeof netOut);
          strlcpy(netComp, comp, sizeof netComp);
          strlcpy(netOther, others, sizeof netOther);
          if (!dumpInFlight) Serial.printf("{\"poll\":\"%s\",\"comp\":\"%s\"}\n", netOut, netComp);
        }
      } else {
        if (!dumpInFlight)
          Serial.printf("{\"poll\":\"fail\",\"code\":%d,\"stack\":%u,\"heap\":%u}\n",
                        code, (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)ESP.getFreeHeap());
      }
      h.end();
    }
    vTaskDelay(pdMS_TO_TICKS(NET_POLL_MS));
  }
}

// --- BLE -------------------------------------------------------------------------------------------
// BLE callbacks run on the BLE stack's own task. Touching the Status struct or the
// canvas from there would race loop(). Queue the line and let loop() consume it.
static char bleQueue[768];
static volatile bool bleQueued = false;

class CmdCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String v = c->getValue();
    if (!v.length() || bleQueued) return;
    strlcpy(bleQueue, v.c_str(), sizeof bleQueue);
    bleQueued = true;
  }
};

void bleDrain() {
  if (!bleQueued) return;
  strlcpy(transport, "ble", sizeof transport);
  handleLine(bleQueue);
  bleQueued = false;
}
void bleBegin() {
  BLEDevice::init("Claude Status");
  BLEServer *srv = BLEDevice::createServer();
  BLEService *svc = srv->createService(BLE_SERVICE);
  bleState = svc->createCharacteristic(BLE_STATE, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  bleState->addDescriptor(new BLE2902());
  BLECharacteristic *cmd = svc->createCharacteristic(BLE_CMD, BLECharacteristic::PROPERTY_WRITE);
  cmd->setCallbacks(new CmdCallback());
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE); adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  bleUp = true;
}
void bleNotifyState() {
  if (!bleState) return;
  char b[120];
  snprintf(b, sizeof b, "{\"st\":\"%s\",\"ctx\":%d,\"h5\":%d,\"wk\":%d,\"out\":\"%s\",\"lim\":%s}", S.st, S.ctx, S.h5, S.wk, S.out, S.lim ? "true" : "false");
  bleState->setValue((uint8_t *)b, strlen(b));
  bleState->notify();
}

// --- lifecycle ---------------------------------------------------------------------------------------
void netBegin() {
  netLoadCreds();
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(NET_HOSTNAME);
  WiFi.setSleep(false);
  if (netSsid.length()) { WiFi.begin(netSsid.c_str(), netPass.c_str()); lastWifiTry = millis(); }
  setenv("TZ", TZ_CENTRAL, 1); tzset();
  http.on("/", httpRoot);
  http.on("/status", HTTP_POST, httpStatus);
  http.on("/shot", HTTP_GET, httpShot);
  http.on("/cmd", HTTP_GET, httpCmd);
  http.on("/info", HTTP_GET, httpInfo);
  http.on("/api/status", HTTP_GET, httpApiStatus);
  http.on("/dashboard", HTTP_GET, httpDashboard);
  http.on("/install.sh", HTTP_GET, httpEnroll);       // carries no secret, so needs no gate
  // The host files themselves, so enrolling needs neither git nor GitHub.
  http.on("/setup.sh",  HTTP_GET, [] { http.send_P(200, "text/plain", PAYLOAD_INSTALL); });
  http.on("/hook.sh",   HTTP_GET, [] { http.send_P(200, "text/plain", PAYLOAD_HOOK); });
  http.on("/daemon.py", HTTP_GET, [] { http.send_P(200, "text/plain", PAYLOAD_DAEMON); });
  http.onNotFound(httpNotFound);                      // /t/<code> is the gated one
  const char *hdrs[] = {"X-Token"}; http.collectHeaders(hdrs, 1);
  bleBegin();
  xTaskCreate(pollStatusTask, "poll", 16384, nullptr, 1, &pollTask);   // mbedTLS handshake is stack-hungry
}

void netLoop() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !wifiUp) {
    wifiUp = true;
    if (!mdnsUp && MDNS.begin(NET_HOSTNAME)) {
      mdnsUp = true;
      MDNS.addService("http", "tcp", 80);   // once only: re-adding leaks records on every AP flap
    }
    http.begin();
    configTzTime(TZ_CENTRAL, "pool.ntp.org", "time.apple.com");   // keeps Central + DST rules
    Serial.printf("{\"wifi\":\"up\",\"ip\":\"%s\"}\n", WiFi.localIP().toString().c_str());
    dirty = true;
  } else if (!up && wifiUp) {
    wifiUp = false; http.stop();
    Serial.println("{\"wifi\":\"down\"}");
    dirty = true;
  }
  if (!up && netSsid.length() && millis() - lastWifiTry > 15000UL) { WiFi.disconnect(); WiFi.begin(netSsid.c_str(), netPass.c_str()); lastWifiTry = millis(); }
  bleDrain();
  if (wifiUp) http.handleClient();
  if (!ntpSet && time(nullptr) > 1700000000) {
    ntpSet = true; dirty = true;
    setenv("TZ", TZ_CENTRAL, 1); tzset();          // belt and braces after the SNTP callback
    Serial.printf("{\"ntp\":\"%s\"}\n", clockStr());
  }
}

// serial/BLE commands for the network
void netCommand(JsonDocument &doc) {
  if (doc["forget"] | false) { netSaveCreds("", "", ""); WiFi.disconnect(true); Serial.println("{\"wifi\":\"forgotten\"}"); return; }
  const char *ssid = doc["ssid"] | (const char *)NULL;
  if (!ssid) { Serial.println("{\"err\":\"ssid required\"}"); return; }
  netSaveCreds(ssid, doc["pass"] | "", doc["token"] | (const char *)NULL);
  WiFi.disconnect(); WiFi.begin(netSsid.c_str(), netPass.c_str()); lastWifiTry = millis();
  Serial.println("{\"wifi\":\"saved\"}");
}
void netForcePoll() { if (pollTask) xTaskAbortDelay(pollTask); }

void netReport() {
  Serial.printf("{\"net\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"mdns\":\"%s.local\",\"ntp\":%s,\"ble\":%s,\"transport\":\"%s\",\"own_out\":\"%s\",\"clock\":\"%s\"}\n",
    wifiUp ? "up" : (netSsid.length() ? "connecting" : "unconfigured"), netSsid.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI(),
    NET_HOSTNAME, ntpSet ? "true" : "false", bleUp ? "true" : "false", transport, netOut, clockStr());
}
