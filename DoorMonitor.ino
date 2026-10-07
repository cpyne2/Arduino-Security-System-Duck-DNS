/*
  Arduino UNO R4 WiFi Security Monitor  (optimized)

  - Door/zone definitions live in ONE table (see "ZONE TABLE" below).
    Add, remove or rename zones there; nothing else needs to change.
  - The R4's 12x8 LED matrix scrolls the name(s) of any open zone,
    e.g.  "OPEN: Front Door, Slider".  Idle = a small blinking dot.
  - Same web page / Duck DNS / NTP behavior as the original sketch.

  Wiring: each sensor between its pin and GND (pins use INPUT_PULLUP).

  Libraries (Library Manager): NTPClient (by Fabrice Weinberg) and
  ArduinoGraphics (by Arduino).
  WiFiS3, RTC and Arduino_LED_Matrix ship with the UNO R4 board core.
*/

#include <WiFiS3.h>
#include "RTC.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "ArduinoGraphics.h"      // must come BEFORE Arduino_LED_Matrix.h
#include "Arduino_LED_Matrix.h"

// ======================= USER SETTINGS =======================
const char ssid[] = "YOUR SSID";
const char pass[] = "YOUR WIFI PASSWORD";

IPAddress staticIp(192, 168, 1, 2);       // comment out USE_STATIC_IP for DHCP
#define USE_STATIC_IP

const char* ddnsToken  = "YOUR DUCKDNS TOKEN";
const char* ddnsDomain = "YOUR DUCKDNS SITE";   // name only, no .duckdns.org

const int  WEB_PORT          = 81;
const int  TZ_STD_OFFSET_HOURS = -5;            // standard time offset from UTC (US Eastern = -5)
const bool USE_US_DST          = true;          // auto-adjust using US rules (2nd Sun Mar -> 1st Sun Nov)
const unsigned long SYNC_INTERVAL_MS = 4UL * 60UL * 60UL * 1000UL;  // 4 hrs
const unsigned long DEBOUNCE_MS      = 30;

// Display settings
const unsigned long SCROLL_STEP_MS = 70;  // lower = faster scrolling

// ======================= ZONE TABLE ==========================
// name        : shown on the web page
// pin         : Arduino pin the sensor is wired to (other side to GND)
// openLevel   : pin level that means "open". Reed switch/magnet that opens
//               when the door opens -> HIGH (pullup pulls it up). For a
//               sensor that closes to ground when OPEN use LOW.
// Zone numbers are simply the row position: first row = zone 1, etc.
struct ZoneConfig {
  const char* name;
  uint8_t     pin;
  uint8_t     openLevel;
};

const ZoneConfig ZONES[] = {
  { "Front Door", 8,  HIGH },   // zone 1
  { "Slider",     9,  HIGH },   // zone 2
  { "Back Door",  10, HIGH },   // zone 3
  { "Basement",   11, HIGH },   // zone 4
  // { "Garage",  12, HIGH },   // zone 5  <- just add a line
};
const size_t ZONE_COUNT = sizeof(ZONES) / sizeof(ZONES[0]);
// =============================================================

// Runtime state, one entry per zone (sized automatically from the table)
struct ZoneState {
  bool          isOpen;
  bool          lastRawOpen;
  unsigned long rawChangedAt;
  char          lastChange[24];
};
ZoneState state[ZONE_COUNT];

char timeNow[24]   = "";
char timeStart[24] = "";

WiFiUDP    udp;
NTPClient  timeClient(udp);
WiFiServer server(WEB_PORT);
ArduinoLEDMatrix matrix;

unsigned long lastSync = 0;
bool          timeValid = false;

// Character shown for a zone: 1-9, then A, B, C ...
char zoneLabel(size_t i) {
  return (i < 9) ? (char)('1' + i) : (char)('A' + (i - 9));
}

// ---- Time zone / DST helpers (the RTC always holds UTC) ----
// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm)
long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}

int dayOfWeek(int y, int m, int d) {          // 0 = Sunday
  return (int)((daysFromCivil(y, m, d) + 4) % 7);
}

