#pragma once

#include <Arduino.h>


#define DEFAULT_NOTE_DURATION 150 //default note duration in ms
#define FREQ 1000 // init Freq
#define PWM_RESOLUTION 8   // LEDC duty-cycle resolution in bits (0-255)


class BuzzerHandler {

public:
    BuzzerHandler();
    BuzzerHandler(int pin);

    void playNote(uint32_t freq = FREQ, unsigned long duration = DEFAULT_NOTE_DURATION);

    // Silence immediately, whatever was playing. Used when switching tasks so a
    // note can never be left sounding across a task boundary.
    void stop();

    // Returns true while a note is still sounding. The control loop watches the
    // falling edge of this to raise EV_TONE_DONE.
    bool update();

private:
    int buzzerPin;
    unsigned long noteDuration, closeTime;
    bool isPlaying;

};