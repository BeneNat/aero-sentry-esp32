/*
 * PROJECT: AERO SENTRY (Final Polish V1.1)
 * Author: Filip Zurek
 * Hardware: ESP32 + BME680 (I2C) + TFT ILI9341 (SPI) + 8-bit LED bar
 * Description: Air monitoring station with web server and trend forecast
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Adafruit_Sensor.h>
#include "Adafruit_BME680.h"
#include "time.h"

// --- 1. User Configuration ---
const char* ssid = "ssid";
const char* password = "password";

// chart update frequency (in milliseconds)
// 300000 = 5 minutes (standard)
// 2000 = 2 seconds (demo/test)
#define GRAPH_INTERVAL 300000 

// --- 2. Object and Constants ---
TFT_eSPI tft = TFT_eSPI();
Adafruit_BME680 bme;
WebServer server(80);

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;  // GMT+1
const int daylightOffset_sec = 3600;  // Summer time

// Hardware pins
const int ledPins[] = {13, 12, 14, 27, 26, 25, 33, 32};
const int numLeds = 8;
#define LED_BUILTIN 2

// Colors
#define C_BG TFT_BLACK
#define C_HEADER TFT_NAVY
#define C_TEXT TFT_WHITE
#define C_GRID 0x18E3
#define C_ACCENT TFT_ORANGE
#define C_GRAPH TFT_YELLOW

// Global variables
#define MAX_HISTORY 40
float tempHistory[MAX_HISTORY];
float pressureBaseline = 0;
String weatherStatus = "Analysis..."; 

// Timers
unsigned long lastUpdate = 0;
unsigned long lastGraphUpdate = 0;
unsigned long lastClockUpdate = 0;

// Cache of recent readings (for WWW)
float lastTemp=0, lastHum=0, lastPres=0, lastGas=0;

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- AERO SENTRY V1.1 BOOT ---");

  // 1. Hardware Init
  for(int i=0; i<numLeds; i++) pinMode(ledPins[i], OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  // 2. TFT Init
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(C_BG);

  // Loading Screen
  tft.setTextColor(TFT_WHITE, C_BG);
  tft.drawCentreString("SYSTEM START...", 120, 120, 2);
  tft.drawCentreString("Connecting to WiFi...", 120, 140, 1);

  // 3. WiFi Init
  WiFi.begin(ssid, password);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500); retry++;
  }

  if(WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    server.on("/", handleRoot);
    server.begin();
    Serial.println("[OK] WiFi Connected");
    Serial.print("[INFO] Server IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[ERROR] No WiFi - offline mode");
  }

  // 4. BME680 Init
  if(!bme.begin(0x76)) {
    tft.fillScreen(TFT_RED);
    tft.setTextColor(TFT_WHITE);
    tft.drawCentreString("BME680 ERROR!", 120, 160, 4);
    Serial.println("[ERROR] BME680 sensor not detected!");
    while(1);
  }

  // BME Configuration (Heat profile)
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);
  Serial.println("[OK] Sensor calibrated!");

  // 5. Start UI
  tft.fillScreen(C_BG);
  for(int i=0; i<MAX_HISTORY; i++) tempHistory[i] = 0;
  // Chart reset
  drawStaticInterface();

  // Displaying the IP address on the screen (if connected)
  if(WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(TFT_CYAN, C_HEADER);
    tft.drawCentreString(WiFi.localIP().toString(), 120, 25, 1);
  }
}

void loop() {
  server.handleClient();  // Web service

  // Clock (every 1 second)
  if (millis() - lastClockUpdate > 1000) {
    lastClockUpdate = millis();
    updateClock();
  }

  // Data reading (every 3 seconds)
  if (millis() - lastUpdate > 3000) {
    lastUpdate = millis();
    if (bme.performReading()) {
      lastTemp = bme.temperature;
      lastHum = bme.humidity;
      lastPres = bme.pressure / 100.0;
      lastGas = bme.gas_resistance / 1000.0;

      updateValues(lastTemp, lastHum, lastPres, lastGas);
      
      int iaq = mapGasToPercent(lastGas);
      updateLedBar(iaq);
      updateForecast(lastPres);
    }
  }

  // History Chart
  if (millis() - lastGraphUpdate > GRAPH_INTERVAL) {
    lastGraphUpdate = millis();
    if (lastTemp != 0) {
      addGraphPoint(lastTemp);
      drawGraph();
    }
  }

  runHeartbeat(); 
}

// --- UI FUNCTIONS ---

void drawStaticInterface() {
  // Header
  tft.fillRect(0, 0, 240, 35, C_HEADER);
  tft.drawFastHLine(0, 35, 240, C_ACCENT);
  
  tft.setTextColor(C_TEXT, C_HEADER);
  tft.drawString("AERO", 10, 8, 4); 
  
  // Temp Section
  tft.setTextColor(TFT_SILVER, C_BG);
  tft.drawString("TEMP", 10, 45, 2);

  // Chart Section
  int graphY = 110; 
  tft.drawRect(10, graphY, 220, 60, C_GRID); 
  tft.drawString("HISTORY", 10, graphY - 15, 1);

  // Bottom Section
  int detailsLineY = 178; 
  tft.drawLine(10, detailsLineY, 230, detailsLineY, TFT_DARKGREY);
  
  tft.drawString("HUMIDITY", 20, detailsLineY + 6, 2);
  tft.drawString("PRESSURE", 130, detailsLineY + 6, 2);

  // IAQ Footer
  tft.drawFastHLine(0, 235, 240, C_ACCENT); 
  
  // POPRAWKA 1: Przesunięcie napisu "AIR QUALITY" z 10 na 20, żeby wyrównać z ramką
  tft.drawString("AIR QUALITY", 20, 242, 2);
  tft.drawRect(20, 265, 200, 15, TFT_WHITE);
}

void updateClock() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return;
  char timeStr[6];
  strftime(timeStr, 6, "%H:%M", &timeinfo);
  
  tft.setTextColor(TFT_CYAN, C_HEADER);
  tft.setTextDatum(TR_DATUM); 
  tft.drawString(timeStr, 230, 8, 4); 
  tft.setTextDatum(TL_DATUM); 
}

void updateValues(float t, float h, float p, float g) {
  // Temp
  tft.setTextColor(TFT_YELLOW, C_BG);
  tft.setTextPadding(140);
  tft.drawFloat(t, 1, 70, 45, 6); 
  tft.setTextPadding(0);

  // Humidity and Pressure
  int valY = 205; 
  
  tft.setTextColor(TFT_CYAN, C_BG);
  tft.setTextPadding(60);
  String humStr = String(h, 0) + " %";
  tft.drawString(humStr, 20, valY, 4);
  
  tft.setTextColor(TFT_MAGENTA, C_BG);
  tft.setTextPadding(80);
  tft.drawFloat(p, 0, 130, valY, 4);
  tft.setTextPadding(0);

  // IAQ Bar and Text
  int barWidth = map(mapGasToPercent(g), 0, 100, 0, 196);
  uint16_t color = TFT_GREEN;
  String status = "GOOD";
  
  if(barWidth > 60) { color = TFT_ORANGE; status = "MODERATE"; }
  if(barWidth > 120) { color = TFT_RED; status = "BAD"; }

  tft.fillRect(22, 267, barWidth, 11, color);
  tft.fillRect(22 + barWidth, 267, 196 - barWidth, 11, C_BG);
  
  tft.setTextColor(TFT_WHITE, C_BG);
  tft.setTextPadding(240);
  tft.drawCentreString(status, 120, 285, 2);
  tft.setTextPadding(0);
}

// --- GRAPH LOGIC ---
void addGraphPoint(float val) {
  for (int i = 0; i < MAX_HISTORY - 1; i++) tempHistory[i] = tempHistory[i+1];
  tempHistory[MAX_HISTORY - 1] = val;
}

void drawGraph() {
  int xBase = 11; int yBase = 168; int gHeight = 56; 
  int colWidth = 218 / MAX_HISTORY;
  
  float minVal = 100; float maxVal = -100;
  for(int i=0; i<MAX_HISTORY; i++) {
    if(tempHistory[i] == 0) continue;
    if(tempHistory[i] < minVal) minVal = tempHistory[i];
    if(tempHistory[i] > maxVal) maxVal = tempHistory[i];
  }
  
  if ((maxVal - minVal) < 1.0) { 
      float mid = (maxVal+minVal)/2; maxVal=mid+1; minVal=mid-1; 
  } else { maxVal+=0.2; minVal-=0.2; }

  tft.fillRect(11, 111, 218, 58, C_BG); 
  for (int i = 0; i < MAX_HISTORY - 1; i++) {
    if (tempHistory[i] == 0 || tempHistory[i+1] == 0) continue;
    int y1 = map(tempHistory[i]*10, minVal*10, maxVal*10, yBase, yBase - gHeight);
    int y2 = map(tempHistory[i+1]*10, minVal*10, maxVal*10, yBase, yBase - gHeight);
    y1 = constrain(y1, yBase - gHeight, yBase);
    y2 = constrain(y2, yBase - gHeight, yBase);
    tft.drawLine(xBase + (i * colWidth), y1, xBase + ((i+1) * colWidth), y2, C_GRAPH);
  }
}

// --- FORECAST ---
void updateForecast(float p) {
  if (pressureBaseline == 0) pressureBaseline = p;
  
  // POPRAWKA 2: Przesunięcie ikony w prawo (z 195 na 215)
  int iconX = 215; 
  int iconY = 195;
  
  float diff = p - pressureBaseline;
  pressureBaseline = (pressureBaseline * 0.95) + (p * 0.05);

  tft.fillRect(iconX - 12, iconY - 12, 24, 24, C_BG);
  if (diff > 0.2) {
    weatherStatus = "Growing (Sun)";
    tft.fillCircle(iconX, iconY, 8, TFT_YELLOW);
    tft.drawLine(iconX, iconY-11, iconX, iconY+11, TFT_YELLOW);
    tft.drawLine(iconX-11, iconY, iconX+11, iconY, TFT_YELLOW);
  } else if (diff < -0.2) {
    weatherStatus = "Falling (Rain)";
    tft.fillCircle(iconX, iconY, 8, TFT_DARKGREY);
    tft.drawLine(iconX-4, iconY+10, iconX-6, iconY+14, TFT_CYAN);
  } else {
    weatherStatus = "Stable";
    tft.drawLine(iconX-8, iconY, iconX+8, iconY, TFT_GREEN);
  }
}

// --- WWW SERVER ---
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='10'>";
  html += "<title>AeroSentry</title>";
  html += "<style>body{font-family:sans-serif; text-align:center; background:#121212; color:#fff; margin:0; padding:20px;}";
  html += ".box{background:#1e1e1e; margin:10px auto; padding:20px; max-width:300px; border-radius:12px; border:1px solid #333;}";
  html += "h1{color:#ff9800; margin-bottom:5px;} h2{font-size:3rem; margin:10px 0; color:#00e5ff;}";
  html += ".lbl{color:#888; font-size:0.9rem;} .stat{font-weight:bold; color:#76ff03;}</style></head>";
  
  html += "<body><h1>AERO SENTRY</h1><p class='lbl'>Status Live</p>";
  html += "<div class='box'><div class='lbl'>TEMPERATURE</div><h2>" + String(lastTemp, 1) + "&deg;C</h2></div>";
  html += "<div class='box'><div class='lbl'>HUMIDITY</div><h2>" + String(lastHum, 0) + "%</h2></div>";
  html += "<div class='box'><div class='lbl'>PRESSURE</div><h2>" + String(lastPres, 0) + " hPa</h2><p>" + weatherStatus + "</p></div>";
  
  String qColor = (mapGasToPercent(lastGas) > 60) ? "#ff3d00" : "#76ff03";
  html += "<div class='box' style='border-color:" + qColor + "'><div class='lbl'>INDOOR AIR QUALITY (IAQ)</div>";
  html += "<h2 style='color:" + qColor + "'>" + String(mapGasToPercent(lastGas)) + "%</h2></div>";
  
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// --- UTILS ---
int mapGasToPercent(float gas_kohm) {
  float clean = 150.0; float bad = 5.0;
  if (gas_kohm > clean) gas_kohm = clean; if (gas_kohm < bad) gas_kohm = bad;
  return (int)((clean - gas_kohm) * (100.0) / (clean - bad));
}

void updateLedBar(int percent) {
  if(percent < 0) percent = 0; if(percent > 100) percent = 100;
  int ledsOn = map(percent, 0, 100, 0, numLeds + 1);
  if(percent > 90 && (millis()/200)%2==0) { 
     for(int i=0; i<numLeds; i++) digitalWrite(ledPins[i], LOW); return;
  }
  for(int i=0; i<numLeds; i++) digitalWrite(ledPins[i], (i < ledsOn) ? HIGH : LOW);
}

void runHeartbeat() {
  digitalWrite(LED_BUILTIN, ((millis() % 2000) < 100) ? HIGH : LOW);
}