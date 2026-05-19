#include "Config.h"
#include "StateManager.h"
#include "WiFiManager.h"
#include "SensorReader.h"
#include "HTTPSClientWrapper.h"
#include <SSD1306Wire.h>
#include <Wire.h>
#include <ESP8266WiFi.h>

#define I2C_SDA 4
#define I2C_SCL 5
#define OLED_ADDR 0x3C
#define I2C_CLOCK 400000

StateManager       stateManager;
WiFiManager        wifiManager;
SensorReader       sensorReader;
HTTPSClientWrapper httpsClient;
SSD1306Wire        oled(OLED_ADDR, I2C_SDA, I2C_SCL);

static unsigned long lastSensorRead   = 0;
static unsigned long lastDataSend     = 0;
static unsigned long lastOledUpdate   = 0;
static unsigned long lastOledRead     = 0;
static unsigned long lastBuzzerToggle = 0;

static bool buzzerActive   = false;
static int  oledCurrentPPM = 0;
static int  oledTargetPPM  = 0;
static int  latestPPM      = 0;

static DeviceParams params;

static const char* statusLabel(int ppm) {
  if (ppm >= params.ppmDanger)  return "DANGER!";
  if (ppm >= params.ppmWarning) return "Warning";
  return "Normal";
}

static void drawOLED(int ppm) {
  oled.clear();
  oled.setFont(ArialMT_Plain_24);
  oled.setTextAlignment(TEXT_ALIGN_CENTER);
  oled.drawString(64, 4, String(ppm) + " PPM");
  oled.drawLine(0, 36, 127, 36);
  oled.setFont(ArialMT_Plain_10);
  oled.drawString(64, 38, String("Status: ") + statusLabel(ppm));

  if (WiFi.status() != WL_CONNECTED) {
    String offlineMsg = F("Offline");
    if (httpsClient.queuedCount() > 0)
      offlineMsg += F(" Q:") + String(httpsClient.queuedCount());
    oled.drawString(64, 52, offlineMsg);
  } else {
    oled.drawString(64, 52, F("Online"));
  }
  oled.display();
}

static void updateBuzzer(int ppm) {
  if (ppm >= params.ppmBuzzer) {
    if (millis() - lastBuzzerToggle >= 300) {
      buzzerActive = !buzzerActive;
      digitalWrite(PIN_BUZZER, buzzerActive);
      lastBuzzerToggle = millis();
    }
  } else {
    digitalWrite(PIN_BUZZER, LOW);
    buzzerActive = false;
  }
}

static void enqueueIfSlow(int ppm) {
  static unsigned long busyStartMs = 0;
  if (busyStartMs == 0) busyStartMs = millis();
  if (millis() - busyStartMs > 2000) {
    httpsClient.enqueue(ppm);
    busyStartMs = 0;
  }
}

static bool initOLED() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_CLOCK);
  
  for (int i = 0; i < 3; i++) {
    oled.init();
    Wire.beginTransmission(OLED_ADDR);
    if (Wire.endTransmission() == 0) {
      oled.flipScreenVertically();
      oled.setContrast(255);
      oled.clear();
      oled.setFont(ArialMT_Plain_10);
      oled.setTextAlignment(TEXT_ALIGN_CENTER);
      oled.drawString(64, 20, F("Gas Leak Detector"));
      oled.drawString(64, 34, String(F("v")) + FIRMWARE_VERSION);
      oled.display();
      return true;
    }
    delay(100);
  }
  return false;
}

void setup() {
  Serial.begin(57600);
  Serial.println(F("\n[BOOT] Gas Leak Detector v" FIRMWARE_VERSION));
  
  ESP.wdtEnable(5000);
  
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  if (!initOLED()) {
    Serial.println(F("[OLED] Init failed"));
  }

  stateManager.init();
  sensorReader.init();
  wifiManager.init();
  httpsClient.init();

  params = wifiManager.getResolvedParams();
  Serial.printf_P(PSTR("[CFG] interval=%dms warn>=%d danger>=%d buzzer>=%d\n"),
    params.sendInterval, params.ppmWarning, params.ppmDanger, params.ppmBuzzer);

  delay(1000);
  drawOLED(0);

  stateManager.setState(STATE_AP_MODE);
  if (wifiManager.hasConfig()) {
    stateManager.setState(STATE_CONNECTING);
    wifiManager.connectToWiFi();
  }
}

void loop() {
  ESP.wdtFeed();
  
  if (ESP.getFreeHeap() < 5000) {
    Serial.println(F("[ERROR] Low heap, restarting"));
    ESP.restart();
  }
  
  wifiManager.handleClient();
  httpsClient.update();
  stateManager.update();

  const State currentState = stateManager.getState();

  if (currentState == STATE_CONNECTING && wifiManager.checkConnection()) {
    stateManager.setState(STATE_CONNECTED);
    httpsClient.reloadConfig();
    params = wifiManager.getResolvedParams();
    Serial.println(F("[WiFi] Connected"));
  }

  if (currentState == STATE_CONNECTED && !wifiManager.isConnected()) {
    stateManager.setState(STATE_CONNECTING);
    wifiManager.connectToWiFi();
    Serial.println(F("[WiFi] Reconnecting..."));
  }

  if (millis() - lastOledRead >= 200) {
    lastOledRead = millis();
    oledTargetPPM = map(analogRead(PIN_SENSOR), 0, 1023, 0, 1000);
  }

  if (millis() - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = millis();
    sensorReader.read();
    latestPPM = sensorReader.getData().ppm;
  }

  if (currentState == STATE_CONNECTED &&
      millis() - lastDataSend >= (unsigned long)params.sendInterval) {
    lastDataSend = millis();
    if (!httpsClient.isBusy()) {
      if (httpsClient.queuedCount() > 0) {
        httpsClient.drainOne();
        Serial.printf_P(PSTR("[DRAIN] queue=%d\n"), httpsClient.queuedCount());
      } else {
        httpsClient.sendSingle(latestPPM);
        Serial.printf_P(PSTR("[SEND] ppm=%d\n"), latestPPM);
      }
    } else {
      enqueueIfSlow(latestPPM);
    }
  }

  if (currentState != STATE_CONNECTED &&
      millis() - lastDataSend >= (unsigned long)params.sendInterval) {
    lastDataSend = millis();
    httpsClient.enqueue(latestPPM);
  }

  if (millis() - lastOledUpdate >= 50) {
    lastOledUpdate = millis();
    if (oledCurrentPPM < oledTargetPPM)
      oledCurrentPPM = min(oledCurrentPPM + 20, oledTargetPPM);
    else if (oledCurrentPPM > oledTargetPPM)
      oledCurrentPPM = max(oledCurrentPPM - 20, oledTargetPPM);
    drawOLED(oledCurrentPPM);
  }

  updateBuzzer(oledTargetPPM);
  yield();
}