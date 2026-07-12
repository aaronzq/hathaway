#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>

// default display time for each grating trial in seconds
#define DEFAULT_DISPLAY_DURATION 3 

// Sprite memory: 1 = internal SRAM (fast per-frame push/scroll, uses ~150 KB
// of SRAM), 0 = PSRAM (slower push/scroll, but frees internal SRAM).
// setInternalRAM() overrides this at runtime.
#define GRATING_USE_INTERNAL_RAM 1

// Set to 1 to print drawGrating()/update() timings (microseconds) over Serial.
// Set to 0 for production to remove the profiling overhead entirely.
#define GRATING_PROFILE 1

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

    // Sprite memory. Preferably call before the first drawGrating(); if the
    // sprite already exists it is reallocated immediately (then redraw).
    void setInternalRAM(bool on);   // true = internal SRAM (fast), false = PSRAM (slow, frees SRAM)
private:
    TFT_eSPI tft;
    TFT_eSprite spr;
    int W, H;

    int ledPin;
    unsigned long duration; // trial display time in seconds; 0 = indefinite
    bool initialized;

    // Sprite allocation preference (applied when the sprite is (re)created).
    bool spriteInternal;         // requested: true = internal SRAM, false = PSRAM
    bool spriteActuallyInternal; // where the buffer really landed
    size_t freeInternalAfter;    // internal SRAM free (bytes) after allocation

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

    // One-cycle grayscale colour lookup table, rebuilt per trial from the
    // current period/contrast. Replaces the per-pixel sinf() in drawGrating.
    // Power of two so the phase index masks cheaply. 1024 entries = 2 KB.
    static constexpr int LUT_SIZE = 1024;
    static constexpr int LUT_SHIFT = 22; // 32 - log2(LUT_SIZE), for the DDS accumulator
    uint16_t lut[LUT_SIZE];

    void initHardware();
    void allocateSprite(); // (re)create the 16bpp sprite honoring spriteInternal
    void buildLUT();
#if GRATING_PROFILE
    // Print "draw=.. push=.. total=.." (us); negative draw/push print as "nan".
    void profPrintDrawPush(long drawUs, long pushUs, unsigned long totalUs);
#endif
    void write16(uint16_t value);
    void configureHardwareScroll();
    void setHardwareScroll(int offset);
    uint16_t gratingColor(float pos);
};
