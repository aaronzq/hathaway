#include <TFT_eSPI.h>
#include <math.h>

#define PI 3.1415926f

TFT_eSPI tft;
TFT_eSprite spr = TFT_eSprite(&tft);

// Landscape: 320-pixel axis is horizontal (uniform per row),
//            240-pixel axis is vertical (sinusoidal modulation + scroll direction).
constexpr int W = 320;
constexpr int H = 240;

const float PERIOD    = 80.0f;
const uint32_t FRAME_US = 1000000UL / 60;

float speed    = 80.0f;  // px/s along the 240-axis; negate to reverse direction
float subPixel = 0.0f;   // fractional-pixel accumulator for smooth motion
float topPhase = 0.0f;   // grating Y-value currently mapped to sprite row 0

uint32_t lastFrameUs;
uint32_t nextFrameUs;

// Returns the 16-bit colour for a given position along the 240-axis.
uint16_t gratingColor(float y) {
  float s = sinf(2.0f * PI * y / PERIOD);
  uint8_t gray = static_cast<uint8_t>(lroundf(127.5f + 127.5f * s));
  return tft.color565(gray, gray, gray);
}

void setup() {
  tft.init();
  tft.setRotation(1);          // landscape: 320 wide x 240 tall
  tft.fillScreen(TFT_BLACK);

  spr.createSprite(W, H);
  spr.setScrollRect(0, 0, W, H, TFT_BLACK);

  // Draw initial horizontal grating: each row has a uniform colour,
  // modulated sinusoidally along Y (the 240-pixel axis).
  for (int y = 0; y < H; y++) {
    spr.drawFastHLine(0, y, W, gratingColor((float)y));
  }
  spr.pushSprite(0, 0);

  lastFrameUs = micros();
  nextFrameUs = lastFrameUs;
}

void loop() {
  uint32_t now = micros();

  int32_t waitUs = static_cast<int32_t>(nextFrameUs - now);
  if (waitUs > 2000) {
    delay((waitUs - 1000) / 1000);
    now = micros();
    waitUs = static_cast<int32_t>(nextFrameUs - now);
  }
  if (waitUs > 0) {
    delayMicroseconds(waitUs);
    now = micros();
  }

  const uint32_t elapsedUs = now - lastFrameUs;
  lastFrameUs = now;

  subPixel += speed * (elapsedUs * 1.0e-6f);
  int n = static_cast<int>(floorf(subPixel));
  subPixel -= static_cast<float>(n);

  if (n > 0) {
    if (n > H) n = H;

    // Shift sprite content upward by n rows; bottom n rows become empty.
    spr.scroll(0, -n);

    // Redraw only the n newly exposed rows at the bottom.
    for (int i = 0; i < n; i++) {
      spr.drawFastHLine(0, H - n + i, W, gratingColor(topPhase + H + i));
    }
    topPhase += static_cast<float>(n);

    spr.pushSprite(0, 0);
  }

  nextFrameUs += FRAME_US;
  now = micros();
  if (static_cast<int32_t>(now - nextFrameUs) >= 0) {
    nextFrameUs = now;
  }
}
