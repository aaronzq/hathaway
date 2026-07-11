#pragma once

#include <Arduino.h>
#include "debounce.h"

class SwitchHandler {
public:
    SwitchHandler();
    SwitchHandler(int p, unsigned long delay = DEFAULT_DEBOUNCE_TIME,
                  bool pol = HIGH);

    bool update();
    bool getState() const;

private:
    int        PIN; 
    Debouncer  debouncer;
    bool       polarity;
};