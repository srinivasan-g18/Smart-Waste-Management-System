/*
  =====================================================================
  IoT-Based Smart Waste Management System
  =====================================================================
  Author  : Srinivasan G (Biomedical Engineering)
  Date    : April 2025

  Description:
  Monitors the fill level of three waste bins (LOW / MEDIUM / HIGH)
  using ultrasonic distance sensors, shows live status on a 16x2 I2C
  LCD, sounds/lights a local alert when a bin is full, and hosts a
  built-in Wi-Fi web dashboard (JSON + HTML) so fill levels can be
  viewed remotely from a phone or laptop browser - matching the
  "Smart Waste Management System" dashboard shown in the project
  photos.

  ---------------------------------------------------------------------
  HARDWARE CONNECTIONS (ESP32 Dev Board)
  ---------------------------------------------------------------------
  BIN 1 - HC-SR04 Ultrasonic Sensor
    VCC  -> 5V
    GND  -> GND
    TRIG -> GPIO 5
    ECHO -> GPIO 18

  BIN 2 - HC-SR04 Ultrasonic Sensor
    VCC  -> 5V
    GND  -> GND
    TRIG -> GPIO 19
    ECHO -> GPIO 21

  BIN 3 - HC-SR04 Ultrasonic Sensor
    VCC  -> 5V
    GND  -> GND
    TRIG -> GPIO 22
    ECHO -> GPIO 23

  16x2 I2C LCD Display
    SDA  -> GPIO 21 *shared I2C bus - see note below*
    SCL  -> GPIO 22 *shared I2C bus - see note below*
    VCC  -> 5V
    GND  -> GND

  Relay / Buzzer Module (Bin Full alert)
    IN   -> GPIO 4
    VCC  -> 5V
    GND  -> GND

  NOTE ON PIN CONFLICTS:
  ESP32 default I2C pins (GPIO 21/22) are reused above for Bin 2/Bin 3
  ultrasonic sensors as an example layout only. On your actual board,
  choose any free GPIOs for the ultrasonic TRIG/ECHO pins that do NOT
  clash with your LCD's SDA/SCL lines, and update the pin constants
  below accordingly. Any digital-capable GPIO pair works for HC-SR04.

  ---------------------------------------------------------------------
  REQUIRED LIBRARIES (install via Arduino Library Manager)
  ---------------------------------------------------------------------
    1. "LiquidCrystal_I2C" (by Frank de Brabander or John Rickman)
    2. ESP32 Board Package (Boards Manager: "esp32" by Espressif Systems)
       WiFi.h and WebServer.h are included with the ESP32 core.
  =====================================================================
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>

// ---------------- Wi-Fi Credentials ------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ---------------- LCD Setup (I2C address, columns, rows) ---------------
// Common I2C addresses are 0x27 or 0x3F - check with an I2C scanner if
// the display shows nothing.
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------- Ultrasonic Sensor Pins --------------------------------
const int TRIG_PIN[3] = {5, 19, 22};
const int ECHO_PIN[3] = {18, 21, 23};

// ---------------- Alert Output ------------------------------------------
const int ALERT_PIN = 4; // relay / buzzer / LED for "Bin Full" alert

// ---------------- Bin Calibration ---------------------------------------
// BIN_HEIGHT_CM = distance (cm) from sensor to the EMPTY bin floor.
// Adjust this to match your physical bin dimensions.
const float BIN_HEIGHT_CM = 30.0;

// Fill-level thresholds (percentage of bin capacity)
const int THRESHOLD_MEDIUM = 50; // >= 50% = MEDIUM
const int THRESHOLD_HIGH   = 85; // >= 85% = HIGH / FULL

// ---------------- Web Server ---------------------------------------------
WebServer server(80);

// ---------------- Bin Data -------------------------------------------------
struct BinData {
  float distanceCm;
  int   fillPercent;
  String status;
};
BinData bins[3];

const char* BIN_NAMES[3] = {"BIN 1", "BIN 2", "BIN 3"};

// Reporting / display refresh interval
const uint32_t UPDATE_INTERVAL_MS = 2000;
uint32_t lastUpdate = 0;

// Which bin is currently shown on the LCD (cycles through 0,1,2)
int lcdBinIndex = 0;
uint32_t lastLcdSwitch = 0;
const uint32_t LCD_SWITCH_INTERVAL_MS = 2500;

void setup() {
  Serial.begin(115200);

  // Configure ultrasonic sensor pins
  for (int i = 0; i < 3; i++) {
    pinMode(TRIG_PIN[i], OUTPUT);
    pinMode(ECHO_PIN[i], INPUT);
    digitalWrite(TRIG_PIN[i], LOW);
  }

  pinMode(ALERT_PIN, OUTPUT);
  digitalWrite(ALERT_PIN, LOW);

  // Initialize LCD
  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart Waste Sys");
  lcd.setCursor(0, 1);
  lcd.print("Monitoring...");

  connectToWiFi();
  setupWebServer();

  delay(1500);
  lcd.clear();
}

void loop() {
  server.handleClient();

  if (millis() - lastUpdate > UPDATE_INTERVAL_MS) {
    updateAllBins();
    printToSerial();
    checkAlerts();
    lastUpdate = millis();
  }

  // Cycle the LCD display between the three bins
  if (millis() - lastLcdSwitch > LCD_SWITCH_INTERVAL_MS) {
    lcdBinIndex = (lcdBinIndex + 1) % 3;
    displayBinOnLCD(lcdBinIndex);
    lastLcdSwitch = millis();
  }
}

// -----------------------------------------------------------------------
// Connects to Wi-Fi so the web dashboard can be reached on the local
// network (prints the assigned IP address to the Serial Monitor).
// -----------------------------------------------------------------------
void connectToWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected! IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("WiFi connection failed - running without dashboard.");
  }
}

// -----------------------------------------------------------------------
// Measures distance (cm) from an HC-SR04 ultrasonic sensor.
// -----------------------------------------------------------------------
float readDistanceCm(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 30000); // 30ms timeout (~5m range)
  if (duration == 0) {
    return BIN_HEIGHT_CM; // no echo received, assume bin empty
  }

  float distance = duration * 0.0343 / 2.0; // speed of sound = 343 m/s
  return distance;
}

// -----------------------------------------------------------------------
// Converts measured distance into a fill percentage and status label.
// -----------------------------------------------------------------------
void updateAllBins() {
  for (int i = 0; i < 3; i++) {
    float distance = readDistanceCm(TRIG_PIN[i], ECHO_PIN[i]);
    if (distance > BIN_HEIGHT_CM) distance = BIN_HEIGHT_CM;
    if (distance < 0) distance = 0;

    int fillPercent = (int)(((BIN_HEIGHT_CM - distance) / BIN_HEIGHT_CM) * 100);
    fillPercent = constrain(fillPercent, 0, 100);

    String status;
    if (fillPercent >= THRESHOLD_HIGH) {
      status = "HIGH";
    } else if (fillPercent >= THRESHOLD_MEDIUM) {
      status = "MEDIUM";
    } else {
      status = "LOW";
    }

    bins[i].distanceCm  = distance;
    bins[i].fillPercent = fillPercent;
    bins[i].status      = status;
  }
}

// -----------------------------------------------------------------------
// Triggers the relay/buzzer/LED alert if any bin is HIGH (full).
// -----------------------------------------------------------------------
void checkAlerts() {
  bool anyFull = false;
  for (int i = 0; i < 3; i++) {
    if (bins[i].status == "HIGH") anyFull = true;
  }
  digitalWrite(ALERT_PIN, anyFull ? HIGH : LOW);
}

// -----------------------------------------------------------------------
// Displays one bin's status on the 16x2 LCD.
// -----------------------------------------------------------------------
void displayBinOnLCD(int index) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(BIN_NAMES[index]);
  lcd.print(": ");
  lcd.print(bins[index].status);

  lcd.setCursor(0, 1);
  lcd.print("Level: ");
  lcd.print(bins[index].fillPercent);
  lcd.print("%");
}

// -----------------------------------------------------------------------
// Logs all bin readings to the Serial Monitor.
// -----------------------------------------------------------------------
void printToSerial() {
  for (int i = 0; i < 3; i++) {
    Serial.print(BIN_NAMES[i]);
    Serial.print(" -> Distance: ");
    Serial.print(bins[i].distanceCm);
    Serial.print(" cm | Fill: ");
    Serial.print(bins[i].fillPercent);
    Serial.print("% | Status: ");
    Serial.println(bins[i].status);
  }
  Serial.println("-----------------------------");
}

// -----------------------------------------------------------------------
// Sets up the built-in web dashboard:
//   GET /       -> simple auto-refreshing HTML dashboard
//   GET /data   -> JSON readings for external apps / mobile dashboards
// -----------------------------------------------------------------------
void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
}

void handleData() {
  String json = "{";
  for (int i = 0; i < 3; i++) {
    json += "\"bin" + String(i + 1) + "\":{";
    json += "\"percent\":" + String(bins[i].fillPercent) + ",";
    json += "\"status\":\"" + bins[i].status + "\"";
    json += "}";
    if (i < 2) json += ",";
  }
  json += "}";
  server.send(200, "application/json", json);
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' "
                 "content='width=device-width, initial-scale=1'>"
                 "<meta http-equiv='refresh' content='3'>"
                 "<title>Smart Waste Management</title>"
                 "<style>"
                 "body{font-family:Arial;background:#0e1a2b;color:#fff;text-align:center;padding:20px;}"
                 "h1{color:#4fc3f7;}"
                 ".bin{display:inline-block;width:28%;margin:1%;padding:15px;"
                 "background:#152840;border-radius:10px;vertical-align:top;}"
                 ".LOW{color:#4caf50;} .MEDIUM{color:#ffc107;} .HIGH{color:#f44336;}"
                 ".bar{background:#2c3e50;border-radius:5px;height:10px;margin-top:8px;}"
                 ".fill{height:10px;border-radius:5px;}"
                 "</style></head><body>";

  html += "<h1>SMART WASTE MANAGEMENT SYSTEM</h1><div>";

  for (int i = 0; i < 3; i++) {
    String colorClass = bins[i].status;
    String barColor = (bins[i].status == "HIGH") ? "#f44336" :
                       (bins[i].status == "MEDIUM") ? "#ffc107" : "#4caf50";

    html += "<div class='bin'>";
    html += "<h3>" + String(BIN_NAMES[i]) + "</h3>";
    html += "<p class='" + colorClass + "'>" + bins[i].status + "</p>";
    html += "<div class='bar'><div class='fill' style='width:" +
            String(bins[i].fillPercent) + "%;background:" + barColor + ";'></div></div>";
    html += "<p>" + String(bins[i].fillPercent) + "%</p>";
    html += "</div>";
  }

  html += "</div><h3>COLLECTION ALERTS</h3>";
  bool anyFull = false;
  for (int i = 0; i < 3; i++) {
    if (bins[i].status == "HIGH") {
      html += "<p style='color:#f44336;'>" + String(BIN_NAMES[i]) +
              " is Full - Collection Required!</p>";
      anyFull = true;
    }
  }
  if (!anyFull) {
    html += "<p style='color:#4caf50;'>All bins within normal range.</p>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}
