#pragma once

#include <Arduino.h>

// default solenoid open time in milliseconds
#define DEFAULT_REWARD_DURATION 100

// ---------------------------------------------------------------------------
// Asynchronous reward valve controller.  ``deliver_reward`` arms the solenoid
// for ``duration`` ms.  ``update`` should be called repeatedly in ``loop`` to
// actually toggle the output pin.
// ---------------------------------------------------------------------------
class Rewarder {
public:
    Rewarder();
    Rewarder(int pin, unsigned long duration = DEFAULT_REWARD_DURATION);

    void deliver_reward(unsigned long duration = 0);
    void update();
    void setRewardDuration(unsigned long duration);  // update default open time

private:
    int rewardPin;
    unsigned long defaultRewardDuration;
    bool isOpen;
    unsigned long closeTime;
};