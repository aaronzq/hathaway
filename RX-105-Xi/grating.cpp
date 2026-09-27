#include "grating.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

GratingHandler::GratingHandler()
    : tft(), spr(&tft), W(0), H(0), ledPin(-1), ledConfigured(false),
      duration(DEFAULT_DISPLAY_DURATION),
      initialized(false), spriteInternal(GRATING_USE_INTERNAL_RAM),
      spriteActuallyInternal(false), freeInternalAfter(0), trialStartMs(0),
      active(false), period(0), angle(0), contrast(1.0f), cosA(1), sinA(0),
      spriteScanMode(false), periodPx(1), scrollSpan(0), topFix(0), bottomFix(0),
      phase(0), lastScroll(-1), subPixel(0), scanPhase(0), effSpeed(0),
      lastFrameUs(0), nextFrameUs(0) {
}

GratingHandler::GratingHandler(int ledPIN, unsigned long duration)
    : tft(), spr(&tft), W(0), H(0), ledPin(ledPIN), ledConfigured(false),
      duration(duration),
      initialized(false), spriteInternal(GRATING_USE_INTERNAL_RAM),
      spriteActuallyInternal(false), freeInternalAfter(0), trialStartMs(0),
      active(false), period(0), angle(0), contrast(1.0f), cosA(1), sinA(0),
      spriteScanMode(false), periodPx(1), scrollSpan(0), topFix(0), bottomFix(0),
      phase(0), lastScroll(-1), subPixel(0), scanPhase(0), effSpeed(0),
      lastFrameUs(0), nextFrameUs(0) {
}

void GratingHandler::setInternalRAM(bool on) {
  spriteInternal = on;
  if (initialized) allocateSprite(); // reallocate now; caller should redraw
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

  allocateSprite();

  // The LED/backlight pin is configured lazily in switchOn(), so it works even
  // if switchOn() is called before the first drawGrating().

  initialized = true;
}

