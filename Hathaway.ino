#include "behavior_board.h"
#include "behavior_task.h"

GratingHandler grating(TFT_BL_PIN);
BuzzerHandler buzzer;
LickHandler lick1, lick2;


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

void playRandomNote() {
  unsigned int freq = FREQS[random(NUM_FREQS)];
  buzzer.playNote(freq, 150);
}

bool handleLick1() {
  unsigned long now = millis();
  if (lick1.update()) {
    Serial.print(F("LICK1,"));
    Serial.println(now);
    return true;
  }
  return false;
}

bool handleLick2() {
  unsigned long now = millis();
  if (lick2.update()) {
    Serial.print(F("LICK2,"));
    Serial.println(now);
    return true;
  }
  return false;
}

void setup() {
  buzzer = BuzzerHandler(BUZZER_PIN);
  lick1 = LickHandler(LICK1_PIN);
  lick2 = LickHandler(LICK2_PIN);

  Serial.begin(115200);
  randomSeed(esp_random()); // hardware RNG seed so trials differ each run
  startTrial();
}

void loop() {
  // update() drives the scroll and returns false once the 3 s trial elapses;
  // immediately start the next randomized trial.
  if (!grating.update()) {
    startTrial();
    playRandomNote();
  }
  buzzer.update();
  handleLick1();
  handleLick2();
}
