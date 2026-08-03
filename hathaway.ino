#include "behavior_board.h"
#include "behavior_task.h"
#include "comms.h"        // telemetry + command facility (protocol.h, comms.cpp)
#include "tasks.h"        // the state machines (task.h, task.cpp, tasks.cpp)

// ===========================================================================
//  HATHAWAY -- behaviour task
//
//  The control loop is three phases, in this order, every cycle:
//
//    1. SENSE   sense() reads every sensor once into an Inputs snapshot and
//               logs the raw events. Nothing decides anything here.
//    2. DECIDE  the active Task's step() consumes that snapshot and pushes
//               Actions. Pure logic -- no pins, no Serial, no millis().
//    3. ACT     act() drains the queue into the controller objects.
//
//  Between 2 and 3, supervise() runs unconditionally: the safety interlocks
//  live there, outside the state machine, where no task can bypass them.
//
//  All serial I/O is handled by the Comms facility: the loop never touches
//  Serial. It only calls Comms::emit() to record an event and Comms::service()
//  to pick up parameter changes from the host.
//
//  To add telemetry or a tunable parameter, add one row to the tables in the
//  REGISTRATION section. To add a task, add a class to tasks.h/tasks.cpp.
// ===========================================================================

GratingHandler grating(TFT_BL_PIN);
BuzzerHandler  buzzer;
LickHandler    lick1, lick2;
Rewarder       rewarder1, rewarder2;
HX711          scale;
SwitchHandler  sw;
Magneto        magnet;

// Task state.
unsigned int rewardNum1, rewardNum2;

// The running state machine, and the id it was started from. TASK (the tunable)
// is the id the host has *asked* for; g_activeTask is the one actually running.
// They differ only while a switch waits for a trial boundary.
Task         *g_task       = nullptr;
unsigned long g_activeTask = 0;
ActionQueue   g_actions;

// Last buzzer state reported to the host, so the TONE sample is emitted once on
// each edge and never repeated.
//
// Two flags, because a pulse train has two nested edges. g_toneOn tracks the
// whole note or train and is what raises EV_TONE_DONE, once, at the end.
// g_pulseOn tracks whether sound is actually coming out, and is what the TONE
// telemetry follows -- so a 3-pulse train logs as three on/off pairs rather
// than one 650 ms block. Both are set at the start in act(), so the first
// onset carries the exact instant playNote/playTrain was called.
static bool     g_toneOn   = false;
static bool     g_pulseOn  = false;
static uint32_t g_toneFreq = 0;   // frequency to report on later pulse onsets


// ===========================================================================
//  REGISTRATION -- the only comms code in this file
// ===========================================================================

// --- outbound messages -----------------------------------------------------
// Every line has the same shape:  <RIG_ID>|NAME:<channel>,<value>,<device_ms>
// Channels are 1-based; a single-channel device reports channel 1.
//
// TELEM_SAMPLE = a signal that holds its value between updates (weight,
// position, magnet).  TELEM_EVENT = an instant (lick, reward). The kind is
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
  TELEM_TONE,     // channel 1, value = frequency Hz while sounding, 0 = silent
  TELEM_STATE,    // channel = state index (0-based), value = trial number
  TELEM_TASK,     // channel = task id,     value = task id
  TELEM_OUTCOME,  // channel = OUTCOME_* code (0-based), value = trial number
};

