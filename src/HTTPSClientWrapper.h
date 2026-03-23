#ifndef HTTPS_CLIENT_WRAPPER_H
#define HTTPS_CLIENT_WRAPPER_H

#include "Config.h"
#include <WiFiClientSecure.h>

// Non-blocking HTTPS client with persistent TLS keep-alive.
//
// Design rationale:
//   A fresh TLS handshake costs 1–2 s, making 500 ms send intervals impossible.
//   This class holds the socket open between requests and reuses it,
//   cutting per-request overhead to ~50–100 ms.
//
// Offline queue:
//   When the device is disconnected, readings are buffered in a circular
//   queue (capacity: MAX_OFFLINE_QUEUE). On reconnect, queued entries are
//   drained one per send interval, oldest first.
class HTTPSClientWrapper {
private:
  DeviceConfig      config;
  WiFiClientSecure* client = nullptr;

  bool          requestInProgress = false;
  unsigned long requestStartTime  = 0;

  QueueEntry offlineQueue[MAX_OFFLINE_QUEUE];
  int        queueHead = 0;
  int        queueSize = 0;

  String pendingPayload;
  bool   hasPending  = false;
  int    retryCount  = 0;
  static const int MAX_RETRIES = 1;

  // ── Non-blocking request state machine ───────────────────────────────────

  enum RequestState {
    REQ_IDLE,
    REQ_SENDING,
    REQ_WAITING,
    REQ_READ_STATUS,
    REQ_READ_HEADERS,
    REQ_READ_BODY
  };
  RequestState reqState      = REQ_IDLE;
  bool         reqSuccess    = false;
  int          contentLength = 0;
  int          bodyRead      = 0;

  // ── Helpers ───────────────────────────────────────────────────────────────

  const char* effectiveDeviceId() const {
    return (config.deviceId[0] != '\0') ? config.deviceId : DEVICE_ID;
  }

  const char* apiHost() const {
    return (config.apiUrl[0] != '\0') ? config.apiUrl : "";
  }

  // Builds the JSON payload for a single reading.
  // Optional fields (name, lat, lng) are omitted when not configured.
  String buildPayload(int ppm) const {
    StaticJsonDocument<256> doc;
    doc["device_id"] = effectiveDeviceId();
    doc["ppm"]       = ppm;
    if (config.name[0] != '\0')
      doc["name"] = config.name;
    if (config.latitude != 0.0 || config.longitude != 0.0) {
      doc["lat"] = config.latitude;
      doc["lng"] = config.longitude;
    }
    String payload;
    serializeJson(doc, payload);
    return payload;
  }

  // ── Socket management ─────────────────────────────────────────────────────

  bool ensureConnected() {
    if (client && client->connected()) return true;
    closeSocket();

    if (WiFi.status() != WL_CONNECTED) return false;
    if (ESP.getFreeHeap() < 8000)       return false;
    if (apiHost()[0] == '\0')           return false;

    client = new WiFiClientSecure();
    client->setInsecure();           // certificate not validated; API key is the auth layer
    client->setBufferSizes(2048, 512);

    if (!client->connect(apiHost(), 443)) {
      delete client;
      client = nullptr;
      return false;
    }
    Serial.println("[HTTP] TLS connected");
    return true;
  }

  void closeSocket() {
    if (client) { client->stop(); delete client; client = nullptr; }
    requestInProgress = false;
    reqState          = REQ_IDLE;
  }

  bool startRequest(const String& payload) {
    if (!ensureConnected()) return false;
    pendingPayload    = payload;
    requestInProgress = true;
    reqState          = REQ_SENDING;
    requestStartTime  = millis();
    hasPending        = true;
    return true;
  }

  // ── State machine tick (called every loop) ────────────────────────────────

