#pragma once

#include <Arduino.h>

#define DEFAULT_FIX_DURATION 10000
#define DEFAULT_MAG_GRACE 0

class Magneto {
public:
    Magneto();
    Magneto(int pin, unsigned long duration = DEFAULT_FIX_DURATION);

    void magnetic_start(unsigned long duration = 0);
    bool update();
    void halt();
    void setFixDuration(unsigned long duration);  // update default hold time
    void setGraceDuration(unsigned long duration);
    bool haltAllowed() const;   // false while inside the post-start grace window

private:
    int magnetPin;
    unsigned long defaultFixDuration;
    bool isOn;
    unsigned long closeTime;
    unsigned long graceDuration;
    unsigned long graceEnd;
};