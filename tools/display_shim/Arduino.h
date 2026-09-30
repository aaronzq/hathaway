#pragma once
#include "../shim/Arduino.h"
#include <cmath>
#define OUTPUT 1
#define PI 3.14159265358979323846
#define DEG_TO_RAD (PI / 180.0)
inline uint32_t micros() { return g_arduinoMillis * 1000u; }
inline bool psramFound() { return true; }