static const TelemSpec TELEM_TABLE[] = {
  { TELEM_WEIGHT,   "WEIGHT",   TELEM_SAMPLE },
  { TELEM_POSITION, "POSITION", TELEM_SAMPLE },
  { TELEM_MAGNET,   "MAGNET",   TELEM_SAMPLE },
  { TELEM_LICK,     "LICK",     TELEM_EVENT  },
  { TELEM_REWARD,   "REWARD",   TELEM_EVENT  },
  // TONE is a SAMPLE, not an event: it holds the frequency being played between
  // updates and drops back to 0 at the note's end, so it plots as a state
  // alongside POSITION and MAGNET rather than as an instant.
  { TELEM_TONE,     "TONE",     TELEM_SAMPLE },
  { TELEM_STATE,    "STATE",    TELEM_EVENT  },
  { TELEM_TASK,     "TASK",     TELEM_SAMPLE },
  // How each trial ended. An EVENT, like STATE: it is an instant, not a level.
  // The channel is the OUTCOME_* code, so hit / incorrect / no-response / abort
  // rates come straight out of a GROUP BY on channel.
  { TELEM_OUTCOME,  "OUTCOME",  TELEM_EVENT  },
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
  // TASK is applied lazily, at the next trial boundary -- see serviceTask().
  // The PARAM ack therefore means "request accepted"; the TASK telemetry line
  // marks the cycle on which the switch actually happened.
  PARAM_U32(TASK,              1,   3,     nullptr),
  PARAM_U32(T1_SPOUT1_ENABLE,  0,   1,     nullptr),
  PARAM_U32(T1_SPOUT2_ENABLE,  0,   1,     nullptr),
  PARAM_U32(T2_CUE_FREQ,       100, 20000, nullptr),
  PARAM_U32(T2_CUE_DUR,        1,   5000,  nullptr),
  PARAM_U32(T2_CUE_TO_WATER,   0,   5000,  nullptr),
  // Block lengths. Zero is legal and means "this spout gets no trials", which
  // is how task 2 takes a spout out of use; both zero leaves nothing to run.
  PARAM_U32(T2_N1,             0,   10000, nullptr),
  PARAM_U32(T2_N2,             0,   10000, nullptr),
  // --- task 3 --------------------------------------------------------------
  PARAM_U32(T3_SAMPLE_FREQ1,   100, 20000, nullptr),
  PARAM_U32(T3_SAMPLE_FREQ2,   100, 20000, nullptr),
  PARAM_U32(T3_PULSE_MS,       1,   5000,  nullptr),
  PARAM_U32(T3_GAP_MS,         0,   5000,  nullptr),
  // At least one pulse: zero would be a sample epoch with no sample in it.
  PARAM_U32(T3_N_PULSES,       1,   20,    nullptr),
  PARAM_U32(T3_DELAY_MS,       0,   10000, nullptr),
  PARAM_U32(T3_CUE_FREQ,       100, 20000, nullptr),
  PARAM_U32(T3_CUE_DUR,        1,   5000,  nullptr),
  PARAM_U32(T3_RESPONSE_MS,    1,   30000, nullptr),
  PARAM_U32(T3_CONSUME_MS,     0,   30000, nullptr),
  PARAM_U32(T3_PUNISH_MS,      0,   30000, nullptr),
  PARAM_U32(T3_ITI_MS,         0,   30000, nullptr),
  // 1 = strict alternation. Zero is excluded: it would force the type to flip
  // on every trial AND on itself, which is just alternation with a worse name.
  PARAM_U32(T3_MAX_REPEAT,     1,   100,   nullptr),
  PARAM_U32(T3_EARLY_LICK_PUNISH, 0, 1,    nullptr),
  ACTION(TARE, doTare),
};
static const size_t CMD_COUNT = sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0]);


// ===========================================================================
//  PHASE 1 -- SENSE
//
//  Read each sensor exactly once, log what happened, and translate it into the
//  Inputs snapshot. Raw telemetry is emitted here unconditionally, so the log
//  is a complete record of the session regardless of which task is running or
//  whether a spout is enabled.
// ===========================================================================

