#include "switch.h"

SwitchHandler::SwitchHandler()
    : PIN(-1), debouncer(0), polarity(HIGH)
{
}

SwitchHandler::SwitchHandler(int p, unsigned long delay, bool pol)
    : PIN(p), debouncer(p, delay), polarity(pol)
{
    pinMode(PIN, INPUT_PULLUP);
}

bool SwitchHandler::update()
{
    bool flipped = debouncer.update();
    if (flipped) {
        return true;
    }
    return false;
}

bool SwitchHandler::getState() const
{
    return debouncer.getState();
}
