#include "railHandler.h"

namespace {
FastAccelStepperEngine railEngine = FastAccelStepperEngine();
bool railEngineReady = false;
}

RailHandler::RailHandler()
    : stepper(nullptr),
      currentPositionPulses_(0)
{
}

bool RailHandler::begin(uint8_t stepPin, uint8_t dirPin,
                        bool dirHighCountsUp, uint32_t speedHz)
{
    if (!railEngineReady) {
        // Pinned to core 1, with the rest of the rig, rather than left to float.
        //
        // Step pulses come out of hardware, not from the CPU toggling a pin.
        // The engine task only refills the peripheral's queue: it wakes every
        // 4 ms and plans 20 ms ahead (FastAccelStepper defaults), so the cost is
        // some 250 short wakes a second -- less than the HX711 read this loop
        // already does every cycle.
        //
        // The argument is NOT optional in effect: FastAccelStepper's default is
        // 255, which means xTaskCreate() and no affinity at all, so the task
        // would be free to land on core 0 and share it with the comms task and
        // Serial. Only 0 and 1 use xTaskCreatePinnedToCore(). Checked against
        // FastAccelStepperEngine.h in 1.2.7.
        railEngine.init(1);
        railEngineReady = true;
    }

    stepper = railEngine.stepperConnectToPin(stepPin);
    if (stepper == nullptr) {
        return false;
    }

    stepper->setDirectionPin(dirPin, dirHighCountsUp, RAIL_DIRECTION_DELAY_US);
    if (stepper->setSpeedInHz(speedHz) != 0) {
        return false;
    }
    if (stepper->setAcceleration(RAIL_ACCELERATION_STEPS_S2) != 0) {
        return false;
    }

    stepper->setCurrentPosition(0);
    currentPositionPulses_ = 0;
    return true;
}

bool RailHandler::move(float distanceMm)
{
    return movePulses(mmToPulses(distanceMm));
}

bool RailHandler::movePulses(int32_t pulses)
{
    if (stepper == nullptr || stepper->isRunning()) {
        return false;
    }
    updateCurrentPosition();
    if (stepper->move(pulses) != MOVE_OK) {
        return false;
    }
    return true;
}

void RailHandler::stop()
{
    if (stepper != nullptr) {
        stepper->stopMove();
        updateCurrentPosition();
    }
}

bool RailHandler::isRunning()
{
    if (stepper == nullptr) {
        return false;
    }
    if (stepper->isRunning()) {
        return true;
    }
    updateCurrentPosition();
    return false;
}

bool RailHandler::setHome()
{
    // The isRunning() guard is not just tidiness. On the ESP32,
    // setCurrentPosition() is implemented on top of getCurrentPosition(), which
    // does not account for the steps of the command already in flight, so the
    // library explicitly recommends calling it only at standstill. Rezeroing
    // mid-move would therefore put the origin at a position the rail is not at.
    if (stepper == nullptr || stepper->isRunning()) {
        return false;
    }
    stepper->setCurrentPosition(0);
    currentPositionPulses_ = 0;
    return true;
}

float RailHandler::currentPositionMm()
{
    return (float)currentPositionPulses() / RAIL_CALIBRATION_MM_TO_PULSE;
}

int32_t RailHandler::currentPositionPulses()
{
    updateCurrentPosition();
    return currentPositionPulses_;
}

void RailHandler::updateCurrentPosition()
{
    if (stepper != nullptr) {
        currentPositionPulses_ = stepper->getCurrentPosition();
    }
}

int32_t RailHandler::mmToPulses(float mm)
{
    float pulses = mm * RAIL_CALIBRATION_MM_TO_PULSE;
    if (pulses >= 0.0f) {
        return (int32_t)(pulses + 0.5f);
    }
    return (int32_t)(pulses - 0.5f);
}
