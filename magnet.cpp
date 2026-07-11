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

void Magneto::update()
{
    if (isOn) {
        unsigned long now = millis();
        if (now <= closeTime) {
            digitalWrite(magnetPin, HIGH);
        } else {
            digitalWrite(magnetPin, LOW);
            isOn = false;
        }
    }
}

void Magneto::halt()
{
    digitalWrite(magnetPin, LOW);
    isOn = false;
}