#include "clock.h"

Clock::Clock() : pin(-1), frequency(0), duty_cycle(0), running(false), state(LOW),
                 period_us(0), high_time_us(0), low_time_us(0),
                 next_high_us(0), next_low_us(0) {
}

Clock::Clock(int pin, float freq, float duty_cycle)
    : pin(pin), frequency(freq), duty_cycle(duty_cycle) {
    period_us = 1.0e6 / frequency;
    high_time_us = period_us * duty_cycle;
    low_time_us = period_us * (1 - duty_cycle);
    running = false;
    state = LOW;
    next_high_us = 0;
    next_low_us = 0;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);  // Ensure pin is LOW initially
}


void Clock::tick() {
    if (!running) return;

    unsigned long current_time = micros();
    if (current_time >= next_high_us) {
        state = HIGH;
        next_low_us = current_time + high_time_us;
        next_high_us = current_time + period_us;
    } else if (current_time >= next_low_us) {
        state = LOW;
    }
    digitalWrite(pin, state);
}

void Clock::start() {
    if (!running) {
        running = true;
        state = HIGH;
        tick();
    }
}

void Clock::stop() {
    if (running) {
        running = false;
        digitalWrite(pin, LOW);  // Ensure pin is LOW when stopped
    }
}

float Clock::get_frequency() {
    return frequency;
}

float Clock::get_duty_cycle() {
    return duty_cycle;
}

void Clock::set_frequency(float freq) {
    frequency = freq;
    period_us = 1.0e6 / frequency;
    high_time_us = period_us * duty_cycle;
    low_time_us = period_us * (1 - duty_cycle);
}

void Clock::set_duty_cycle(float duty_cycle) {
    this->duty_cycle = duty_cycle;
    high_time_us = period_us * duty_cycle;
    low_time_us = period_us * (1 - duty_cycle);
}

bool Clock::is_running() {
    return running;
}

void Clock::toggle() {
    state = !state;
    digitalWrite(pin, state);
}

