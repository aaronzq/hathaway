#include "buzzer.h"


BuzzerHandler::BuzzerHandler()
    : buzzerPin(-1), noteDuration(DEFAULT_NOTE_DURATION),
      isPlaying(false), closeTime(0) {

}

BuzzerHandler::BuzzerHandler(int pin)
    : buzzerPin(pin), noteDuration(DEFAULT_NOTE_DURATION),
      isPlaying(false), closeTime(0) {
    ledcAttach(buzzerPin, FREQ, 10);  // 10 bits: must match ledcWriteTone(), see buzzer.h
    ledcWrite(buzzerPin, 0); // output low (pwm: 0%), mute
}


// The two places sound is turned on and off, so the duty cycle and the pulseOn
// flag can never drift apart.
void BuzzerHandler::soundOn() {
    // ledcWriteTone() sets the frequency and a 50% duty in one call, so at the
    // default width it is the whole job and no second write happens at all.
    // Off 50%, ledcWrite() overrides the duty; it is scaled by 1024 because the
    // tone call just installed a 10-bit period (see buzzer.h).
    if (pulseWidthPct == 0) {
        // Skip the tone call entirely: it would sound at 50% for the microsecond
        // before the duty write landed. pulseOn still goes true, so a silent
        // pulse logs and times exactly like an audible one.
        ledcWrite(buzzerPin, 0);
    } else {
        ledcWriteTone(buzzerPin, trainFreq);
        if (pulseWidthPct != 50) {
            ledcWrite(buzzerPin, (1024u * pulseWidthPct) / 100);
        }
    }
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

void BuzzerHandler::setPulseWidth(uint8_t pct) {
    pulseWidthPct = (pct > 100) ? 100 : pct;
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
