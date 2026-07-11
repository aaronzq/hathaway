#pragma once

#include <Arduino.h>
#include "debounce.h"

// ---------------------------------------------------------------------------
// Simple helper reading a lick detector and reporting debounced events.
// ``polarity`` defines the on state of the detector (HIGH or LOW).
// ---------------------------------------------------------------------------
class LickHandler {
public:
    LickHandler();
    LickHandler(int p, unsigned long delay = DEFAULT_DEBOUNCE_TIME,
                bool pol = HIGH);

    bool update();
    bool getState() const;

private:
    int        PIN;
    Debouncer  debouncer;
    bool       polarity;
};