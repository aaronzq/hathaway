#pragma once

#include <Arduino.h>
#include "debounce.h"

// ---------------------------------------------------------------------------
// Simple helper reading a lick detector and reporting debounced events.
// ``polarity`` defines the on state of the detector (HIGH or LOW).
// ---------------------------------------------------------------------------

extern unsigned long LICK_DEBOUNCE_TIME;

class LickHandler {
public:
    LickHandler();
    LickHandler(int p, unsigned long delay = LICK_DEBOUNCE_TIME,
                bool pol = HIGH);

    bool update();
    bool getState() const;
    void setDebounceTime(unsigned long delay);

private:
    int        PIN;
    Debouncer  debouncer;
    bool       polarity;
};