void GratingHandler::allocateSprite() {
  spr.deleteSprite(); // no-op if not yet created; lets us reallocate on demand

  spr.setColorDepth(16); // always 16bpp; see note in grating.h

  const size_t spriteBytes = static_cast<size_t>(W) * H * 2 + 64;
  const bool havePsram = psramFound();
  void* buf = nullptr;

  if (spriteInternal) {
    // Force the sprite into internal SRAM. setAttribute(PSRAM_ENABLE,false) is
    // not sufficient on the S3: TFT_eSPI falls back to plain calloc(), which the
    // allocator routes to PSRAM for blocks larger than ~16 KB. Raising the
    // "allocate internally below this size" threshold above the sprite size
    // keeps it in SRAM for the duration of the allocation; then we restore it.
    if (havePsram) heap_caps_malloc_extmem_enable(spriteBytes + 4096);
    spr.setAttribute(PSRAM_ENABLE, false);
    buf = spr.createSprite(W, H);
    if (havePsram) heap_caps_malloc_extmem_enable(16384); // restore default routing
    if (!buf) {
      // Internal RAM too fragmented; fall back to PSRAM so the sprite exists.
      spr.setAttribute(PSRAM_ENABLE, true);
      buf = spr.createSprite(W, H);
    }
  } else {
    // Explicitly request PSRAM (frees internal SRAM for other uses).
    spr.setAttribute(PSRAM_ENABLE, true);
    buf = spr.createSprite(W, H);
  }

  // Record where the buffer actually landed and how much internal SRAM remains,
  // for the per-trial profiling line printed by drawGrating().
  spriteActuallyInternal = buf && esp_ptr_internal(buf);
  freeInternalAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
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

void GratingHandler::buildLUT() {
  // Precompute one full cycle of the grayscale grating (period + contrast are
  // fixed for the trial). Called once per drawGrating(); the fill loops below
  // then only index this table instead of calling sinf() per pixel.
  for (int i = 0; i < LUT_SIZE; ++i) {
    float s = sinf(2.0f * PI * i / LUT_SIZE);
    float g = 127.5f + 127.5f * contrast * s;
    if (g < 0.0f) g = 0.0f;
    if (g > 255.0f) g = 255.0f;
    uint8_t gray = static_cast<uint8_t>(lroundf(g));
    lut[i] = tft.color565(gray, gray, gray);
  }
}

#if GRATING_PROFILE
void GratingHandler::profPrintDrawPush(long drawUs, long pushUs, unsigned long totalUs) {
  Serial.print(F("draw="));
  if (drawUs < 0) Serial.print(F("nan")); else Serial.print(drawUs);
  Serial.print(F("us push="));
  if (pushUs < 0) Serial.print(F("nan")); else Serial.print(pushUs);
  Serial.print(F("us total="));
  Serial.print(totalUs);
  Serial.println(F("us"));
}
#endif

uint16_t GratingHandler::gratingColor(float pos) {
  // Map a position (in pixels) to the LUT bucket for its phase within a cycle.
  float c = pos / period;
  c -= floorf(c); // wrap into [0, 1) — handles negative pos correctly
  int idx = static_cast<int>(c * LUT_SIZE) & (LUT_SIZE - 1);
  return lut[idx];
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

#if GRATING_PROFILE
  uint32_t profDrawStart = micros();
  uint32_t profBeforePush = 0, profAfterPush = 0;
#endif

  buildLUT();
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

    if (buf) {
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
      // Fallback if the sprite buffer is unavailable for direct access.
      for (int x = 0; x < scrollSpan; ++x) {
        for (int y = 0; y < H; ++y) {
          spr.drawPixel(x + topFix, y, gratingColor(cosA * x + sinA * y));
        }
      }
    }

#if GRATING_PROFILE
    profBeforePush = micros();
#endif
    spr.pushSprite(0, 0);
#if GRATING_PROFILE
    profAfterPush = micros();
#endif
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
#if GRATING_PROFILE
    profBeforePush = micros();
#endif
    spr.pushSprite(0, 0);
#if GRATING_PROFILE
    profAfterPush = micros();
#endif
  }

#if GRATING_PROFILE
  Serial.print(F("[prof] drawGrating mem="));
  Serial.print(spriteActuallyInternal ? F("SRAM") : F("PSRAM"));
  if (spriteActuallyInternal) {
    Serial.print(F(" freeSRAM="));
    Serial.print(freeInternalAfter);
  }
  Serial.print(' ');
  profPrintDrawPush(static_cast<long>(profBeforePush - profDrawStart),
                    static_cast<long>(profAfterPush - profBeforePush),
                    profAfterPush - profDrawStart);
#endif

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

#if GRATING_PROFILE
  long profDraw = -1, profPush = -1; // -1 => printed as "nan"
  uint32_t profStart = 0, profEnd = 0;
  bool profWorked = false;
#endif

  if (!spriteScanMode) {
    phase += effSpeed * dt;
    phase = fmodf(phase, static_cast<float>(scrollSpan));
    if (phase < 0.0f) phase += scrollSpan;

    // This tiny register write replaces the full-screen pixel transfer, so it
    // has no draw/push stage (reported as nan).
#if GRATING_PROFILE
    profStart = micros();
#endif
    setHardwareScroll(static_cast<int>(phase));
#if GRATING_PROFILE
    profEnd = micros();
    profWorked = true;
#endif
  } else {
    subPixel += effSpeed * dt;
    int n = static_cast<int>(subPixel);
    subPixel -= static_cast<float>(n);

    if (n != 0) {
      int m = (n > 0) ? n : -n;
      if (m > H) m = H;

#if GRATING_PROFILE
      uint32_t ps0 = micros();
#endif
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
#if GRATING_PROFILE
      uint32_t ps1 = micros();
#endif
      spr.pushSprite(0, 0);
#if GRATING_PROFILE
      uint32_t ps2 = micros();
      profDraw = static_cast<long>(ps1 - ps0);
      profPush = static_cast<long>(ps2 - ps1);
      profStart = ps0;
      profEnd = ps2;
      profWorked = true;
#endif
    }
  }

#if GRATING_PROFILE
  // Unified per-frame line for both modes; throttled to keep the log readable.
  if (profWorked) {
    static uint16_t profFrame = 0;
    if ((profFrame++ % 30) == 0) {
      Serial.print(F("[prof] update "));
      profPrintDrawPush(profDraw, profPush, profEnd - profStart);
    }
  }
#endif

  nextFrameUs += FRAME_US;
  now = micros();
  if (static_cast<int32_t>(now - nextFrameUs) >= 0) {
    nextFrameUs = now;
  }

  return true;
}

void GratingHandler::switchOn(bool on) {
  if (ledPin < 0) return;
  // Configure the pin on first use so switchOn() works regardless of whether
  // drawGrating()/initHardware() has run yet.
  if (!ledConfigured) {
    pinMode(ledPin, OUTPUT);
    ledConfigured = true;
  }
  digitalWrite(ledPin, on ? HIGH : LOW);
}
