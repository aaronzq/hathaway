#pragma once

#include <Arduino.h>


// EVERYTHING IN THIS FILE ASSUMES A 10-BIT LEDC PERIOD (duty 0..1024).
// That is not a choice: ledcWriteTone() installs 10-bit resolution itself, and
// any ledcWrite() afterwards is read against that period. There is no constant
// to change -- ledcAttach() below is passed a literal 10 to match, and duty
// percentages are scaled by 1024. Using some other resolution silently rescales
// every duty cycle (an 8-bit value written against the tone call's 10-bit
// period gives 12.5% where 50% was meant).

#define DEFAULT_NOTE_DURATION 150 //default note duration in ms
#define FREQ 1000 // init Freq
#define DEFAULT_PULSE_WIDTH 50  // PWM duty cycle, percent


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

    // Duty cycle in percent, 0..100. Applies from the next pulse onward, so a
    // change mid-train takes effect on the following pulse. 0 is silence that
    // still logs and still ends with EV_TONE_DONE -- a silent control trial.
    void setPulseWidth(uint8_t pct);

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
    uint8_t       pulseWidthPct = DEFAULT_PULSE_WIDTH;

    void soundOn();
    void soundOff();
};