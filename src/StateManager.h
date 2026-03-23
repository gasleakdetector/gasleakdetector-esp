#ifndef STATE_MANAGER_H
#define STATE_MANAGER_H

#include "Config.h"

class StateManager {
private:
  State         currentState;
  unsigned long ledLastToggle;
  bool          ledState;

  void updateLED() {
    if (currentState == STATE_TEST_MODE) {
      toggleLEDEvery(150);
      return;
    }
    if (currentState == STATE_CONNECTED) {
      digitalWrite(PIN_LED, HIGH);
      return;
    }
    toggleLEDEvery(getLEDBlinkInterval());
  }

  void toggleLEDEvery(unsigned long intervalMs) {
    if (millis() - ledLastToggle >= intervalMs) {
      ledState = !ledState;
      digitalWrite(PIN_LED, ledState);
      ledLastToggle = millis();
    }
  }

  unsigned long getLEDBlinkInterval() const {
    switch (currentState) {
      case STATE_AP_MODE:     return 500;
      case STATE_CONNECTING:  return 200;
      case STATE_CONN_FAILED: return 100;
      default:                return 1000;
    }
  }

public:
  StateManager()
    : currentState(STATE_AP_MODE), ledLastToggle(0), ledState(false) {}

  void init() {
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    EEPROM.begin(EEPROM_SIZE);
  }

  void update() {
    updateLED();
  }

  State getState() const { return currentState; }

  void setState(State newState) {
    if (currentState == newState) return;
    if (currentState == STATE_TEST_MODE && newState != STATE_AP_MODE) return;
    currentState = newState;
  }

  bool isTestMode() const { return currentState == STATE_TEST_MODE; }
};

#endif
