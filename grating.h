#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>


#define DEFAULT_DISPLAY_DURATION 3 // default display time for each grating trial in seconds


class GratingHandler {
public:
    GratingHandler();
    GratingHandler(int ledPIN, unsigned long duration = DEFAULT_DISPLAY_DURATION);

    void drawGrating(float period, float angle, float contrast); // draw grating in the sprite and sent to tft GRAM
    void configScroll(float speed); //configurate the scroll parameters. Execute once after drawing grating
    bool update(); //advance the scroll one frame; returns false once the trial duration has elapsed
    void switchOn(bool on); //drive ledPIN HIGH/LOW (general-purpose, not tied to the grating)
    void setDuration(unsigned long seconds); //trial display time; 0 = run indefinitely
    bool isActive() const { return active; } //true while the current trial is still displaying
private:
    TFT_eSPI tft;
    TFT_eSprite spr;
    int W, H;

    int ledPin;
    unsigned long duration; // trial display time in seconds; 0 = indefinite
    bool initialized;

    // trial timing (wall-clock, independent of the frame pacer)
    uint32_t trialStartMs;
    bool active;

    // grating parameters, in native (pre-rotation) sprite/GRAM space
    float period, angle, contrast;
    float cosA, sinA;

    // true for angle == 90/270 (mod 180), where hardware scrolling cannot
    // move the pattern, so we fall back to scanning the sprite ourselves.
    bool spriteScanMode;

    // hardware VSCRDEF/VSCRSADD scroll state (used when !spriteScanMode)
    int periodPx;    // grating period projected onto the scroll axis, in pixels
    int scrollSpan;  // width of the centered, whole-period-multiple scroll region
    int topFix, bottomFix;
    float phase;
    int lastScroll;

    // sprite-scan state (used when spriteScanMode)
    float subPixel;
    float scanPhase; // grating position value currently mapped to sprite row 0

    float effSpeed;  // effective px/s along the active scroll axis

    uint32_t lastFrameUs, nextFrameUs;
    static constexpr uint32_t FRAME_US = 1000000UL / 60;
    static constexpr float ANGLE_EPS = 0.01f;

    void initHardware();
    void write16(uint16_t value);
    void configureHardwareScroll();
    void setHardwareScroll(int offset);
    uint16_t gratingColor(float pos);
};
