#include "grating.h"

// ledPIN is available for other uses (not the grating on/off). Duration defaults to 3 s.
#define TFT_BL_PIN A2

GratingHandler grating(TFT_BL_PIN);

// Trial parameter pools.
const float ANGLES[]    = {0, 45, 90, 135};
const float CONTRASTS[] = {0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
const int   NUM_ANGLES    = sizeof(ANGLES) / sizeof(ANGLES[0]);
const int   NUM_CONTRASTS = sizeof(CONTRASTS) / sizeof(CONTRASTS[0]);

const float PERIOD = 45.0f;  // grating period, px
const float SPEED  = 160.0f; // drift speed, px/s

void startTrial() {
  float angle    = ANGLES[random(NUM_ANGLES)];
  float contrast = CONTRASTS[random(NUM_CONTRASTS)];

  Serial.print("Trial -> angle: ");
  Serial.print(angle, 0);
  Serial.print(" deg, contrast: ");
  Serial.println(contrast, 1);

  grating.drawGrating(PERIOD, angle, contrast);
  grating.configScroll(SPEED);
}

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random()); // hardware RNG seed so trials differ each run

  startTrial();
}

void loop() {
  // update() drives the scroll and returns false once the 3 s trial elapses;
  // immediately start the next randomized trial.
  if (!grating.update()) {
    startTrial();
  }
}
