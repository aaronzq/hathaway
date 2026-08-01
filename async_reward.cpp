#include "async_reward.h"

Rewarder::Rewarder()
    : rewardPin(-1), defaultRewardDuration(DEFAULT_REWARD_DURATION),
      isOpen(false), closeTime(0)
{
}

Rewarder::Rewarder(int pin, unsigned long duration)
    : rewardPin(pin), defaultRewardDuration(duration),
      isOpen(false), closeTime(0)
{
    pinMode(rewardPin, OUTPUT);
    digitalWrite(rewardPin, LOW);
}

void Rewarder::deliver_reward(unsigned long duration)
{
    if (duration == 0) {
        duration = defaultRewardDuration;
    }
    isOpen = true;
    closeTime = millis() + duration;
}

void Rewarder::setRewardDuration(unsigned long duration)
{
    defaultRewardDuration = duration;
}

void Rewarder::update()
{
    if (isOpen) {
        unsigned long now = millis();
        // Signed difference, not "now <= closeTime": millis() wraps every 49.7
        // days, and a plain compare closes the valve on the first update after
        // the wrap instead of after `duration`.
        if ((long)(now - closeTime) < 0) {
            digitalWrite(rewardPin, HIGH);
        } else {
            digitalWrite(rewardPin, LOW);
            isOpen = false;
        }
    }
}
