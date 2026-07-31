#include "behavior_board.h"
#include "behavior_task.h"
#include "comms.h"        // telemetry + command facility (protocol.h, comms.cpp)

// ===========================================================================
//  HATHAWAY -- behaviour task
//
//  All serial I/O is handled by the Comms facility: the control loop below
//  never touches Serial. It only calls Comms::emit() to record an event and
//  Comms::service() to pick up parameter changes from the host.
//
//  To add telemetry or a tunable parameter, add one row to the tables in the
//  REGISTRATION section. Nothing else needs to change.
// ===========================================================================

GratingHandler grating(TFT_BL_PIN);
BuzzerHandler  buzzer;
LickHandler    lick1, lick2;
Rewarder       rewarder1, rewarder2;
HX711          scale;
SwitchHandler  sw;
Magneto        magnet;

// Task state.
bool          enReward;
unsigned long nextTime;
unsigned int  rewardNum1, rewardNum2;


// ===========================================================================
//  REGISTRATION -- the only comms code in this file
// ===========================================================================

// --- outbound messages -----------------------------------------------------
// Every line has the same shape:  <RIG_ID>|NAME:<channel>,<value>,<device_ms>
// Channels are 1-based; a single-channel device reports channel 1.
//
// TELEM_SAMPLE = a signal that holds its value between updates (weight,
// position, magnet).  TELEM_EVENT = an instant (lick, reward).  The kind is
// announced to the host at startup as "#DEF <NAME>,<S|E>", so adding a message
// here needs no change on the host side at all.
//
// To add a message: add an id below and a row to the table.

enum : uint8_t {
  TELEM_WEIGHT,
  TELEM_POSITION,
  TELEM_MAGNET,
  TELEM_LICK,
  TELEM_REWARD,
};

static const TelemSpec TELEM_TABLE[] = {
  { TELEM_WEIGHT,   "WEIGHT",   TELEM_SAMPLE },
  { TELEM_POSITION, "POSITION", TELEM_SAMPLE },
  { TELEM_MAGNET,   "MAGNET",   TELEM_SAMPLE },
  { TELEM_LICK,     "LICK",     TELEM_EVENT  },
  { TELEM_REWARD,   "REWARD",   TELEM_EVENT  },
};
static const size_t TELEM_COUNT = sizeof(TELEM_TABLE) / sizeof(TELEM_TABLE[0]);

// --- inbound commands ------------------------------------------------------
// Parameters are set live with "SET <NAME> <VALUE>"; actions are sent bare.
// A parameter row names its variable once: the wire name is the C identifier,
// so the two can never drift apart. Range checks, "PARAM:" acks, "DUMP"/"GET"
// and "#ERR" replies are all handled by the engine.
//
// The apply function is optional -- it is only needed when a device object has
// to be told about the new value. It runs on the control core, right after the
// variable is written.

static void applyRewardDuration1(float v) { rewarder1.setRewardDuration((unsigned long)v); }
static void applyRewardDuration2(float v) { rewarder2.setRewardDuration((unsigned long)v); }
static void applyMagFixDuration(float v)  { magnet.setFixDuration((unsigned long)v); }
static void doTare(float)                 { scale.tare(); }   // blocks ~1 s

static const CmdSpec CMD_TABLE[] = {
  PARAM_U32(REWARD_DURATION1,  0,   1000,  applyRewardDuration1),
  PARAM_U32(REWARD_DURATION2,  0,   1000,  applyRewardDuration2),
  PARAM_U32(REWARD_INTERVAL1,  0,   60000, nullptr),
  PARAM_U32(REWARD_INTERVAL2,  0,   60000, nullptr),
  PARAM_U32(MAG_FIX_DURATION,  0,   60000, applyMagFixDuration),
  PARAM_F32(SCALE_HIGH_THRESH, -50, 50,    nullptr),
  PARAM_F32(SCALE_LOW_THRESH,  -50, 50,    nullptr),
  ACTION(TARE, doTare),
};
static const size_t CMD_COUNT = sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0]);


// ===========================================================================
//  TASK
// ===========================================================================

void startTrial() {
  float angle    = ANGLES[random(NUM_ANGLES)];
  float contrast = CONTRASTS[random(NUM_CONTRASTS)];
  grating.drawGrating(PERIOD, angle, contrast);
  grating.configScroll(SPEED);
}