static Inputs sense() {
  Inputs in;
  in.now = millis();

  // --- licks -------------------------------------------------------------
  // Logged whether or not the spout is enabled: a disabled spout is still a
  // measurement. The enable flags reach the task as levels, below.
  if (lick1.update()) {
    in.events  |= EV_LICK1;
    in.t_lick1  = in.now;
    Comms::emit(TELEM_LICK, 1, 1.0f, in.now);
  }
  if (lick2.update()) {
    in.events  |= EV_LICK2;
    in.t_lick2  = in.now;
    Comms::emit(TELEM_LICK, 2, 1.0f, in.now);
  }

  // --- position switch ---------------------------------------------------
  if (sw.update()) {
    if (sw.getState()) {
      in.events |= EV_SWITCH_ON;
      magnet.magnetic_start();
      Comms::emit(TELEM_POSITION, 1, 1.0f, in.now);
    } else {
      in.events |= EV_SWITCH_OFF;
      magnet.halt();
      Comms::emit(TELEM_POSITION, 1, 0.0f, in.now);
    }
  }
  if (sw.getState()) in.levels |= LV_IN_POSITION;

  // --- buzzer ------------------------------------------------------------
  // update() returns "the note or train is still running", so its falling edge
  // is the end of the whole thing. The buzzer is serviced here rather than in
  // act() because that edge is an input to the task, not an output from it.
  //
  // isPulseOn() is the finer signal: it drops during a train's gaps. Its edges
  // drive the TONE telemetry, so the log shows the pulse pattern. The FIRST
  // onset is reported by act() instead, where the exact call instant is known,
  // which is why act() presets g_pulseOn.
  bool toneNow  = buzzer.update();
  bool pulseNow = buzzer.isPulseOn();

  if (g_pulseOn != pulseNow) {
    Comms::emit(TELEM_TONE, 1, pulseNow ? (float)g_toneFreq : 0.0f, in.now);
    g_pulseOn = pulseNow;
  }
  // Raised after the pulse edge above, so the closing TONE 0 is already on the
  // wire by the time the task is told the train finished.
  if (g_toneOn && !toneNow) {
    in.events |= EV_TONE_DONE;
    g_toneOn = false;
  }
  if (toneNow) in.levels |= LV_TONE_ON;

  // --- load cell ---------------------------------------------------------
  // A continuous signal becomes events by thresholding, so the task layer only
  // ever deals in discrete things.
  if (scale.is_ready()) {
    uint32_t t = millis();          // stamp at capture, not at transmit
    float reading = scale.get_units(1);
    in.weight = reading;
    Comms::emit(TELEM_WEIGHT, 1, reading, t);
    if (reading >= SCALE_HIGH_THRESH) in.events |= EV_WEIGHT_HI;
    if (reading <= SCALE_LOW_THRESH)  in.events |= EV_WEIGHT_LO;
  }

  // --- magnet ------------------------------------------------------------
  static int lastMagnet = -1;       // -1 = unknown, forces the first report
  int magState = magnet.update() ? 1 : 0;
  if (magState != lastMagnet) {
    lastMagnet = magState;
    Comms::emit(TELEM_MAGNET, 1, (float)magState, in.now);
  }

  // --- operator switches -------------------------------------------------
  if (T1_SPOUT1_ENABLE) in.levels |= LV_SPOUT1_EN;
  if (T1_SPOUT2_ENABLE) in.levels |= LV_SPOUT2_EN;

  return in;
}


// ===========================================================================
//  SAFETY -- runs every cycle, outside the state machine
//
//  Nothing a task does can suppress this, and no task can express it. Keep
//  interlocks here, not in tasks.cpp.
// ===========================================================================

static void supervise(const Inputs &in) {
  if (in.has(EV_WEIGHT_HI) || in.has(EV_WEIGHT_LO)) magnet.halt();
}


// ===========================================================================
//  PHASE 3 -- ACT
//
//  The only place in the firmware that turns a task decision into hardware,
//  and therefore the only place that has to log one.
// ===========================================================================

