#ifndef CONFIG_H
#define CONFIG_H

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#define DEVICE_ID        "ESP_GASLEAK_01"
#define FIRMWARE_VERSION "1.0.0"

static const int PIN_LED    = 2;
static const int PIN_SENSOR = A0;
static const int PIN_BUZZER = 14;

static const char* AP_PASSWORD = "gasleakdetector";
static const byte  DNS_PORT    = 53;

static const int EEPROM_SIZE = 640;

static const unsigned long CONNECT_FAIL_BACKOFF = 5000;
static const unsigned long ERROR_RETRY_INTERVAL = 15000;
static const unsigned long REQUEST_TIMEOUT      = 8000;
static const unsigned long SENSOR_READ_INTERVAL = 400;

static const char* API_ENDPOINT = "/api/ingest";

static const int MAX_OFFLINE_QUEUE = 60;

static const int DEFAULT_SEND_INTERVAL = 200;
static const int DEFAULT_PPM_WARNING   = 300;
static const int DEFAULT_PPM_DANGER    = 800;
static const int DEFAULT_PPM_BUZZER    = 300;

struct DeviceParams {
  int sendInterval;
  int ppmWarning;
  int ppmDanger;
  int ppmBuzzer;
};

inline DeviceParams resolveParams(const DeviceParams& p) {
  DeviceParams resolved = p;
  if (resolved.sendInterval <= 0) resolved.sendInterval = DEFAULT_SEND_INTERVAL;
  if (resolved.ppmWarning   <= 0) resolved.ppmWarning   = DEFAULT_PPM_WARNING;
  if (resolved.ppmDanger    <= 0) resolved.ppmDanger    = DEFAULT_PPM_DANGER;
  if (resolved.ppmBuzzer    <= 0) resolved.ppmBuzzer    = DEFAULT_PPM_BUZZER;
  return resolved;
}

struct DeviceConfig {
  char ssid[32];
  char password[64];
  char apiKey[128];
  char token[40];
  bool valid;
  char apiUrl[128];
  DeviceParams params;
  char   deviceId[32];
  char   name[64];
  double latitude;
  double longitude;
};

enum State {
  STATE_AP_MODE,
  STATE_CONFIGURING,
  STATE_CONNECTING,
  STATE_CONNECTED,
  STATE_CONN_FAILED,
  STATE_TEST_MODE
};

struct SensorData {
  int           ppm;
  float         temperature;
  float         humidity;
  int           level;
  unsigned long timestamp;
};

struct QueueEntry {
  int  ppm;
  long unixTime;
};

#endif
