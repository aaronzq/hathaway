#include "magnet.h"

Magneto::Magneto()
    : magnetPin(-1), defaultFixDuration(DEFAULT_FIX_DURATION),
      isOn(false), closeTime(0)
{
}

Magneto::Magneto(int pin, unsigned long duration)
    : magnetPin(pin), defaultFixDuration(duration),
      isOn(false), closeTime(0)
{
    pinMode(magnetPin, OUTPUT);
    digitalWrite(magnetPin, LOW);
}

void Magneto::magnetic_start(unsigned long duration)
{
    if (duration == 0) {
        duration = defaultFixDuration;
    }
    isOn = true;
    closeTime = millis() + duration;
}

bool Magneto::update()
{
    if (isOn) {
        unsigned long now = millis();
        // Signed difference: see the note in async_reward.cpp. A plain compare
        // releases the magnet early on the first update after the millis() wrap.
        if ((long)(now - closeTime) < 0) {
            digitalWrite(magnetPin, HIGH);
        } else {
            digitalWrite(magnetPin, LOW);
            isOn = false;
        }
    }
    return isOn;
}

void Magneto::halt()
{
    digitalWrite(magnetPin, LOW);
    isOn = false;
}

void Magneto::setFixDuration(unsigned long duration)
{
    defaultFixDuration = duration;
}