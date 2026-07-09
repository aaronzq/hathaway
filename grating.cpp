#include "grating.h"

GratingHandler::GratingHandler()
    : tft(), spr(&tft), W(0), H(0), ledPin(-1), duration(DEFAULT_DISPLAY_DURATION),
      initialized(false), trialStartMs(0), active(false), period(0), angle(0),
      contrast(1.0f), cosA(1), sinA(0), spriteScanMode(false), periodPx(1),
      scrollSpan(0), topFix(0), bottomFix(0), phase(0), lastScroll(-1),
      subPixel(0), scanPhase(0), effSpeed(0), lastFrameUs(0), nextFrameUs(0) {
}

GratingHandler::GratingHandler(int ledPIN, unsigned long duration)
    : tft(), spr(&tft), W(0), H(0), ledPin(ledPIN), duration(duration),
      initialized(false), trialStartMs(0), active(false), period(0), angle(0),
      contrast(1.0f), cosA(1), sinA(0), spriteScanMode(false), periodPx(1),
      scrollSpan(0), topFix(0), bottomFix(0), phase(0), lastScroll(-1),
      subPixel(0), scanPhase(0), effSpeed(0), lastFrameUs(0), nextFrameUs(0) {
}

void GratingHandler::setDuration(unsigned long seconds) {
  duration = seconds;
}

void GratingHandler::initHardware() {
  tft.init();
  tft.setRotation(1); // landscape: 320 wide x 240 tall
  tft.fillScreen(TFT_BLACK);

  W = tft.width();
  H = tft.height();

  spr.createSprite(W, H);

  if (ledPin >= 0) {
    pinMode(ledPin, OUTPUT);
  }

  initialized = true;
}

void GratingHandler::write16(uint16_t value) {
  tft.writedata(value >> 8);
  tft.writedata(value & 0xFF);
}

void GratingHandler::configureHardwareScroll() {
  tft.startWrite();
  tft.writecommand(ILI9341_VSCRDEF);
  write16(topFix);      // Top fixed area
  write16(scrollSpan);  // Scrolling area
  write16(bottomFix);   // Bottom fixed area
  tft.endWrite();
}

void GratingHandler::setHardwareScroll(int offset) {
  offset %= scrollSpan;
  if (offset < 0) {
    offset += scrollSpan;
  }

  if (offset == lastScroll) {
    return;
  }

  tft.startWrite();
  tft.writecommand(ILI9341_VSCRSADD);
  write16(offset + topFix);
  tft.endWrite();

  lastScroll = offset;
}

uint16_t GratingHandler::gratingColor(float pos) {
  float s = sinf(2.0f * PI * pos / period);
  float g = 127.5f + 127.5f * contrast * s;
  if (g < 0.0f) g = 0.0f;
  if (g > 255.0f) g = 255.0f;
  uint8_t gray = static_cast<uint8_t>(lroundf(g));
  return tft.color565(gray, gray, gray);
}

