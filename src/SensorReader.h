#ifndef SENSOR_READER_H
#define SENSOR_READER_H

#include "Config.h"

// Reads the MQ-6 gas sensor and exposes a SensorData snapshot.
// Temperature and humidity are placeholders; the MQ-6 does not provide them.
class SensorReader {
private:
  SensorData lastReading;

  // Maps the 10-bit ADC value (0–1023) to a PPM range of 0–1000.
  int readRawPPM() const {
    return map(analogRead(PIN_SENSOR), 0, 1023, 0, 1000);
  }

public:
  SensorReader() {
    memset(&lastReading, 0, sizeof(lastReading));
  }

  void init() {
    pinMode(PIN_SENSOR, INPUT);
    Serial.println("[SENSOR] MQ-6 ready");
  }

  void read() {
    lastReading.ppm         = readRawPPM();
    lastReading.level       = 0;
    lastReading.temperature = 25.0f;
    lastReading.humidity    = 60.0f;
    lastReading.timestamp   = millis() / 1000;
    Serial.printf("[SENSOR] ppm=%d\n", lastReading.ppm);
  }

  const SensorData& getData() const { return lastReading; }
};

#endif
