// Wi-Fi, HTTP, NTP, own status polling, and BLE for the Claude status display.
// Credentials live only in NVS (Preferences) and are set over serial or BLE:
//   {"cmd":"wifi","ssid":"...","pass":"...","token":"..."}   {"cmd":"wifi","forget":true}
// The host daemon POSTs the same JSON payload to http://claude-status.local/status
// (header X-Token if a token is set). GET /shot dumps the framebuffer, GET /cmd?c=tap.

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
void httpRoot() {
  char page[900];
  snprintf(page, sizeof page,
    "<!doctype html><meta name=viewport content='width=device-width'><title>Claude Status</title>"
    "<body style='font-family:system-ui;background:#111;color:#eee;padding:24px'>"
    "<h2>Claude Status</h2><p>state <b>%s</b> &middot; api <b>%s</b></p>"
    "<p>ctx %d%% &middot; 5h %d%% (%s) &middot; week %d%% (%s)</p>"
    "<p>model %s &middot; %s &middot; fw " FW_VERSION " &middot; via %s</p>"
    "<p><a href='/cmd?c=tap'>tap</a> &middot; <a href='/cmd?c=rotate'>rotate</a> &middot; <a href='/cmd?c=hold'>night</a></p>",
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