void GratingHandler::drawGrating(float period_, float angle_, float contrast_) {
  if (!initialized) {
    initHardware();
  }

  period = period_;
  angle = angle_;
  contrast = constrain(contrast_, 0.0f, 1.0f);

  cosA = cosf(angle * DEG_TO_RAD);
  sinA = sinf(angle * DEG_TO_RAD);

  float aMod = fmodf(angle, 180.0f);
  if (aMod < 0.0f) aMod += 180.0f;
  spriteScanMode = fabsf(aMod - 90.0f) < ANGLE_EPS;

  spr.fillSprite(TFT_BLACK);

  if (!spriteScanMode) {
    // Grating drifts along the display's X axis, which maps onto the
    // native GRAM row address that ILI9341 hardware scrolling controls.
    periodPx = static_cast<int>(lroundf(period / fabsf(cosA)));
    if (periodPx < 1) periodPx = 1;
    if (periodPx > W) periodPx = W;

    scrollSpan = W - (W % periodPx);
    if (scrollSpan <= 0) scrollSpan = periodPx;

    topFix = (W - scrollSpan) / 2;
    bottomFix = W - topFix - scrollSpan;

    // Draw only the centered, whole-period-multiple region so that when the
    // hardware wraps the scroll offset, the last column blends seamlessly
    // back into the first one. The topFix/bottomFix margins stay black.
    for (int x = 0; x < scrollSpan; ++x) {
      for (int y = 0; y < H; ++y) {
        float pos = cosA * x + sinA * y;
        spr.drawPixel(x + topFix, y, gratingColor(pos));
      }
    }

    spr.pushSprite(0, 0);
    configureHardwareScroll();
    phase = 0.0f;
    lastScroll = -1;
    setHardwareScroll(0);
  } else {
    // Angle is (near) 90/270 degrees: bars run horizontally and must drift
    // vertically, a direction the ILI9341 hardware scroll cannot address
    // (it only ever scrolls along the native-row / display-X axis). Instead
    // we scan the sprite ourselves: each update() shifts its rows in place
    // and redraws only the newly exposed ones from a continuously running
    // phase, so the pattern loops seamlessly with no cropping required.
    scanPhase = 0.0f;
    subPixel = 0.0f;

    for (int y = 0; y < H; ++y) {
      spr.drawFastHLine(0, y, W, gratingColor(scanPhase + y));
    }

    spr.setScrollRect(0, 0, W, H, TFT_BLACK);
    spr.pushSprite(0, 0);
  }

  lastFrameUs = micros();
  nextFrameUs = lastFrameUs;

  // Start the trial clock. This grating displays for `duration` seconds
  // (0 = indefinitely), enforced by update().
  trialStartMs = millis();
  active = true;
}

void GratingHandler::configScroll(float speed) {
  if (!spriteScanMode) {
    effSpeed = speed / cosA;
  } else {
    effSpeed = speed / sinA;
  }
}

bool GratingHandler::update() {
  if (!active) {
    return false;
  }

  // End the trial once the display duration has elapsed (duration == 0 runs
  // indefinitely). Blank the screen so the animation stops cleanly.
  if (duration > 0 &&
      (millis() - trialStartMs) >= (duration * 1000UL)) {
    active = false;
    spr.fillSprite(TFT_BLACK);
    spr.pushSprite(0, 0);
    setHardwareScroll(0);
    return false;
  }

  uint32_t now = micros();
  if (static_cast<int32_t>(now - nextFrameUs) < 0) {
    return true; // not yet time for the next frame
  }

  const uint32_t elapsedUs = now - lastFrameUs;
  lastFrameUs = now;
  const float dt = elapsedUs * 1.0e-6f;

  if (!spriteScanMode) {
    phase += effSpeed * dt;
    phase = fmodf(phase, static_cast<float>(scrollSpan));
    if (phase < 0.0f) phase += scrollSpan;

    // This tiny register update replaces the full-screen pixel transfer.
    setHardwareScroll(static_cast<int>(phase));
  } else {
    subPixel += effSpeed * dt;
    int n = static_cast<int>(subPixel);
    subPixel -= static_cast<float>(n);

    if (n > 0) {
      if (n > H) n = H;

      // Shift sprite content upward by n rows; bottom n rows become empty.
      spr.scroll(0, -n);
      for (int i = 0; i < n; ++i) {
        spr.drawFastHLine(0, H - n + i, W, gratingColor(scanPhase + H + i));
      }
      scanPhase += static_cast<float>(n);
      spr.pushSprite(0, 0);
    } else if (n < 0) {
      int m = -n;
      if (m > H) m = H;

      // Shift sprite content downward by m rows; top m rows become empty.
      spr.scroll(0, m);
      for (int i = 0; i < m; ++i) {
        spr.drawFastHLine(0, i, W, gratingColor(scanPhase - m + i));
      }
      scanPhase -= static_cast<float>(m);
      spr.pushSprite(0, 0);
    }
  }

  nextFrameUs += FRAME_US;
  now = micros();
  if (static_cast<int32_t>(now - nextFrameUs) >= 0) {
    nextFrameUs = now;
  }

  return true;
}

void GratingHandler::switchOn(bool on) {
  if (ledPin >= 0) {
    digitalWrite(ledPin, on ? HIGH : LOW);
  }
}
