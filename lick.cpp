#include "lick.h"

LickHandler::LickHandler()
    : PIN(-1), debouncer(0), polarity(HIGH)
{
}

LickHandler::LickHandler(int p, unsigned long delay, bool pol)
    : PIN(p), debouncer(p, delay), polarity(pol)
{
    pinMode(PIN, INPUT);
}

bool LickHandler::update()
{
    bool flipped = debouncer.update();
    if (flipped && (getState() == polarity)) {
        return true;
    }
    return false;
}

bool LickHandler::getState() const
{
    return debouncer.getState();
}