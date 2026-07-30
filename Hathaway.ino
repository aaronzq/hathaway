#include "behavior_board.h"
#include "behavior_task.h"
#include "protocol.h"      // TelemType/TelemRec/ParamId/ParamCmd/ParamSpec

GratingHandler grating(TFT_BL_PIN);
BuzzerHandler buzzer;
LickHandler lick1, lick2;
Rewarder rewarder1, rewarder2;
HX711 scale;
SwitchHandler sw;
Magneto magnet;

bool enReward;
unsigned long nextTime;
unsigned int rewardNum1, rewardNum2;

// --------------------------------------------------------------------------- //
// Telemetry pipeline
// The control loop (core 1) only enqueues small records; a comms task on core 0
// owns Serial and formats/writes the wire lines. This keeps blocking Serial I/O
// off the control loop, so sensor reading and control never stall on the UART.
// Every emitted line is prefixed with "<RIG_ID>|" so a database fed by several
// rigs can tell them apart, e.g. "1|LICK1,12345". REWARD also carries the spout
// channel: "1|REWARD:<ch>,<count>,<ms>". ingest.py must strip the "<id>|" prefix.
//
// Tunable parameters are set live from the host with a "SET <NAME> <VALUE>" line.
// The comms task (core 0) parses/validates it and enqueues a ParamCmd; the
// control loop (core 1) applies it, so the parameter globals and the
// rewarder/magnet objects only ever change from one core. Each applied change is
// echoed as a PARAM: ack. (Types: TelemRec / ParamCmd / ParamSpec in protocol.h)
// --------------------------------------------------------------------------- //
static const ParamSpec PARAM_TABLE[] = {
  { "REWARD_DURATION1",  PARAM_REWARD_DURATION1, 0,  1000 },
  { "REWARD_DURATION2",  PARAM_REWARD_DURATION2, 0,  1000 },
  { "REWARD_INTERVAL1",  PARAM_REWARD_INTERVAL1, 0,  60000 },
  { "REWARD_INTERVAL2",  PARAM_REWARD_INTERVAL2, 0,  60000 },
  { "MAG_FIX_DURATION",  PARAM_MAG_FIX_DURATION, 0,  60000 },
  { "SCALE_HIGH_THRESH", PARAM_SCALE_HIGH,   -50,  50 },
  { "SCALE_LOW_THRESH",  PARAM_SCALE_LOW,    -50,  50 },
};
static const int PARAM_COUNT = sizeof(PARAM_TABLE) / sizeof(PARAM_TABLE[0]);

static const char *paramName(uint8_t id) {
  for (int i = 0; i < PARAM_COUNT; i++)
    if (PARAM_TABLE[i].id == id) return PARAM_TABLE[i].name;
  return "?";
}

QueueHandle_t cmdQueue = nullptr;   // comms task -> control loop

static const int TELEM_QUEUE_LEN = 128;
QueueHandle_t telemQueue = nullptr;
TaskHandle_t  commsTaskHandle = nullptr;
volatile uint32_t telemDropped = 0;   // records dropped when the queue is full

// Called from the control core. Non-blocking: never stalls the loop.
static inline void emitTelem(uint8_t type, uint8_t channel, float value,
                             uint32_t dev_ms) {
  TelemRec r = { type, channel, value, dev_ms };
  if (telemQueue == nullptr || xQueueSend(telemQueue, &r, 0) != pdTRUE) {
    telemDropped++;   // drop rather than block control
  }
}

// Format a telemetry record into its exact wire line, prefixed with "<RIG_ID>|".
static int formatTelem(char *line, size_t cap, const TelemRec &r) {
  int p = snprintf(line, cap, "%d|", RIG_ID);   // rig prefix on every line
  if (p < 0 || (size_t)p >= cap) return 0;
  char *b = line + p;
  size_t c = cap - p;
  int m = 0;
  switch (r.type) {
    case TELEM_WEIGHT:   // no device timestamp, matching the original line
      m = snprintf(b, c, "HX711 reading: %.2f\n", r.value); break;
    case TELEM_POSITION:
      m = snprintf(b, c, "POSITION:%d,%lu\n",
                   (int)r.value, (unsigned long)r.dev_ms); break;
    case TELEM_MAGNET:
      m = snprintf(b, c, "MAGNET:%d,%lu\n",
                   (int)r.value, (unsigned long)r.dev_ms); break;
    case TELEM_LICK:
      m = snprintf(b, c, "LICK%u,%lu\n",
                   (unsigned)r.channel, (unsigned long)r.dev_ms); break;
    case TELEM_REWARD:
      m = snprintf(b, c, "REWARD:%u,%u,%lu\n",
                   (unsigned)r.channel, (unsigned)r.value,
                   (unsigned long)r.dev_ms); break;
    case TELEM_PARAM:
      m = snprintf(b, c, "PARAM:%s,%g\n", paramName(r.channel), r.value); break;
    case TELEM_TARE:
      m = snprintf(b, c, "#TARE ok\n"); break;
    default:
      return 0;
  }
  return (m > 0) ? p + m : 0;
}

