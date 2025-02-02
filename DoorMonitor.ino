/*
Arduino UNO R4 Security Monitor Sketch

Register with Duck DNS for remote door status montoring via a small webserver over your internet connection.

This example monitors 4 doors, (Back Door, Slider, Front Door, and Basement Door).   Customize to your needs.


 */

#if defined(ARDUINO_PORTENTA_C33)
#include <WiFiC3.h>
#elif defined(ARDUINO_UNOWIFIR4)
#include <WiFiS3.h>
#endif

#include "RTC.h"
#include <NTPClient.h>
#include <WiFiUdp.h>

//Added for Duck DDNS
#include <SPI.h>
#include <HttpClient.h>
//  DDNS


int wifiStatus = WL_IDLE_STATUS;
WiFiUDP Udp; // A UDP instance to let us send and receive packets over UDP
NTPClient timeClient(Udp);

//14400000 milisecond timer = 4hrs.  Sync with the NTP and DDNS on this interval
#define TIMER_HRS 14400000UL
unsigned long startTime;

const char ssid[] = "Your SSID";  // change your network SSID (name)
const char pass[] = "Your WIFI Pass";   // change your network password (use for WPA, or use as key for WEP)
// Static IP address.  Comment out for DHCP
IPAddress ip(192, 168, 1, 2);   

const char* token = "YourDuckDNSToken"; // Replace with your DuckDNS token
const char* domain = "YourDuckDNSDomain"; // Replace with your DuckDNS domain, (name only without duckdns.org)

const char dateFormat[] PROGMEM = "    %02d.%02d.%d %02d:%02d:%02d    ";
const int bufferSize = 300;
char TimeBufferCurrent[bufferSize];
char TimeBufferStart[bufferSize];
char TimeBufferFront[bufferSize];
char TimeBufferBack[bufferSize];
char TimeBufferSlider[bufferSize];
char TimeBufferBasement[bufferSize];

int status = WL_IDLE_STATUS;
WiFiServer server(81);   // this sets the website port number

#define DOORFRONT_SENSOR_PIN 8 // The Arduino UNO R4 pin connected to door sensor's pin
int doorFront_state;
int prev_doorFront_state;

#define DOORSLIDER_SENSOR_PIN 9 // The Arduino UNO R4 pin connected to door sensor's pin
int doorSlider_state;
int prev_doorSlider_state;

#define DOORBACK_SENSOR_PIN 10 // The Arduino UNO R4 pin connected to door sensor's pin
int doorBack_state;
int prev_doorBack_state;

#define DOORBASEMENT_SENSOR_PIN 11 // The Arduino UNO R4 pin connected to door sensor's pin
int doorBasement_state;
int prev_doorBasement_state;

float getTemperature() {
  //return 26.9456;
  // YOUR SENSOR IMPLEMENTATION HERE
  // simulate the temperature value
  float temp_x100 = random(0, 10000);  // a ramdom value from 0 to 10000
  return temp_x100 / 100;              // return the simulated temperature value from 0 to 100 in float
}

