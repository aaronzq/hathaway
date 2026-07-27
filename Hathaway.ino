#include "behavior_board.h"
#include "behavior_task.h"

GratingHandler grating(TFT_BL_PIN);
BuzzerHandler buzzer;
LickHandler lick1, lick2;
Rewarder rewarder1, rewarder2;
HX711 scale;
SwitchHandler sw;
Magneto magnet;

bool enReward;
unsigned long nextTime;
unsigned int rewardNum;

void startTrial() {
  float angle    = ANGLES[random(NUM_ANGLES)];
  float contrast = CONTRASTS[random(NUM_CONTRASTS)];

  // Serial.print("Trial -> angle: ");
  // Serial.print(angle, 0);
  // Serial.print(" deg, contrast: ");
  // Serial.println(contrast, 1);

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
    if (enReward) {
      rewarder1.deliver_reward();
      rewardNum ++;
      enReward = false;
      nextTime = REWARD_INTERVAL + now;
      Serial.print(F("REWARD:"));
      Serial.print(rewardNum);
      Serial.print(",");
      Serial.println(now);
    } 
    return true;
  }
  if (!enReward) {
    if (now >= nextTime) {
      enReward = true;
    }
  }
  return false;
}

bool handleLick2() {
  unsigned long now = millis();
  if (lick2.update()) {

    rewarder2.deliver_reward();

    Serial.print(F("LICK2,"));
    Serial.println(now);
    return true;
  }
  return false;
}

bool handleSwitch() {
  unsigned long now = millis();
  if (sw.update()) {
    if (sw.getState()) {
      magnet.magnetic_start();
      Serial.print(F("POSITION:1,"));
    } else {
      magnet.halt();
      Serial.print(F("POSITION:0,"));
    }
    Serial.println(now);
    return true;
  }
  return false;
}

void handleScale() {
  if (scale.is_ready()) {
    float reading = scale.get_units(1);
    Serial.print("HX711 reading: ");
    Serial.println(reading);
    if (reading >= SCALE_HIGH_THRESH || reading <= SCALE_LOW_THRESH) {
      magnet.halt();
    }
  } 
}

void handleManget() {
  static int lastMagnet = -1;   // -1 = unknown, forces first print
  unsigned long now = millis();
  int state = magnet.update() ? 1 : 0;
  if (state != lastMagnet) {
    lastMagnet = state;
    Serial.print(state ? F("MAGNET:1,") : F("MAGNET:0,"));
    Serial.println(now);
  }
}

void setup() {
  buzzer = BuzzerHandler(BUZZER_PIN);
  lick1 = LickHandler(LICK1_PIN);
  lick2 = LickHandler(LICK2_PIN);
  rewarder1 = Rewarder(SPOUT1_PIN, REWARD_DURATION);  // use default duration
  rewarder2 = Rewarder(SPOUT2_PIN, REWARD_DURATION);
  sw = SwitchHandler(SWITCH_PIN);
  magnet = Magneto(MAGNET_PIN, MAG_FIX_DURATION);

  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale();	
  scale.tare();	
  scale.set_scale(640.7f);
  
  randomSeed(esp_random()); // hardware RNG seed so trials differ each run
  grating.switchOn(false);

  Serial.begin(115200);

  startTrial();
  enReward = true;
  rewardNum = 0;
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
  // handleLick2();
  rewarder1.update();
  // rewarder2.update();
  handleSwitch();
  handleManget();
  handleScale();
}