// Parse one inbound line ("SET <NAME> <VALUE>") and, if valid, enqueue a
// ParamCmd for the control loop to apply. Runs on the comms task (core 0),
// which owns Serial, so error replies are written directly here. The success
// ack (PARAM:) is emitted by the control loop once the change is applied.
static void handleCommandLine(const char *s) {
  // "DUMP" / "GET" -> re-emit all current parameters (control loop does it).
  if (strncmp(s, "DUMP", 4) == 0 || strncmp(s, "GET", 3) == 0) {
    ParamCmd cmd = { PARAM_DUMP, 0.0f };
    if (cmdQueue) xQueueSend(cmdQueue, &cmd, 0);
    return;
  }
  // "TARE" -> run scale.tare() once (on the control loop).
  if (strncmp(s, "TARE", 4) == 0) {
    ParamCmd cmd = { PARAM_TARE, 0.0f };
    if (cmdQueue) xQueueSend(cmdQueue, &cmd, 0);
    return;
  }
  char name[24];
  double val;
  if (sscanf(s, "SET %23s %lf", name, &val) != 2) {
    Serial.printf("%d|#ERR parse: %s\n", RIG_ID, s);
    return;
  }
  for (int i = 0; i < PARAM_COUNT; i++) {
    if (strcmp(name, PARAM_TABLE[i].name) == 0) {
      if (val < PARAM_TABLE[i].lo || val > PARAM_TABLE[i].hi) {
        Serial.printf("%d|#ERR range: %s=%g\n", RIG_ID, name, val);
        return;
      }
      ParamCmd cmd = { PARAM_TABLE[i].id, (float)val };
      if (cmdQueue == nullptr || xQueueSend(cmdQueue, &cmd, 0) != pdTRUE)
        Serial.printf("%d|#ERR busy: %s\n", RIG_ID, name);
      return;
    }
  }
  Serial.printf("%d|#ERR unknown: %s\n", RIG_ID, name);
}