void getDoors() {
// Watch Doors and record changes
RTCTime currentTime; 
RTC.getTime(currentTime);

// ----- Record start time if this is the first run and variable is empty. 
if (*TimeBufferStart == '\0'){
      snprintf(TimeBufferStart, bufferSize, dateFormat,
         Month2int(currentTime.getMonth()), currentTime.getDayOfMonth(), currentTime.getYear(),
         currentTime.getHour(), currentTime.getMinutes(), currentTime.getSeconds());
   }

// ------------- DOOR CHECK ----------------------
prev_doorFront_state = doorFront_state;
doorFront_state = digitalRead(DOORFRONT_SENSOR_PIN); // read current state

prev_doorSlider_state = doorSlider_state;
doorSlider_state = digitalRead(DOORSLIDER_SENSOR_PIN); // read current state

prev_doorBack_state = doorBack_state;
doorBack_state = digitalRead(DOORBACK_SENSOR_PIN); // read current state

prev_doorBasement_state = doorBasement_state;
doorBasement_state = digitalRead(DOORBASEMENT_SENSOR_PIN); // read current state

// Record time
snprintf(TimeBufferCurrent, bufferSize, dateFormat,
         Month2int(currentTime.getMonth()), currentTime.getDayOfMonth(), currentTime.getYear(),
         currentTime.getHour(), currentTime.getMinutes(), currentTime.getSeconds());

// Door just opened or closed.  Record close time
if (doorFront_state != prev_doorFront_state){
      snprintf(TimeBufferFront, bufferSize, dateFormat,
         Month2int(currentTime.getMonth()), currentTime.getDayOfMonth(), currentTime.getYear(),
         currentTime.getHour(), currentTime.getMinutes(), currentTime.getSeconds());
   }

// Door just opened or closed.  Record close time
if (doorSlider_state != prev_doorSlider_state){
      snprintf(TimeBufferSlider, bufferSize, dateFormat,
         Month2int(currentTime.getMonth()), currentTime.getDayOfMonth(), currentTime.getYear(),
         currentTime.getHour(), currentTime.getMinutes(), currentTime.getSeconds());
   }

// Door just opened or closed.  Record close time
if (doorBack_state != prev_doorBack_state){
      snprintf(TimeBufferBack, bufferSize, dateFormat,
         Month2int(currentTime.getMonth()), currentTime.getDayOfMonth(), currentTime.getYear(),
         currentTime.getHour(), currentTime.getMinutes(), currentTime.getSeconds());
   }

// Door just opened or closed.  Record close time
if (doorBasement_state != prev_doorBasement_state){
      snprintf(TimeBufferBasement, bufferSize, dateFormat,
         Month2int(currentTime.getMonth()), currentTime.getDayOfMonth(), currentTime.getYear(),
         currentTime.getHour(), currentTime.getMinutes(), currentTime.getSeconds());
   } 

// ------------- DOOR CHECK ----------------------
}

void syncRtcNtp(){

  // ********************* NTP
  Serial.println("\nStarting connection to server...");
  timeClient.begin();
  timeClient.update();

  // Get the current date and time from an NTP server and convert
  // it to UTC by passing the time zone offset in hours.
  // You may change the time zone offset to your local one.
  auto timeZoneOffsetHours = -5;
  auto unixTime = timeClient.getEpochTime() + (timeZoneOffsetHours * 3600);
  Serial.print("Unix time = ");
  Serial.println(unixTime);
  RTCTime timeToSet = RTCTime(unixTime);
  RTC.setTime(timeToSet);

  // Retrieve the date and time from the RTC and print them
  RTCTime currentTime;
  RTC.getTime(currentTime); 
  Serial.println("The RTC was just set to: " + String(currentTime));
//  *********************   NTP

}

void syncDuckDNS(){

  // DDNS
WiFiClient client;  //changed to Wifi
HttpClient http(client, "www.duckdns.org", 80);
  // Make a HTTP request:
String url = "/update?domains=" + String(domain) + "&token=" + String(token) + "&verbose=true";
http.get(url);
// Read the response
int statusCode = http.responseStatusCode();
String response = http.responseBody();

Serial.print("Status code: ");
Serial.println(statusCode);
Serial.print("Response: ");
Serial.println(response);

//DDNS

}

void setup() {
  //Initialize serial and wait for port to open:
  Serial.begin(9600);
  while (!Serial);

  //start recording timeer from startup
  startTime = millis();

  connectToWiFi();

  syncDuckDNS();

  RTC.begin();
  syncRtcNtp();
 
  pinMode(DOORFRONT_SENSOR_PIN, INPUT_PULLUP); // set arduino pin to input pull-up mode
  pinMode(DOORSLIDER_SENSOR_PIN, INPUT_PULLUP); // set arduino pin to input pull-up mode
  pinMode(DOORBACK_SENSOR_PIN, INPUT_PULLUP); // set arduino pin to input pull-up mode
  pinMode(DOORBASEMENT_SENSOR_PIN, INPUT_PULLUP); // set arduino pin to input pull-up mode

}

void connectToWiFi(){

    String fv = WiFi.firmwareVersion();
  if (fv < WIFI_FIRMWARE_LATEST_VERSION)
    Serial.println("Please upgrade the firmware");

//Set a static IP
  WiFi.config(ip);

  // attempt to connect to WiFi network:
  while (status != WL_CONNECTED) {
    Serial.print("Attempting to connect to SSID: ");
    Serial.println(ssid);
    // Connect to WPA/WPA2 network. Change this line if using open or WEP network:
    status = WiFi.begin(ssid, pass);

    // wait 10 seconds for connection:
    delay(10000);
  }
  server.begin();
  // you're connected now, so print out the status:
  printWifiStatus();

}

