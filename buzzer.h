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

    // A pulsed tone: nPulses bursts of `pulseMs` separated by `gapMs` of
    // silence, with no trailing gap. Total length is therefore
    // nPulses*pulseMs + (nPulses-1)*gapMs -- the task computes the same figure
    // to arm its own timeout, so it never has to wait on the buzzer.
    //
    // Sequenced here rather than as a chain of task states because the pulse
    // pattern is a property of the stimulus, not of the behaviour: a task that
    // had to count pulses itself would need five states to say "play a tone".
    void playTrain(uint32_t freq, unsigned long pulseMs,
                   unsigned long gapMs, uint8_t nPulses);

    // Silence immediately, whatever was playing. Used when switching tasks so a
    // note can never be left sounding across a task boundary.
    void stop();

    // Returns true while a note or a WHOLE train is still running -- for a
    // train it stays true across the silent gaps. The control loop watches the
    // falling edge of this to raise EV_TONE_DONE, so a train produces exactly
    // one EV_TONE_DONE, at the end of its last pulse.
    bool update();

    // True only while sound is actually coming out, so it goes false during a
    // train's gaps. The control loop edge-detects this for TONE telemetry,
    // which is what makes a train log as separate pulses rather than as one
    // continuous block. For a single note it is identical to update().
    bool isPulseOn() const { return pulseOn; }

private:
    int buzzerPin;
    unsigned long noteDuration, closeTime;
    bool isPlaying;

    // Train state. Default-initialised here rather than in the constructor
    // init lists, which are ordered for the members that predate them.
    bool          isTrain    = false;   // a train, not a single note
    bool          pulseOn    = false;   // sound is on right now
    uint32_t      trainFreq  = FREQ;
    unsigned long pulseMs    = DEFAULT_NOTE_DURATION;
    unsigned long gapMs      = 0;
    unsigned long phaseEnd   = 0;       // when the current pulse or gap ends
    uint8_t       pulsesLeft = 0;       // pulses not yet started

    void soundOn();
    void soundOff();
};