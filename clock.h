#pragma once

#include <Arduino.h>

class Clock {
    public:
        Clock();
        Clock(int pin, float freq, float duty_cycle = 0.5);
        void start();
        void stop();
        void tick();
        float get_frequency();
        float get_duty_cycle();
        void set_frequency(float freq);
        void set_duty_cycle(float duty_cycle);
        bool is_running();
    private:
        int pin;
        float frequency;
        float duty_cycle;
        bool running;
        bool state;
        unsigned long period_us;
        unsigned long high_time_us;
        unsigned long low_time_us;
        unsigned long next_high_us;
        unsigned long next_low_us;
        void toggle();
};