void loop() {

// Continuously watch the doors for status changes
getDoors();

//  -------  Timer - Sync the RTC to NTP and DDNS every 4 hrs -----------
if (millis() - startTime > TIMER_HRS)
  {
    syncRtcNtp();
    syncDuckDNS();
    startTime = millis();
  }
//  -------  Timer - Sync the RTC to NTP every 4 hrs -----------

  // listen for incoming clients
  WiFiClient client = server.available();
  if (client) {
    // read the HTTP request header line by line
    while (client.connected()) {
      if (client.available()) {
        String HTTP_header = client.readStringUntil('\n');  // read the header line of HTTP request

        if (HTTP_header.equals("\r"))  // the end of HTTP request
          break;

        Serial.print("<< ");
        Serial.println(HTTP_header);  // print HTTP request to Serial Monitor
      }
    }

    // send the HTTP response
    // send the HTTP response header
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("Connection: close");  // the connection will be closed after completion of the response
    client.println();                     // the separator between HTTP header and body
    // send the HTTP response body
    client.println("<!DOCTYPE HTML>");
    client.println("<html>");
    client.println("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">");  // Adjusts size for viewing on a phone

    client.println("<head>");
    client.println("<link rel=\"icon\" href=\"data:,\">");
    client.println("</head>");

    client.println("<p>");

    client.print("Current Time: <span style=\"color: red;\">");
    client.print(TimeBufferCurrent);
    client.println("</span>");
    
    client.println("<p>");

    client.print("Start Time: <span style=\"color: red;\">");
    client.print(TimeBufferStart);
    client.println("</span>");
    
   // client.print("Temperature: <span style=\"color: red;\">");
   // float temperature = getTemperature();
   // client.print(temperature, 2);
   // client.println("&deg;C</span>");

    client.println("</p>");

//  #################   Display DOORS  ################
    client.println("<p>");

    //Check door last time changes
    getDoors();

    client.print("Front Door : ");
      doorFront_state = digitalRead(DOORFRONT_SENSOR_PIN); // read current state

    if (doorFront_state == HIGH) {
      client.println("<span style=\"color: red;\"> Open: ");
      client.println(TimeBufferFront);
    } else {
      client.println("Closed: ");
      client.println(TimeBufferFront);
    }

    client.println("</span>");
    client.println("</p>");

    client.print("Slider Door : ");
      doorSlider_state = digitalRead(DOORSLIDER_SENSOR_PIN); // read current state

    if (doorSlider_state == HIGH) {
      client.println("<span style=\"color: red;\"> Open: ");
      client.println(TimeBufferSlider);
    } else {
      client.println("Closed: ");
      client.println(TimeBufferSlider);
    }

    client.println("</span>");
    client.println("</p>");
  
    client.print("Back Door : ");
      doorBack_state = digitalRead(DOORBACK_SENSOR_PIN); // read current state

    if (doorBack_state == HIGH) {
      client.println("<span style=\"color: red;\"> Open: ");
      client.println(TimeBufferBack);
    } else {
      client.println("Closed: ");
      client.println(TimeBufferBack);
    }

    client.println("</span>");
    client.println("</p>");

    client.print("Basement Door : ");
      doorBasement_state = digitalRead(DOORBASEMENT_SENSOR_PIN); // read current state

    if (doorBasement_state == HIGH) {
      client.println("<span style=\"color: red;\"> Open: ");
      client.println(TimeBufferBasement);
    } else {
      client.println("Closed: ");
      client.println(TimeBufferBasement);
    }

    client.println("</span>");
    client.println("</p>");

//  #################   END Display DOORS  ################

    client.println("</html>");
    client.flush();

    // give the web browser time to receive the data
    delay(10);

    // close the connection:
    client.stop();

  }
}

void printWifiStatus() {
  // print your board's IP address:
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // print the received signal strength:
  Serial.print("signal strength (RSSI):");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
}
  
