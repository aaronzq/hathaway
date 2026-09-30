#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>

// Draw -> start/update/stop. Tasks own timing and backlight policy.
class GratingHandler {
    static constexpr int LUT_SIZE = 1024;
public:
    explicit GratingHandler(int backlightPin = -1);

    // Generate and transmit a fresh grating. Period >= 2; contrast in [0,1].
    // Backlight remains under caller control during this synchronous operation.
    bool drawGrating(float period, float angle, float contrast);
    // Start/resume at speed in pixels/second. Runs until explicitly stopped.
    bool startAnimation(float speed);
    void stopAnimation();
    bool update(); // service animation; false when stopped
    bool isAnimating() const { return active; }

    // Backlight is independent of pixels and movement. Filling stops animation.
    void setBacklight(bool on);
    void fillColor(uint8_t red, uint8_t green, uint8_t blue);
    // Unsupported angles still render, using software frame transfers instead.
    static bool supportsHardwareScroll(float angle);

private:
    TFT_eSPI tft;
    TFT_eSprite spr;
    int W = 0, H = 0;
    int ledPin;
    bool ledConfigured = false, initialized = false;
    bool active = false, patternLoaded = false, spriteScanMode = false;
    float period = 0, cosA = 1, sinA = 0;
    int scrollSpan = 1, topFix = 0, bottomFix = 0, lastScroll = -1;
    float phase = 0, subPixel = 0, scanPhase = 0, effSpeed = 0;
    uint32_t lastFrameUs = 0, nextFrameUs = 0;
    static constexpr uint32_t FRAME_US = 1000000UL / 60;
    static constexpr int LUT_SHIFT = 22;
    uint16_t lut[LUT_SIZE];

    bool initialize();
    void buildLUT(float contrast);
    uint16_t gratingColor(float position);
    void write16(uint16_t value);
    void configureHardwareScroll();
    void setHardwareScroll(int offset);
};