void playRandomNote() {
  unsigned int freq = FREQS[random(NUM_FREQS)];
  buzzer.playNote(freq, 150);
}

// NOTE: enReward / nextTime are shared by both spouts, so a reward at either
// spout gates the other, and the interval used is that of the spout that just
// rewarded. Existing behaviour, left unchanged.
bool handleLick1() {
  unsigned long now = millis();
  if (lick1.update()) {
    Comms::emit(TELEM_LICK, 1, 1.0f, now);
    if (enReward) {
      rewarder1.deliver_reward();
      rewardNum1++;
      enReward = false;
      nextTime = REWARD_INTERVAL1 + now;   // shared gate, spout-1 refractory
      Comms::emit(TELEM_REWARD, 1, (float)rewardNum1, now);
    }
    return true;
  }
  if (!enReward && now >= nextTime) enReward = true;
  return false;
}

bool handleLick2() {
  unsigned long now = millis();
  if (lick2.update()) {
    Comms::emit(TELEM_LICK, 2, 1.0f, now);
    if (enReward) {
      rewarder2.deliver_reward();
      rewardNum2++;
      enReward = false;
      nextTime = REWARD_INTERVAL2 + now;   // shared gate, spout-2 refractory
      Comms::emit(TELEM_REWARD, 2, (float)rewardNum2, now);
    }
    return true;
  }
  if (!enReward && now >= nextTime) enReward = true;
  return false;
}

bool handleSwitch() {
  unsigned long now = millis();
  if (sw.update()) {
    if (sw.getState()) {
      magnet.magnetic_start();
      Comms::emit(TELEM_POSITION, 1, 1.0f, now);
    } else {
      magnet.halt();
      Comms::emit(TELEM_POSITION, 1, 0.0f, now);
    }
    return true;
  }
  return false;
}

void handleScale() {
  if (scale.is_ready()) {
    unsigned long now = millis();   // stamp at capture, not at transmit
    float reading = scale.get_units(1);
    Comms::emit(TELEM_WEIGHT, 1, reading, now);
    if (reading >= SCALE_HIGH_THRESH || reading <= SCALE_LOW_THRESH) {
      magnet.halt();
    }
  }
}

void handleMagnet() {
  static int lastMagnet = -1;   // -1 = unknown, forces the first report
  unsigned long now = millis();
  int state = magnet.update() ? 1 : 0;
  if (state != lastMagnet) {
    lastMagnet = state;
    Comms::emit(TELEM_MAGNET, 1, (float)state, now);
  }
}


// ===========================================================================
//  SETUP / LOOP
// ===========================================================================

void setup() {
  buzzer    = BuzzerHandler(BUZZER_PIN);
  lick1     = LickHandler(LICK1_PIN);
  lick2     = LickHandler(LICK2_PIN);
  rewarder1 = Rewarder(SPOUT1_PIN, REWARD_DURATION1);
  rewarder2 = Rewarder(SPOUT2_PIN, REWARD_DURATION2);
  sw        = SwitchHandler(SWITCH_PIN);
  magnet    = Magneto(MAGNET_PIN, MAG_FIX_DURATION);

  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale();
  scale.tare();
  scale.set_scale(636.5f);

  randomSeed(esp_random());   // hardware RNG seed so trials differ each run
  grating.switchOn(false);

  Comms::begin(RIG_ID, TELEM_TABLE, TELEM_COUNT, CMD_TABLE, CMD_COUNT);

  startTrial();
  enReward   = true;
  rewardNum1 = 0;
  rewardNum2 = 0;

  // Announce the message set, then the parameters in effect, so the host knows
  // both the schema and the settings from the start of the session.
  Comms::dumpSchema();
  Comms::dumpParams();
}

void loop() {
  Comms::service();   // apply any parameter changes the host sent

  // update() drives the scroll and returns false once the trial elapses;
  // immediately start the next randomized trial.
  if (!grating.update()) {
    startTrial();
    playRandomNote();
  }
  buzzer.update();
  handleLick1();
  handleLick2();
  rewarder1.update();
  rewarder2.update();
  handleSwitch();
  handleMagnet();
  handleScale();
}