bool isDST(time_t utc) {
  if (!USE_US_DST) return false;
  time_t stdLocal = utc + (time_t)TZ_STD_OFFSET_HOURS * 3600;
  struct tm tmv;
  gmtime_r(&stdLocal, &tmv);
  int y = tmv.tm_year + 1900;

  int marchDay = 8 + (7 - dayOfWeek(y, 3, 8)) % 7;     // 2nd Sunday in March
  int novDay   = 1 + (7 - dayOfWeek(y, 11, 1)) % 7;    // 1st Sunday in November

  // DST starts 2:00 local standard time, ends 2:00 local daylight time
  time_t startUtc = (time_t)daysFromCivil(y, 3, marchDay) * 86400L + 2 * 3600L
                    - (time_t)TZ_STD_OFFSET_HOURS * 3600L;
  time_t endUtc   = (time_t)daysFromCivil(y, 11, novDay) * 86400L + 2 * 3600L
                    - (time_t)(TZ_STD_OFFSET_HOURS + 1) * 3600L;
  return utc >= startUtc && utc < endUtc;
}

void formatTime(char* buf, size_t len) {
  RTCTime t;
  RTC.getTime(t);
  time_t utc = (time_t)t.getUnixTime();
  time_t local = utc + (time_t)(TZ_STD_OFFSET_HOURS + (isDST(utc) ? 1 : 0)) * 3600L;
  struct tm tmv;
  gmtime_r(&local, &tmv);
  snprintf(buf, len, "%02d.%02d.%d %02d:%02d:%02d",
           tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_year + 1900,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

// ---------------------------------------------------------------
// Zone scanning
bool readZoneOpen(size_t i) {
  return digitalRead(ZONES[i].pin) == ZONES[i].openLevel;
}

// Debounced scan of every zone. Returns true if any zone changed.
bool scanZones() {
  bool changed = false;
  unsigned long now = millis();
  for (size_t i = 0; i < ZONE_COUNT; i++) {
    bool raw = readZoneOpen(i);
    if (raw != state[i].lastRawOpen) {
      state[i].lastRawOpen = raw;
      state[i].rawChangedAt = now;
    }
    if (raw != state[i].isOpen && (now - state[i].rawChangedAt) >= DEBOUNCE_MS) {
      state[i].isOpen = raw;
      formatTime(state[i].lastChange, sizeof(state[i].lastChange));
      changed = true;
    }
  }
  return changed;
}

// ---------------------------------------------------------------
// LED matrix: scrolling text listing the open zones
char          scrollMsg[160] = "";
int           scrollX = 12;
int           scrollW = 0;
unsigned long lastScroll = 0;
bool          heartbeat = false;

// Rebuild "OPEN: Front Door, Slider" from the current zone states
void buildScrollMessage() {
  scrollMsg[0] = '\0';
  size_t used = 0;
  bool first = true;
  for (size_t i = 0; i < ZONE_COUNT; i++) {
    if (!state[i].isOpen) continue;
    if (used >= sizeof(scrollMsg) - 1) break;
    int n;
    if (first) n = snprintf(scrollMsg, sizeof(scrollMsg), "OPEN: %s", ZONES[i].name);
    else       n = snprintf(scrollMsg + used, sizeof(scrollMsg) - used, ", %s", ZONES[i].name);
    if (n > 0) used += n;
    first = false;
  }
  scrollW = strlen(scrollMsg) * 4;     // Font_4x6 ~ 4 px per character
  scrollX = 12;                        // start just off the right edge
}

void drawFrame() {
  matrix.beginDraw();
  matrix.clear();
  matrix.stroke(0xFFFFFFFF);
  if (scrollMsg[0]) {
    matrix.textFont(Font_4x6);
    matrix.text(scrollMsg, scrollX, 1);
  } else if (heartbeat) {
    matrix.point(0, 0);                // idle heartbeat in the corner
  }
  matrix.endDraw();
}

void updateDisplay() {
  unsigned long now = millis();
  if (now - lastScroll < SCROLL_STEP_MS) return;
  lastScroll = now;

  if (scrollMsg[0]) {
    drawFrame();
    if (--scrollX < -scrollW) scrollX = 12;   // loop
  } else {
    // idle: blink once per ~1 s (every 14 steps at 70 ms)
    static uint8_t tick = 0;
    if (++tick >= 14) { tick = 0; heartbeat = !heartbeat; drawFrame(); }
  }
}

// Scroll a message across the matrix once (blocking; used only at boot)
void scrollOnce(const char* msg) {
  int w = strlen(msg) * 4;
  matrix.textFont(Font_4x6);
  for (int x = 12; x >= -w; x--) {
    matrix.beginDraw();
    matrix.clear();
    matrix.stroke(0xFFFFFFFF);
    matrix.text(msg, x, 1);
    matrix.endDraw();
    delay(SCROLL_STEP_MS);
  }
  matrix.beginDraw();
  matrix.clear();
  matrix.endDraw();
}

// ---------------------------------------------------------------
// Network / time
void syncRtcNtp() {
  timeClient.begin();
  if (timeClient.update() || timeClient.forceUpdate()) {
    RTCTime t((time_t)timeClient.getEpochTime());   // NTP time is UTC
    RTC.setTime(t);
    timeValid = true;
    Serial.println("RTC synced to NTP");
  } else {
    Serial.println("NTP sync failed");
  }
}

void syncDuckDNS() {
  WiFiClient client;
  if (!client.connect("www.duckdns.org", 80)) {
    Serial.println("DuckDNS connect failed");
    return;
  }
  client.print("GET /update?domains=");
  client.print(ddnsDomain);
  client.print("&token=");
  client.print(ddnsToken);
  client.println("&verbose=true HTTP/1.1");
  client.println("Host: www.duckdns.org");
  client.println("Connection: close");
  client.println();

  // Print the reply (body contains OK/KO), give up after 5 seconds
  unsigned long t0 = millis();
  while ((client.connected() || client.available()) && millis() - t0 < 5000) {
    while (client.available()) Serial.write(client.read());
  }
  Serial.println();
  client.stop();
}

void connectToWiFi() {
  if (String(WiFi.firmwareVersion()) < String(WIFI_FIRMWARE_LATEST_VERSION)) {
    Serial.println("Please upgrade the WiFi firmware");
  }
#ifdef USE_STATIC_IP
  WiFi.config(staticIp);
#endif
  int status = WL_IDLE_STATUS;
  while (status != WL_CONNECTED) {
    Serial.print("Connecting to ");
    Serial.println(ssid);
    status = WiFi.begin(ssid, pass);
    if (status != WL_CONNECTED) delay(5000);
  }
  server.begin();
  Serial.print("IP: ");
  Serial.print(WiFi.localIP());
  Serial.print("  RSSI: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
}

// ---------------------------------------------------------------
// Web page
// One "label ... value" row of the info card
void infoRow(WiFiClient& client, const char* label, const char* value) {
  client.print("<div class=\"row\"><span class=\"label\">");
  client.print(label);
  client.print("</span><span class=\"value\">");
  client.print(value);
  client.println("</span></div>");
}

void sendPage(WiFiClient& client) {
  formatTime(timeNow, sizeof(timeNow));

  size_t openCount = 0;
  for (size_t i = 0; i < ZONE_COUNT; i++) if (state[i].isOpen) openCount++;

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();

  // Styles: grey labels, blue values, red = open/alarm, green = closed/ok.
  // Light and dark mode follow the viewer's device setting.
  client.print(R"HTML(<!DOCTYPE HTML><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="icon" href="data:,">
<title>Security Monitor</title>
<style>
:root{--bg:#f4f6f8;--card:#fff;--text:#1d2530;--label:#5f6b7a;--value:#0b5cad;
--ok:#1b7f3b;--okbg:#e6f4ea;--alarm:#c62828;--alarmbg:#fdecea;--line:#e3e8ee}
@media(prefers-color-scheme:dark){:root{--bg:#12161b;--card:#1b2129;--text:#e6ebf1;
--label:#9aa7b5;--value:#6cb2ff;--ok:#5fd37f;--okbg:#17301f;--alarm:#ff6b6b;
--alarmbg:#3a1a1a;--line:#2a323c}}
body{font-family:system-ui,Arial,sans-serif;background:var(--bg);color:var(--text);margin:0;padding:16px}
.wrap{max-width:520px;margin:0 auto}
h1{font-size:1.25rem;margin:0 0 12px}
.banner{padding:12px 14px;border-radius:10px;font-weight:700;margin-bottom:12px}
.banner.ok{background:var(--okbg);color:var(--ok)}
.banner.alarm{background:var(--alarm);color:#fff}
.card{background:var(--card);border-radius:10px;padding:2px 14px;margin-bottom:12px;
box-shadow:0 1px 3px rgba(0,0,0,.15)}
.row{display:flex;justify-content:space-between;align-items:center;gap:12px;
padding:10px 0;border-bottom:1px solid var(--line)}
.row:last-child{border-bottom:0}
.label{color:var(--label)}
.value{color:var(--value);font-weight:600;text-align:right}
.zone.open{background:var(--alarmbg);margin:0 -14px;padding:10px 14px}
.num{display:inline-block;min-width:1.6em;text-align:center;border-radius:5px;
background:var(--line);color:var(--label);font-weight:700;margin-right:8px}
.name{font-weight:600}
.right{text-align:right}
.state{display:block;font-weight:800;letter-spacing:.04em}
.state.open{color:var(--alarm)}
.state.closed{color:var(--ok)}
.when{display:block;font-size:.85rem;color:var(--value)}
</style></head><body><div class="wrap">
<h1>Security Monitor</h1>
)HTML");

  if (openCount == 0) {
    client.println("<div class=\"banner ok\">&#10003; All zones closed</div>");
  } else {
    client.print("<div class=\"banner alarm\">&#9888; ");
    client.print(openCount);
    client.println(openCount == 1 ? " zone open</div>" : " zones open</div>");
  }

  // Info card
  client.println("<div class=\"card\">");
  infoRow(client, "Current Time", timeNow);
  infoRow(client, "Start Time", timeStart);
  String ipPort = WiFi.localIP().toString() + ":" + String(WEB_PORT);
  infoRow(client, "Local IP", ipPort.c_str());
  client.println("</div>");

  // Zone card
  client.println("<div class=\"card\">");
  for (size_t i = 0; i < ZONE_COUNT; i++) {
    bool o = state[i].isOpen;
    client.print(o ? "<div class=\"row zone open\">" : "<div class=\"row zone\">");
    client.print("<span><span class=\"num\">");
    client.print(zoneLabel(i));
    client.print("</span><span class=\"name\">");
    client.print(ZONES[i].name);
    client.print("</span></span><span class=\"right\">");
    client.print(o ? "<span class=\"state open\">OPEN</span>"
                   : "<span class=\"state closed\">Closed</span>");
    client.print("<span class=\"when\">");
    client.print(state[i].lastChange);
    client.println("</span></span></div>");
  }
  client.println("</div>");
  client.println("</div></body></html>");
}

void handleClient() {
  WiFiClient client = server.available();
  if (!client) return;

  unsigned long t0 = millis();
  while (client.connected() && millis() - t0 < 1000) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      if (line.equals("\r")) break;          // end of request headers
    }
  }
  sendPage(client);
  client.flush();
  delay(10);
  client.stop();
}

// ---------------------------------------------------------------
void setup() {
  Serial.begin(9600);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 2000);   // don't hang when USB isn't connected

  matrix.begin();

  for (size_t i = 0; i < ZONE_COUNT; i++) {
    pinMode(ZONES[i].pin, INPUT_PULLUP);
    bool o = readZoneOpen(i);
    state[i].isOpen = state[i].lastRawOpen = o;
    state[i].rawChangedAt = millis();
    strcpy(state[i].lastChange, "since boot");
  }

  connectToWiFi();
  char ipMsg[32];
  snprintf(ipMsg, sizeof(ipMsg), "IP %s", WiFi.localIP().toString().c_str());
  scrollOnce(ipMsg);                         // show the IP on the matrix at boot

  RTC.begin();
  syncRtcNtp();
  syncDuckDNS();
  lastSync = millis();

  formatTime(timeStart, sizeof(timeStart));
  buildScrollMessage();                      // show any zone already open at boot
}

void loop() {
  if (scanZones()) buildScrollMessage();     // only rebuild text on a change
  updateDisplay();

  if (millis() - lastSync > SYNC_INTERVAL_MS) {
    if (WiFi.status() != WL_CONNECTED) connectToWiFi();
    syncRtcNtp();
    syncDuckDNS();
    lastSync = millis();
  }

  handleClient();
}