  void processRequest() {
    if (!client || !client->connected()) {
      Serial.println("[HTTP] Socket dropped");
      onRequestDone(false);
      return;
    }

    switch (reqState) {

      case REQ_SENDING: {
        String req  = "POST ";
        req        += API_ENDPOINT;
        req        += " HTTP/1.1\r\nHost: ";
        req        += apiHost();
        req        += "\r\nUser-Agent: ESP8266/" FIRMWARE_VERSION;
        req        += "\r\nContent-Type: application/json";
        req        += "\r\nX-API-Key: ";
        req        += config.apiKey;
        req        += "\r\nContent-Length: ";
        req        += String(pendingPayload.length());
        req        += "\r\nConnection: keep-alive\r\n\r\n";
        req        += pendingPayload;
        client->print(req);
        client->flush();
        reqState         = REQ_WAITING;
        requestStartTime = millis();
        break;
      }

      case REQ_WAITING:
        if (client->available()) {
          reqState = REQ_READ_STATUS;
        } else if (millis() - requestStartTime > REQUEST_TIMEOUT) {
          Serial.println("[HTTP] Timeout");
          onRequestDone(false);
        }
        break;

      case REQ_READ_STATUS: {
        if (!client->available()) break;
        String statusLine = client->readStringUntil('\n');
        reqSuccess    = (statusLine.indexOf("200") > 0);
        contentLength = 0;
        bodyRead      = 0;
        reqState      = REQ_READ_HEADERS;
        break;
      }

      case REQ_READ_HEADERS: {
        if (!client->available()) break;
        String header = client->readStringUntil('\n');
        header.trim();
        if (header.startsWith("Content-Length:"))
          contentLength = header.substring(15).toInt();
        if (header.length() == 0) reqState = REQ_READ_BODY;
        break;
      }

      case REQ_READ_BODY: {
        while (client->available() && bodyRead < contentLength) {
          client->read();
          bodyRead++;
        }
        if (bodyRead >= contentLength) {
          // Socket stays open; reset state for the next request.
          requestInProgress = false;
          reqState          = REQ_IDLE;
          if (reqSuccess) {
            hasPending = false;
            retryCount = 0;
          } else {
            retryCount++;
            if (retryCount <= MAX_RETRIES) {
              Serial.printf("[HTTP] Retry %d\n", retryCount);
              startRequest(pendingPayload);
            } else {
              Serial.println("[HTTP] Dropping request after max retries");
              hasPending = false;
              retryCount = 0;
            }
          }
        }
        break;
      }

      default: break;
    }
  }

  void onRequestDone(bool success) {
    closeSocket();
    if (!success) {
      retryCount++;
      if (retryCount <= MAX_RETRIES) {
        Serial.printf("[HTTP] Retry %d\n", retryCount);
        startRequest(pendingPayload);
        return;
      }
      Serial.println("[HTTP] Dropping request after max retries");
    }
    hasPending = false;
    retryCount = 0;
  }

  // ── EEPROM sanitisation ───────────────────────────────────────────────────

  // Clears fields that contain garbage bytes (non-printable ASCII).
  // This guards against EEPROM corruption when the struct layout changes.
  void sanitizeConfig() {
    auto isPrintableStr = [](const char* s, int maxLen) -> bool {
      if (s[0] == '\0' || s[0] == (char)0xFF) return true;
      for (int i = 0; i < maxLen && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E) return false;
      }
      return true;
    };

    if (!isPrintableStr(config.deviceId, sizeof(config.deviceId)))
      memset(config.deviceId, 0, sizeof(config.deviceId));
    if (!isPrintableStr(config.name, sizeof(config.name)))
      memset(config.name, 0, sizeof(config.name));

    if (isnan(config.latitude)  || config.latitude  < -90  || config.latitude  > 90)
      config.latitude  = 0.0;
    if (isnan(config.longitude) || config.longitude < -180 || config.longitude > 180)
      config.longitude = 0.0;
  }

public:
  // ── Public API ────────────────────────────────────────────────────────────

  void init() {
    EEPROM.get(0, config);
    sanitizeConfig();
    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    int attempts = 0;
    while (time(nullptr) < 100000 && attempts++ < 10) { delay(500); yield(); }
    Serial.printf("[HTTP] NTP sync %s\n", time(nullptr) >= 100000 ? "OK" : "FAILED");
  }

  // Re-reads config from EEPROM and closes the socket so the next request
  // reconnects to the (possibly new) host.
  void reloadConfig() {
    EEPROM.get(0, config);
    sanitizeConfig();
    closeSocket();
  }

  // Enqueues a reading for offline buffering.
  // Overwrites the oldest entry when the queue is full.
  void enqueue(int ppm) {
    int idx = (queueHead + queueSize) % MAX_OFFLINE_QUEUE;
    if (queueSize >= MAX_OFFLINE_QUEUE) {
      offlineQueue[queueHead] = { ppm, (long)(millis() / 1000) };
      queueHead = (queueHead + 1) % MAX_OFFLINE_QUEUE;
    } else {
      offlineQueue[idx] = { ppm, (long)(millis() / 1000) };
      queueSize++;
    }
  }

  int queuedCount() const { return queueSize; }

  // Sends the oldest queued entry. Returns false if nothing to drain or busy.
  bool drainOne() {
    if (queueSize == 0 || requestInProgress) return false;
    int ppm   = offlineQueue[queueHead].ppm;
    queueHead = (queueHead + 1) % MAX_OFFLINE_QUEUE;
    queueSize--;
    return startRequest(buildPayload(ppm));
  }

  // Sends a live reading. Enqueues it instead if a request is in flight.
  bool sendSingle(int ppm) {
    if (requestInProgress) { enqueue(ppm); return false; }
    return startRequest(buildPayload(ppm));
  }

  // Must be called every loop iteration to advance the state machine.
  void update() {
    if (requestInProgress) processRequest();
  }

  bool isBusy() const { return requestInProgress; }
};

#endif
