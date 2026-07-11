#pragma once

#include <Arduino.h>

// default debounce interval in milliseconds
#define DEFAULT_DEBOUNCE_TIME 50 
//50

// ---------------------------------------------------------------------------
// Simple digital input debouncer.  The implementation is intentionally tiny
// as the reward‑conditioning firmware is very small.
// ---------------------------------------------------------------------------
class Debouncer {
public:
    explicit Debouncer(int p, unsigned long delay = DEFAULT_DEBOUNCE_TIME, int MODE = INPUT);

    bool update();
    bool getState() const;

private:
    int pin;
    bool state;
    unsigned long lastDebounceTime;
    unsigned long debounceDelay;
};