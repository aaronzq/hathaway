#include "buzzer.h"


BuzzerHandler::BuzzerHandler()
    : buzzerPin(-1), noteDuration(DEFAULT_NOTE_DURATION),
      isPlaying(false), closeTime(0) {

}

BuzzerHandler::BuzzerHandler(int pin) 
    : buzzerPin(pin), noteDuration(DEFAULT_NOTE_DURATION),
      isPlaying(false), closeTime(0) {
    ledcAttach(buzzerPin, FREQ, PWM_RESOLUTION);
    ledcWrite(buzzerPin, 0); // output low (pwm: 0%), mute 
}


void BuzzerHandler::playNote(uint32_t freq, unsigned long duration) {
    if (buzzerPin < 0) {
        return;
    }
    ledcWriteTone(buzzerPin, freq);
    ledcWrite(buzzerPin, 1 << (PWM_RESOLUTION - 1));
    if (duration == 0) {
        noteDuration = DEFAULT_NOTE_DURATION;
    } else {
        noteDuration = duration;
    }
    isPlaying = true;
    closeTime = millis() + noteDuration;
}

bool BuzzerHandler::update() {
    if (buzzerPin < 0) {
        return false;
    }
    if (isPlaying) {
        if ((long)(millis() - closeTime) >= 0) {
                isPlaying = false;
                ledcWrite(buzzerPin, 0);
        }
    } 
    return isPlaying;
}