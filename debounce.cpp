#include "debounce.h"

Debouncer::Debouncer(int p, unsigned long delay, int MODE)
    : pin(p), state(digitalRead(p)), lastDebounceTime(0), debounceDelay(delay)
{
    pinMode(pin, MODE);
}

bool Debouncer::update()
{
    bool currentState = digitalRead(pin);
    unsigned long now = millis();
    if (currentState != state) {
        if (lastDebounceTime == 0) {
            lastDebounceTime = now;         // first time we see a change
        } else if (now - lastDebounceTime >= debounceDelay) {
            state = currentState;           // accept new state
            lastDebounceTime = 0;
            return true;                    // register a change has occurred
        }
    } else {
        lastDebounceTime = 0;
    }
    return false;
}

bool Debouncer::getState() const
{
    return state;
}
