#pragma once

#include <Arduino.h>
#include <FastAccelStepper.h>

#define RAIL_DEFAULT_SPEED_HZ 3200u
#define RAIL_CALIBRATION_MM_TO_PULSE 3020.0f
#define RAIL_DIRECTION_DELAY_US 200u
#define RAIL_ACCELERATION_STEPS_S2 1000000

class RailHandler {
public:
    RailHandler();

    bool begin(uint8_t stepPin, uint8_t dirPin,
               bool dirHighCountsUp = true,
               uint32_t speedHz = RAIL_DEFAULT_SPEED_HZ);

    bool move(float distanceMm);
    bool movePulses(int32_t pulses);
    void stop();
    bool isRunning();
    bool setHome();
    float currentPositionMm();
    int32_t currentPositionPulses();

    // Public and static because the sketch converts too: a move commanded in
    // pulses is logged in millimetres, and one commanded in millimetres is
    // handed to the stepper in pulses. One definition of the calibration, so
    // the command that runs and the number that gets logged cannot disagree.
    static int32_t mmToPulses(float mm);

private:
    FastAccelStepper *stepper;
    int32_t currentPositionPulses_;

    void updateCurrentPosition();
};
