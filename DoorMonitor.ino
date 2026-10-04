/*
  Arduino UNO R4 WiFi Security Monitor  

  - Door/zone definitions live in ONE table (see "ZONE TABLE" below).
    Add, remove or rename zones there.
  - The R4's 12x8 LED matrix scrolls the name(s) of any open zone,
    e.g.  "OPEN: Front Door, Slider".  Idle = a small heartbeat dot.
  - "HIGH" = Normally Closed zone. Change to "LOW" in the table for any zones that are normally open.
  
  Wiring: each sensor between its pin and GND (pins use INPUT_PULLUP).

  Libraries (Library Manager): NTPClient (by Fabrice Weinberg).
  ArduinoGraphics + Arduino_LED_Matrix + RTC + WiFiS3 ship with the R4 core.
*/

#include <WiFiS3.h>
#include "RTC.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "ArduinoGraphics.h"      // must come BEFORE Arduino_LED_Matrix.h
#include "Arduino_LED_Matrix.h"

// ======================= USER SETTINGS =======================
const char ssid[] = "Your WIFI SSID";
const char pass[] = "YOUR WIFI PASS";

IPAddress staticIp(192, 168, 0, 99);       // comment out USE_STATIC_IP for DHCP
#define USE_STATIC_IP

const char* ddnsToken  = "YourDuckDNSToken";
const char* ddnsDomain = "YourDuckDNSDomain";   // name only, no .duckdns.org

const int  WEB_PORT          = 81;
const int  TZ_STD_OFFSET_HOURS = -5;            // standard time offset from UTC (US Eastern = -5)
const bool USE_US_DST          = true;          // auto-adjust using US rules (2nd Sun Mar -> 1st Sun Nov)
const unsigned long SYNC_INTERVAL_MS = 4UL * 60UL * 60UL * 1000UL;  // 4 hrs
const unsigned long DEBOUNCE_MS      = 30;
const unsigned long SCROLL_STEP_MS   = 70;      // lower = faster scroll

// ======================= ZONE TABLE ==========================
// name        : shown on the matrix and the web page
// pin         : Arduino pin the sensor is wired to (other side to GND)
// openLevel   : pin level that means "open". Reed switch/magnet that opens
//               when the door opens -> HIGH (pullup pulls it up). For a
//               normally-closed-to-ground-when-OPEN sensor use LOW.
struct ZoneConfig {
  const char* name;
  uint8_t     pin;
  uint8_t     openLevel;
};

const ZoneConfig ZONES[] = {
  { "Front Door", 8,  HIGH },
  { "Slider",     9,  HIGH },
  { "Back Door",  10, HIGH },
  { "Basement",   11, HIGH },
  // { "Garage",  12, HIGH },    // <- just add a line
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

// Scroll-display state
char          scrollMsg[160] = "";
int           scrollX = 12;
int           scrollW = 0;
unsigned long lastScroll = 0;
uint32_t      openMask = 0;       // bit per zone (supports up to 32 zones)
bool          heartbeat = false;

// ---------------------------------------------------------------
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
// LED matrix
void buildScrollMessage() {
  scrollMsg[0] = '\0';
  uint32_t mask = 0;
  size_t used = snprintf(scrollMsg, sizeof(scrollMsg), "OPEN: ");
  bool first = true;
  for (size_t i = 0; i < ZONE_COUNT; i++) {
    if (!state[i].isOpen) continue;
    if (i < 32) mask |= (1UL << i);
    if (used < sizeof(scrollMsg) - 1) {
      used += snprintf(scrollMsg + used, sizeof(scrollMsg) - used, "%s%s",
                       first ? "" : ", ", ZONES[i].name);
    }
    first = false;
  }
  if (mask == 0) scrollMsg[0] = '\0';
  openMask = mask;
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
void sendPage(WiFiClient& client) {
  formatTime(timeNow, sizeof(timeNow));

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE HTML><html><head>");
  client.println("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">");
  client.println("<link rel=\"icon\" href=\"data:,\">");
  client.println("</head><body>");

  client.print("<p>Current Time: <span style=\"color:red;\">");
  client.print(timeNow);
  client.println("</span></p>");
  client.print("<p>Start Time: <span style=\"color:red;\">");
  client.print(timeStart);
  client.println("</span></p>");

  for (size_t i = 0; i < ZONE_COUNT; i++) {
    client.print("<p>");
    client.print(ZONES[i].name);
    client.print(" : ");
    if (state[i].isOpen) client.print("<span style=\"color:red;\">Open: ");
    else                 client.print("<span>Closed: ");
    client.print(state[i].lastChange);
    client.println("</span></p>");
  }
  client.println("</body></html>");
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
