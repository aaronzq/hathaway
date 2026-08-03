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


// The two places sound is turned on and off, so the duty cycle and the pulseOn
// flag can never drift apart.
void BuzzerHandler::soundOn() {
    ledcWriteTone(buzzerPin, trainFreq);
    ledcWrite(buzzerPin, 1 << (PWM_RESOLUTION - 1));
    pulseOn = true;
}

void BuzzerHandler::soundOff() {
    ledcWrite(buzzerPin, 0);
    pulseOn = false;
}


void BuzzerHandler::playNote(uint32_t freq, unsigned long duration) {
    if (buzzerPin < 0) {
        return;
    }
    trainFreq = freq;
    isTrain   = false;
    soundOn();
    if (duration == 0) {
        noteDuration = DEFAULT_NOTE_DURATION;
    } else {
        noteDuration = duration;
    }
    isPlaying = true;
    closeTime = millis() + noteDuration;
}

void BuzzerHandler::playTrain(uint32_t freq, unsigned long pulse,
                              unsigned long gap, uint8_t nPulses) {
    if (buzzerPin < 0 || nPulses == 0) {
        return;
    }
    trainFreq = freq;
    pulseMs   = (pulse == 0) ? DEFAULT_NOTE_DURATION : pulse;
    gapMs     = gap;
    isTrain   = true;
    isPlaying = true;
    // The first pulse starts now, so it is already spoken for.
    pulsesLeft = nPulses - 1;
    soundOn();
    phaseEnd = millis() + pulseMs;
}

void BuzzerHandler::stop() {
    if (buzzerPin < 0) {
        return;
    }
    soundOff();
    isPlaying  = false;
    isTrain    = false;
    pulsesLeft = 0;
}

bool BuzzerHandler::update() {
    if (buzzerPin < 0) {
        return false;
    }
    if (!isPlaying) {
        return false;
    }

    if (isTrain) {
        // A while loop, not an if: with a short pulse or gap and a slow cycle
        // more than one phase can be due at once, and the train must still end
        // on the right pulse rather than stretch to fit the loop period.
        while (isPlaying && (long)(millis() - phaseEnd) >= 0) {
            if (pulseOn) {
                soundOff();
                if (pulsesLeft == 0) {      // last pulse: the train is over
                    isPlaying = false;
                    isTrain   = false;
                    break;
                }
                phaseEnd += gapMs;
            } else {
                soundOn();
                pulsesLeft--;
                phaseEnd += pulseMs;
            }
        }
        return isPlaying;
    }

    if ((long)(millis() - closeTime) >= 0) {
        isPlaying = false;
        soundOff();
    }
    return isPlaying;
}
