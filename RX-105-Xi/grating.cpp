#include "grating.h"
#include "esp_heap_caps.h"

GratingHandler::GratingHandler(int backlightPin)
    : spr(&tft), ledPin(backlightPin) {}

bool GratingHandler::initialize() {
  if (!initialized) {
    tft.init();
    tft.setRotation(1); // landscape, 320 x 240
    tft.fillScreen(TFT_BLACK);
    W = tft.width();
    H = tft.height();
    initialized = true;
  }
  if (spr.getPointer()) return true;

  // Prefer internal memory for the working sprite. Restore the allocator
  // threshold after this allocation.
  spr.setColorDepth(16);
  if (psramFound()) heap_caps_malloc_extmem_enable(size_t(W) * H * 2 + 4160);
  spr.setAttribute(PSRAM_ENABLE, false);
  void *buffer = spr.createSprite(W, H);
  if (psramFound()) heap_caps_malloc_extmem_enable(16384);
  if (!buffer) {
    spr.setAttribute(PSRAM_ENABLE, true);
    buffer = spr.createSprite(W, H);
  }
  return buffer != nullptr;
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

void GratingHandler::buildLUT(float contrast) {
  // One sinusoidal cycle; rendering indexes this table instead of using sin per pixel.
  for (int i = 0; i < LUT_SIZE; ++i) {
    float s = sinf(2.0f * PI * i / LUT_SIZE);
    float g = 127.5f + 127.5f * contrast * s;
    if (g < 0.0f) g = 0.0f;
    if (g > 255.0f) g = 255.0f;
    uint8_t gray = static_cast<uint8_t>(lroundf(g));
    lut[i] = tft.color565(gray, gray, gray);
  }
}

uint16_t GratingHandler::gratingColor(float pos) {
  // Map a position (in pixels) to the LUT bucket for its phase within a cycle.
  float c = pos / period;
  c -= floorf(c); // wrap into [0, 1) — handles negative pos correctly
  int idx = static_cast<int>(c * LUT_SIZE) & (LUT_SIZE - 1);
  return lut[idx];
}

bool GratingHandler::drawGrating(float period_, float angle, float contrast) {
  stopAnimation();
  patternLoaded = false;
  if (!isfinite(period_) || !isfinite(angle) || !isfinite(contrast) ||
      period_ < 2 || contrast < 0 || contrast > 1) return false;
  if (!initialize()) return false;
  period = period_;
  phase = scanPhase = subPixel = 0;

  cosA = cosf(angle * DEG_TO_RAD);
  sinA = sinf(angle * DEG_TO_RAD);

  spriteScanMode = !supportsHardwareScroll(angle);

  buildLUT(contrast);
  spr.fillSprite(TFT_BLACK);

  if (!spriteScanMode) {
    // Grating drifts along the display's X axis, which maps onto the
    // native GRAM row address that ILI9341 hardware scrolling controls.
    int periodPx = static_cast<int>(lroundf(fminf(period / fabsf(cosA), float(W))));
    if (periodPx < 1) periodPx = 1;
    if (periodPx > W) periodPx = W;

    scrollSpan = W - (W % periodPx);
    if (scrollSpan <= 0) scrollSpan = periodPx;

    topFix = (W - scrollSpan) / 2;
    bottomFix = W - topFix - scrollSpan;

    // Draw only the centered, whole-period-multiple region so that when the
    // hardware wraps the scroll offset, the last column blends seamlessly
    // back into the first one. The topFix/bottomFix margins stay black.
    //
    // Fill via a 32-bit DDS phase accumulator: one full grating cycle spans
    // 2^32, so unsigned wrap-around gives free, exact modulo (and handles
    // negative cosA/sinA at obtuse angles). The top LUT_SHIFT bits index the
    // LUT. Writing straight into the sprite buffer skips per-pixel float math
    // and drawPixel() bounds checks entirely.
    void* buf = spr.getPointer();
    const uint32_t stepX =
        static_cast<uint32_t>(static_cast<int32_t>(llroundf((cosA / period) * 4294967296.0f)));
    const uint32_t stepY =
        static_cast<uint32_t>(static_cast<int32_t>(llroundf((sinA / period) * 4294967296.0f)));

    uint16_t* img = static_cast<uint16_t*>(buf);
    for (int y = 0; y < H; ++y) {
      uint32_t acc = static_cast<uint32_t>(y) * stepY; // phase at x = 0 for this row
      uint16_t* row = img + static_cast<size_t>(y) * W + topFix;
      for (int x = 0; x < scrollSpan; ++x) {
        // TFT_eSprite stores 16-bpp pixels byte-swapped (big-endian) for fast
        // pushing, so match that here since we bypass drawPixel().
        uint16_t c = lut[acc >> LUT_SHIFT];
        row[x] = (c >> 8) | (c << 8);
        acc += stepX;
      }
    }

  } else {
    // Angle is (near) 90/270 degrees: bars run horizontally and must drift
    // vertically, a direction the ILI9341 hardware scroll cannot address
    // (it only ever scrolls along the native-row / display-X axis). Instead
    // we scan the sprite ourselves: each update() shifts its rows in place
    // and redraws only the newly exposed ones from a continuously running
    // phase, so the pattern loops seamlessly with no cropping required.
    // Software scrolling uses the full screen with no hardware offset.
    topFix=bottomFix=0; scrollSpan=W;

    for (int y = 0; y < H; ++y) {
      spr.drawFastHLine(0, y, W, gratingColor(scanPhase + y));
    }

    spr.setScrollRect(0, 0, W, H, TFT_BLACK);
  }

  // Preserve the tested hardware upload -> configure -> reset sequence.
  if (!spriteScanMode) spr.pushSprite(0, 0);
  configureHardwareScroll();
  lastScroll = -1;
  setHardwareScroll(0);
  if (spriteScanMode) spr.pushSprite(0, 0);
  patternLoaded = true;
  return true;
}

bool GratingHandler::update() {
  if (!active) {
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

    // Hardware movement needs only a scroll-register write.
    setHardwareScroll(static_cast<int>(phase));
  } else {
    subPixel += effSpeed * dt;
    int n = static_cast<int>(subPixel);
    subPixel -= static_cast<float>(n);

    if (n != 0) {
      int m = (n > 0) ? n : -n;
      if (m > H) m = H;

      if (n > 0) {
        // Shift sprite content upward by m rows; redraw the m exposed bottom rows.
        spr.scroll(0, -m);
        for (int i = 0; i < m; ++i) {
          spr.drawFastHLine(0, H - m + i, W, gratingColor(scanPhase + H + i));
        }
        scanPhase += static_cast<float>(m);
      } else {
        // Shift sprite content downward by m rows; redraw the m exposed top rows.
        spr.scroll(0, m);
        for (int i = 0; i < m; ++i) {
          spr.drawFastHLine(0, i, W, gratingColor(scanPhase - m + i));
        }
        scanPhase -= static_cast<float>(m);
      }
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

void GratingHandler::setBacklight(bool on) {
  if (ledPin < 0) return;
  // Backlight control also works before display initialization.
  if (!ledConfigured) {
    pinMode(ledPin, OUTPUT);
    ledConfigured = true;
  }
  digitalWrite(ledPin, on ? HIGH : LOW);
}

bool GratingHandler::supportsHardwareScroll(float angle) {
  if (!isfinite(angle)) return false;
  // Preserve the experimentally tested renderer's 0.01-degree cutoff.
  float aMod = fmodf(angle, 180.0f);
  if (aMod < 0.0f) aMod += 180.0f;
  return fabsf(aMod - 90.0f) >= 0.01f;
}

bool GratingHandler::startAnimation(float speed) {
  if (!patternLoaded || !isfinite(speed)) return false;
  effSpeed = speed / (spriteScanMode ? sinA : cosA);
  lastFrameUs = micros();
  nextFrameUs = lastFrameUs;
  active = true;
  return true;
}

void GratingHandler::stopAnimation() {
  active = false;
}

void GratingHandler::fillColor(uint8_t red, uint8_t green, uint8_t blue) {
  stopAnimation();
  patternLoaded = false;
  initialize();
  topFix = bottomFix = 0;
  scrollSpan = W;
  lastScroll = -1;
  configureHardwareScroll();
  setHardwareScroll(0);
  tft.fillScreen(tft.color565(red, green, blue));
}
