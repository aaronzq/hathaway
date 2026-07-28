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

// --------------------------------------------------------------------------- //
// Telemetry pipeline
// The control loop (core 1) only enqueues small records; a comms task on core 0
// owns Serial and formats/writes the wire lines. This keeps blocking Serial I/O
// off the control loop, so sensor reading and control never stall on the UART.
// The wire format is unchanged, so ingest.py / Grafana need no changes.
// --------------------------------------------------------------------------- //
enum TelemType : uint8_t {
  TELEM_WEIGHT,     // "HX711 reading: <float>"   (no device timestamp)
  TELEM_POSITION,   // "POSITION:<0|1>,<ms>"
  TELEM_MAGNET,     // "MAGNET:<0|1>,<ms>"
  TELEM_LICK,       // "LICK<ch>,<ms>"
  TELEM_REWARD,     // "REWARD:<count>,<ms>"
};

struct TelemRec {
  uint8_t  type;      // TelemType
  uint8_t  channel;   // LICK spout (1/2); unused otherwise
  float    value;     // weight, 0/1 state, or reward count
  uint32_t dev_ms;    // millis() captured at the event
};

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

// Runs on core 0. Owns Serial; blocking writes here are harmless.
void commsTask(void *pv) {
  TelemRec r;
  char line[48];
  for (;;) {
    if (xQueueReceive(telemQueue, &r, portMAX_DELAY) != pdTRUE) continue;
    int n = 0;
    switch (r.type) {
      case TELEM_WEIGHT:   // no device timestamp, matching the original line
        n = snprintf(line, sizeof(line), "HX711 reading: %.2f\n", r.value);
        break;
      case TELEM_POSITION:
        n = snprintf(line, sizeof(line), "POSITION:%d,%lu\n",
                     (int)r.value, (unsigned long)r.dev_ms);
        break;
      case TELEM_MAGNET:
        n = snprintf(line, sizeof(line), "MAGNET:%d,%lu\n",
                     (int)r.value, (unsigned long)r.dev_ms);
        break;
      case TELEM_LICK:
        n = snprintf(line, sizeof(line), "LICK%u,%lu\n",
                     (unsigned)r.channel, (unsigned long)r.dev_ms);
        break;
      case TELEM_REWARD:
        n = snprintf(line, sizeof(line), "REWARD:%u,%lu\n",
                     (unsigned)r.value, (unsigned long)r.dev_ms);
        break;
    }
    if (n > 0) Serial.write((const uint8_t*)line, (size_t)n);
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
      rewardNum ++;
      enReward = false;
      nextTime = REWARD_INTERVAL + now;
      emitTelem(TELEM_REWARD, 0, (float)rewardNum, now);
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

    emitTelem(TELEM_LICK, 2, 1.0f, now);
    return true;
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

  // Telemetry pipeline: create the queue, then start the comms task on core 0.
  // (Serial must be up first, since the comms task owns it.)
  telemQueue = xQueueCreate(TELEM_QUEUE_LEN, sizeof(TelemRec));
  xTaskCreatePinnedToCore(commsTask, "comms", 4096, nullptr, 1,
                          &commsTaskHandle, 0);   // core 0 owns Serial

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
