#include "magnet.h"

Magneto::Magneto()
    : magnetPin(-1), defaultFixDuration(DEFAULT_FIX_DURATION),
      isOn(false), closeTime(0),
      graceDuration(DEFAULT_MAG_GRACE), graceEnd(0)
{
}

Magneto::Magneto(int pin, unsigned long duration)
    : magnetPin(pin), defaultFixDuration(duration),
      isOn(false), closeTime(0),
      graceDuration(DEFAULT_MAG_GRACE), graceEnd(0)
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
    unsigned long now = millis();
    closeTime = now + duration;
    graceEnd = now + graceDuration;
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

void Magneto::setGraceDuration(unsigned long duration)
{
    // Takes effect at the next magnetic_start(); a magnet already running keeps
    // the window it was given.
    graceDuration = duration;
}

bool Magneto::haltAllowed() const
{
    if (!isOn) return true;
    // Signed difference for the same millis() wrap reason as update().
    return (long)(millis() - graceEnd) >= 0;
}