// Runs on core 0. Owns Serial: writes telemetry AND reads inbound commands.
void commsTask(void *pv) {
  TelemRec r;
  char line[64];   // room for the "<rig>|" prefix plus the longest line
  static char rx[64];
  static size_t rxlen = 0;
  for (;;) {
    // 1) Drain outgoing telemetry (short wait so we also poll input).
    if (xQueueReceive(telemQueue, &r, pdMS_TO_TICKS(5)) == pdTRUE) {
      int n = formatTelem(line, sizeof(line), r);
      if (n > 0) Serial.write((const uint8_t*)line, (size_t)n);
      while (xQueueReceive(telemQueue, &r, 0) == pdTRUE) {   // flush the rest
        n = formatTelem(line, sizeof(line), r);
        if (n > 0) Serial.write((const uint8_t*)line, (size_t)n);
      }
    }
    // 2) Read inbound bytes; dispatch on each complete line.
    while (Serial.available() > 0) {
      char c = (char)Serial.read();
      if (c == '\n' || c == '\r') {
        if (rxlen > 0) { rx[rxlen] = '\0'; handleCommandLine(rx); rxlen = 0; }
      } else if (rxlen < sizeof(rx) - 1) {
        rx[rxlen++] = c;
      } else {
        rxlen = 0;   // overflow; drop the line
      }
    }
  }
}

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
    emitTelem(TELEM_LICK, 1, 1.0f, now);
    if (enReward) {
      rewarder1.deliver_reward();
      rewardNum1 ++;
      enReward = false;
      nextTime = REWARD_INTERVAL1 + now;   // shared gate, spout-1 refractory
      emitTelem(TELEM_REWARD, 1, (float)rewardNum1, now);
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
    emitTelem(TELEM_LICK, 2, 1.0f, now);
    if (enReward) {
      rewarder2.deliver_reward();
      rewardNum2 ++;
      enReward = false;
      nextTime = REWARD_INTERVAL2 + now;   // shared gate, spout-2 refractory
      emitTelem(TELEM_REWARD, 2, (float)rewardNum2, now);
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

bool handleSwitch() {
  unsigned long now = millis();
  if (sw.update()) {
    if (sw.getState()) {
      magnet.magnetic_start();
      emitTelem(TELEM_POSITION, 0, 1.0f, now);
    } else {
      magnet.halt();
      emitTelem(TELEM_POSITION, 0, 0.0f, now);
    }
    return true;
  }
  return false;
}

void handleScale() {
  if (scale.is_ready()) {
    float reading = scale.get_units(1);
    emitTelem(TELEM_WEIGHT, 0, reading, millis());
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
    emitTelem(TELEM_MAGNET, 0, (float)state, now);
  }
}

// Current value of a tunable parameter (for acks / dumps).
static float paramValue(uint8_t id) {
  switch (id) {
    case PARAM_REWARD_DURATION1: return (float)REWARD_DURATION1;
    case PARAM_REWARD_DURATION2: return (float)REWARD_DURATION2;
    case PARAM_REWARD_INTERVAL1: return (float)REWARD_INTERVAL1;
    case PARAM_REWARD_INTERVAL2: return (float)REWARD_INTERVAL2;
    case PARAM_MAG_FIX_DURATION: return (float)MAG_FIX_DURATION;
    case PARAM_SCALE_HIGH:       return SCALE_HIGH_THRESH;
    case PARAM_SCALE_LOW:        return SCALE_LOW_THRESH;
  }
  return 0.0f;
}

// Emit every current parameter as a PARAM: line so the host/database learns the
// full parameter state in effect (used at startup and on a DUMP/GET command).
static void dumpParams(uint32_t now) {
  for (int i = 0; i < PARAM_COUNT; i++)
    emitTelem(TELEM_PARAM, PARAM_TABLE[i].id, paramValue(PARAM_TABLE[i].id), now);
}

// Apply a validated parameter change on the control core, then ack it.
// Runs only here, so the tunable globals and rewarder/magnet objects have a
// single writer (no cross-core races with the comms task).
static void applyParam(const ParamCmd &cmd) {
  if (cmd.id == PARAM_DUMP) { dumpParams(millis()); return; }
  if (cmd.id == PARAM_TARE) {          // one-shot: zero the scale
    scale.tare();                      // blocks ~1 s (averages several readings)
    emitTelem(TELEM_TARE, 0, 0.0f, millis());
    return;
  }
  switch (cmd.id) {
    case PARAM_REWARD_DURATION1:
      REWARD_DURATION1 = (unsigned long)cmd.value;
      rewarder1.setRewardDuration(REWARD_DURATION1);
      break;
    case PARAM_REWARD_DURATION2:
      REWARD_DURATION2 = (unsigned long)cmd.value;
      rewarder2.setRewardDuration(REWARD_DURATION2);
      break;
    case PARAM_REWARD_INTERVAL1:
      REWARD_INTERVAL1 = (unsigned long)cmd.value;
      break;
    case PARAM_REWARD_INTERVAL2:
      REWARD_INTERVAL2 = (unsigned long)cmd.value;
      break;
    case PARAM_MAG_FIX_DURATION:
      MAG_FIX_DURATION = (unsigned long)cmd.value;
      magnet.setFixDuration(MAG_FIX_DURATION);
      break;
    case PARAM_SCALE_HIGH:
      SCALE_HIGH_THRESH = cmd.value;
      break;
    case PARAM_SCALE_LOW:
      SCALE_LOW_THRESH = cmd.value;
      break;
  }
  emitTelem(TELEM_PARAM, cmd.id, cmd.value, millis());   // PARAM: ack
}

void setup() {
  buzzer = BuzzerHandler(BUZZER_PIN);
  lick1 = LickHandler(LICK1_PIN);
  lick2 = LickHandler(LICK2_PIN);
  rewarder1 = Rewarder(SPOUT1_PIN, REWARD_DURATION1);
  rewarder2 = Rewarder(SPOUT2_PIN, REWARD_DURATION2);
  sw = SwitchHandler(SWITCH_PIN);
  magnet = Magneto(MAGNET_PIN, MAG_FIX_DURATION);

  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale();	
  scale.tare();	
  scale.set_scale(636.5f);
  
  randomSeed(esp_random()); // hardware RNG seed so trials differ each run
  grating.switchOn(false);

  Serial.begin(115200);

  // Telemetry pipeline: create the queue, then start the comms task on core 0.
  // (Serial must be up first, since the comms task owns it.)
  telemQueue = xQueueCreate(TELEM_QUEUE_LEN, sizeof(TelemRec));
  cmdQueue   = xQueueCreate(8, sizeof(ParamCmd));   // host SET commands
  xTaskCreatePinnedToCore(commsTask, "comms", 4096, nullptr, 1,
                          &commsTaskHandle, 0);   // core 0 owns Serial

  startTrial();
  enReward = true;
  rewardNum1 = 0;
  rewardNum2 = 0;

  // Affirm the current parameters at startup so the database knows what was in
  // effect from the beginning of this session.
  dumpParams(millis());
}

void loop() {
  // Apply any parameter changes the host sent via SET (validated on core 0).
  ParamCmd cmd;
  while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) applyParam(cmd);

  // update() drives the scroll and returns false once the 3 s trial elapses;
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
  handleManget();
  handleScale();
}
