#include "Config.h"
#include "StateManager.h"
#include "WiFiManager.h"
#include "SensorReader.h"
#include "HTTPSClientWrapper.h"

#include <SSD1306Wire.h>

StateManager       stateManager;
WiFiManager        wifiManager;
SensorReader       sensorReader;
HTTPSClientWrapper httpsClient;
SSD1306Wire        oled(0x3C, 5, 4);

static unsigned long lastSensorRead   = 0;
static unsigned long lastDataSend     = 0;
static unsigned long lastOledUpdate   = 0;
static unsigned long lastOledRead     = 0;
static unsigned long lastBuzzerToggle = 0;

static bool buzzerActive   = false;
static int  oledCurrentPPM = 0;
static int  oledTargetPPM  = 0;

static int          latestPPM = 0;
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
    String offlineMsg = "Offline";
    if (httpsClient.queuedCount() > 0)
      offlineMsg += " Q:" + String(httpsClient.queuedCount());
    oled.drawString(64, 52, offlineMsg);
  } else {
    oled.drawString(64, 52, "Online");
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

void setup() {
  Serial.begin(57600);
  Serial.println("\n[BOOT] Gas Leak Detector v1.0");

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  oled.init();
  oled.flipScreenVertically();
  oled.setContrast(255);
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.setTextAlignment(TEXT_ALIGN_CENTER);
  oled.drawString(64, 20, "Gas Leak Detector");
  oled.drawString(64, 34, "v" FIRMWARE_VERSION);
  oled.display();

  stateManager.init();
  sensorReader.init();
  wifiManager.init();
  httpsClient.init();

  params = wifiManager.getResolvedParams();
  Serial.printf("[CFG] interval=%dms warn>=%d danger>=%d buzzer>=%d\n",
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
  wifiManager.handleClient();
  httpsClient.update();
  stateManager.update();

  const State currentState = stateManager.getState();

  if (currentState == STATE_CONNECTING && wifiManager.checkConnection()) {
    stateManager.setState(STATE_CONNECTED);
    httpsClient.reloadConfig();
    params = wifiManager.getResolvedParams();
    Serial.println("[WiFi] Connected");
  }

  if (currentState == STATE_CONNECTED && !wifiManager.isConnected()) {
    stateManager.setState(STATE_CONNECTING);
    wifiManager.connectToWiFi();
    Serial.println("[WiFi] Reconnecting...");
  }

  if (millis() - lastOledRead >= 200) {
    lastOledRead  = millis();
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
        Serial.printf("[DRAIN] queue=%d\n", httpsClient.queuedCount());
      } else {
        httpsClient.sendSingle(latestPPM);
        Serial.printf("[SEND] ppm=%d\n", latestPPM);
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
    if      (oledCurrentPPM < oledTargetPPM) oledCurrentPPM = min(oledCurrentPPM + 20, oledTargetPPM);
    else if (oledCurrentPPM > oledTargetPPM) oledCurrentPPM = max(oledCurrentPPM - 20, oledTargetPPM);
    drawOLED(oledCurrentPPM);
  }

  updateBuzzer(oledTargetPPM);

  yield();
}
