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
        railEngine.init();
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

int32_t RailHandler::mmToPulses(float mm) const
{
    float pulses = mm * RAIL_CALIBRATION_MM_TO_PULSE;
    if (pulses >= 0.0f) {
        return (int32_t)(pulses + 0.5f);
    }
    return (int32_t)(pulses - 0.5f);
}