static void act(const ActionQueue &q, uint32_t now) {
  for (uint8_t i = 0; i < q.size(); i++) {
    const Action &a = q.at(i);
    switch (a.verb) {
      case ACT_REWARD:
        // No enable check here. A task that must not water a spout simply does
        // not ask -- task 1 filters the lick, task 2 gives that spout no block.
        // Keeping the policy in the tasks means a REWARD line always follows a
        // decision that is visible in the task's own state trace.
        if (a.a0 == 1) {
          rewarder1.deliver_reward(a.a1);
          rewardNum1++;
          Comms::emit(TELEM_REWARD, 1, (float)rewardNum1, now);
        } else if (a.a0 == 2) {
          rewarder2.deliver_reward(a.a1);
          rewardNum2++;
          Comms::emit(TELEM_REWARD, 2, (float)rewardNum2, now);
        }
        break;

      case ACT_TONE:
        buzzer.playNote(a.a0, a.a1);
        // Opens the TONE state at the frequency requested. sense() closes it
        // with a 0 when the note finishes.
        Comms::emit(TELEM_TONE, 1, (float)a.a0, now);
        g_toneFreq = a.a0;
        g_toneOn   = true;
        g_pulseOn  = true;
        break;

      case ACT_TONE_TRAIN:
        // The pulse shape comes from the tunables rather than from the Action,
        // which carries only two arguments. tasks.cpp computes the train's
        // total length from these same three values to arm its own timeout, so
        // there is one definition of the pattern, not two.
        buzzer.playTrain(a.a0, T3_PULSE_MS, T3_GAP_MS, (uint8_t)T3_N_PULSES);
        Comms::emit(TELEM_TONE, 1, (float)a.a0, now);
        g_toneFreq = a.a0;
        g_toneOn   = true;
        g_pulseOn  = true;   // the first pulse is already sounding
        break;
    }
  }
}


// ===========================================================================
//  TASK SWITCHING
//
//  A switch is deferred until the running task reports it is at a trial
//  boundary, so "SET TASK 2" mid-trial finishes the trial in progress first.
// ===========================================================================

static void serviceTask(uint32_t now) {
  if (TASK == g_activeTask) return;
  if (g_task != nullptr && !g_task->safeToSwitch()) return;

  Task *next = taskById(TASK);
  if (next == nullptr) {          // range-checked by CMD_TABLE; belt and braces
    TASK = g_activeTask;
    return;
  }

  buzzer.stop();                  // never carry a note across a boundary
  if (g_pulseOn) {                // close the TONE state here rather than let
    Comms::emit(TELEM_TONE, 1, 0.0f, now);   // sense() report it, so the
    g_pulseOn = false;                       // incoming task sees no stray
  }                                          // EV_TONE_DONE on its first cycle
  g_toneOn = false;
  g_task       = next;
  g_activeTask = TASK;
  g_task->reset(now);
  Comms::emit(TELEM_TASK, (uint8_t)TASK, (float)TASK, now);
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

  // The grating is initialised but no longer driven from loop(): none of the
  // current tasks use the display, and pushing a sprite every frame was the
  // largest single source of loop-period jitter. To bring it back, call
  // grating.update() from loop() and drive it from a new ACT_ verb.
  grating.switchOn(false);

  Comms::begin(RIG_ID, TELEM_TABLE, TELEM_COUNT, CMD_TABLE, CMD_COUNT);

  rewardNum1 = 0;
  rewardNum2 = 0;

  // Announce the message set, then the parameters in effect, so the host knows
  // both the schema and the settings from the start of the session.
  Comms::dumpSchema();
  Comms::dumpParams();

  // Start whichever task TASK names. g_activeTask is 0, so this always fires.
  serviceTask(millis());
}

void loop() {
  Comms::service();          // apply any parameter changes the host sent

  serviceTask(millis());     // honour a pending "SET TASK n", if it is safe to

  Inputs in = sense();       // 1. SENSE
  supervise(in);             //    safety interlocks, unconditionally

  uint32_t before  = g_task->transitions();
  uint32_t trialsB = g_task->trial();
  g_actions.clear();
  g_task->step(in, g_actions);                               // 2. DECIDE

  // Log the state entered before the actions it triggered, so the log reads in
  // causal order. A counter rather than a state comparison, so that re-entering
  // the same state is still recorded.
  if (g_task->transitions() != before)
    Comms::emit(TELEM_STATE, g_task->state(), (float)g_task->trial(), in.now);

  // A trial just closed, so say how it ended. Watching the counter means the
  // sketch needs to know nothing about which task is running or how many ways
  // it can end -- it just reports the code the task recorded.
  if (g_task->trial() != trialsB)
    Comms::emit(TELEM_OUTCOME, g_task->lastOutcome(), (float)g_task->trial(), in.now);

  act(g_actions, in.now);                                    // 3. ACT

  rewarder1.update();
  rewarder2.update();
}
