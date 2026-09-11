#include "behavior_board.h"
#include "behavior_task.h"
#include "comms.h"        // telemetry + command facility (protocol.h, comms.cpp)
#include "railHandler.h"  // stepper rail (FastAccelStepper)
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
RailHandler    rail;

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
//  RAIL STATE
//
//  The rail is open loop: the only record of where it is, is the count of
//  pulses the firmware believes it has sent. That belief survives only if
//  nothing ever disturbs a move in progress, so exactly one move may be in
//  flight at a time and only RAIL_STOP may interrupt it.
//
//  Why a flag and not just the stepper's isRunning(): commanding a move while
//  the rail is already moving does not fail. FastAccelStepper retargets,
//  relative to the target of the move in flight, so two clicks on Move would
//  travel the sum of both. g_railBusy refuses the second command, and unlike a
//  hardware read it can say WHY, so the refusal gets logged.
//
//  isRunning() is what ENDS a move, because only the hardware knows when the
//  last pulse went out -- especially after a stop, which finishes somewhere
//  nobody predicted. Reading the position at that moment is also what makes it
//  exact: on the ESP32 the library warns that getCurrentPosition() can be off
//  by the steps of the command in progress, and that is over precisely when
//  isRunning() goes false.
// ===========================================================================

static bool     g_railBusy     = false; // a move has been ordered and not finished
static uint32_t g_railDeadline = 0;     // backstop: give up waiting at this time

// How long a move of `pulses` should take, rounded up: the ramp is near-instant
// at the configured acceleration, so travel time dominates. Never used to
// decide a move HAS finished -- only how soon it possibly could have.
static uint32_t railExpectedMs(int32_t pulses) {
  if (pulses == INT32_MIN) return 0;    // -INT32_MIN would overflow; unreachable
  if (pulses < 0) pulses = -pulses;
  return (uint32_t)((1000.0f * (float)pulses) / (float)RAIL_DEFAULT_SPEED_HZ) + 50u;
}


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
  TELEM_T3_PROB1, // channel 1, value = effective type-1 draw probability %
  TELEM_RAIL_CMD, // channel = RAIL_CMD_* disposition, value = commanded mm
  TELEM_RAIL_POS, // channel 1, value = rail position in mm
};

// Why a RAIL_CMD line was written. The disposition is on the wire so that
// "was this move performed?" is a query on the channel, not an inference from
// whether the position afterwards happens to differ.
enum : uint8_t {
  RAIL_CMD_ACCEPTED = 1,   // move started; a RAIL_POS follows when it ends
  RAIL_CMD_REFUSED  = 2,   // rail was already moving; nothing happened
  RAIL_CMD_SET_HOME = 3,   // position redefined as zero; nothing moved
  RAIL_CMD_STOP     = 4,   // move cut short on request
  RAIL_CMD_TIMEOUT  = 5,   // backstop fired: the move never reported finishing
  // No stepper attached, or it would not take the ramp. Kept apart from
  // REFUSED: one means "ask again in a moment", the other means the rail is
  // not going to work until someone looks at the wiring.
  RAIL_CMD_UNAVAILABLE = 6,
  // Accepted, but nobody asked for it: task 1's automatic retraction. A
  // separate code rather than a flag elsewhere, so "did the rig move the rail
  // or did the operator" is one WHERE clause and never an inference from
  // timestamps.
  RAIL_CMD_AUTO     = 7,
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
  // Task-3-only state sample: the probability fed into that trial's type draw.
  { TELEM_T3_PROB1, "T3_PROB1", TELEM_SAMPLE },
  // Rail commands and rail position, both in millimetres. An EVENT for the
  // instruction and a SAMPLE for the resulting place: the command is an instant
  // with a disposition, the position is a level that holds until the next move.
  // A pulse-denominated command is converted to mm before it is reported, so
  // there is exactly one unit in the log whichever way the operator asked.
  { TELEM_RAIL_CMD, "RAIL_CMD", TELEM_EVENT  },
  { TELEM_RAIL_POS, "RAIL_POS", TELEM_SAMPLE },
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
static void applyLickDebounceTime(float v) {
  lick1.setDebounceTime((unsigned long)v);
  lick2.setDebounceTime((unsigned long)v);
}
static void applyMagFixDuration(float v)  { magnet.setFixDuration((unsigned long)v); }
static void applyMagGrace(float v)        { magnet.setGraceDuration((unsigned long)v); }
static void applyBuzPulseWidth(float v)   { buzzer.setPulseWidth((uint8_t)v); }
static void applyT3AntiBiasAuto(float v)  {
  if ((unsigned long)v == 0) {
    Task *t3 = taskById(3);
    if (t3 != nullptr) t3->clearT3AntiBiasHistory();
  }
}
// Turning the retraction off discards its reward history, so switching it back
// on always starts from an empty window rather than acting on rewards earned at
// a rail position, or under settings, that no longer apply. Same shape as
// applyT3AntiBiasAuto above: clear on the write of zero, so no previous-value
// bookkeeping is needed.
static void applyT1RailAuto(float v) {
  if ((unsigned long)v == 0) {
    Task *t1 = taskById(1);
    if (t1 != nullptr) t1->clearT1RailWindow();
  }
}
static void doTare(float)                 { scale.tare(); }   // blocks ~1 s

// --- rail ------------------------------------------------------------------
// All four run on the control core, from Comms::service(). movePulses() and
// stopMove() are non-blocking: they hand work to the stepper's background
// helper and return, so none of these holds up the loop.
//
// Three of them refuse outright while a move is in flight. That is the whole
// point of g_railBusy -- see RAIL STATE above. Refusals are reported rather
// than swallowed, because the engine's "#NAME ok" ack only means the line was
// received, and a silent refusal would look exactly like a completed move to
// anyone reading the log later.

static void railReportPos() {
  Comms::emit(TELEM_RAIL_POS, 1, rail.currentPositionMm(), millis());
}

// Order a move of `pulses`. `mm` is the same distance in millimetres, which is
// what gets logged: the caller converts, so the two never disagree. `okCode` is
// the disposition to record on success -- ACCEPTED for an operator command, AUTO
// for task 1's retraction. One entry point for both, so the interlock, the
// backstop and the position report cannot drift apart between them.
static void railStartMove(int32_t pulses, float mm, uint8_t okCode) {
  uint32_t now      = millis();
  uint32_t expected = railExpectedMs(pulses);
  if (g_railBusy) {
    Comms::emit(TELEM_RAIL_CMD, RAIL_CMD_REFUSED, mm, now);
    return;
  }
  // Claimed BEFORE the move is handed over, so a second command arriving in the
  // same service() drain, or on the very next loop, finds the rail busy even
  // though the hardware has not started stepping yet.
  g_railBusy     = true;
  g_railDeadline = now + 3u * expected + 500u;

  if (!rail.movePulses(pulses)) {
    g_railBusy = false;
    // movePulses() refuses for two unrelated reasons: there is no stepper, or
    // the stepper still reports itself running. The second is only reachable
    // after the backstop released the interlock early, and it means "try
    // again", not "check the wiring" -- so ask which it was rather than
    // reporting both as the same fault.
    Comms::emit(TELEM_RAIL_CMD,
                rail.isRunning() ? RAIL_CMD_REFUSED : RAIL_CMD_UNAVAILABLE,
                mm, now);
    return;
  }
  Comms::emit(TELEM_RAIL_CMD, okCode, mm, now);
}

static void doRailMoveMm(float mm) {
  railStartMove(RailHandler::mmToPulses(mm), mm, RAIL_CMD_ACCEPTED);
}

static void doRailMovePulses(float pulses) {
  int32_t p = (int32_t)pulses;         // whole number guaranteed by ACTION_I32
  railStartMove(p, (float)p / RAIL_CALIBRATION_MM_TO_PULSE, RAIL_CMD_ACCEPTED);
}

static void doRailSetHome(float) {
  uint32_t now = millis();
  // Refused, but it does not claim the flag: redefining zero is instantaneous,
  // so there is nothing to wait for afterwards. Busy and "no stepper" are
  // reported apart, because they are the two things an operator watching a rail
  // that will not move most needs to tell apart.
  if (g_railBusy) {
    Comms::emit(TELEM_RAIL_CMD, RAIL_CMD_REFUSED, 0.0f, now);
    return;
  }
  if (!rail.setHome()) {
    Comms::emit(TELEM_RAIL_CMD, RAIL_CMD_UNAVAILABLE, 0.0f, now);
    return;
  }
  Comms::emit(TELEM_RAIL_CMD, RAIL_CMD_SET_HOME, 0.0f, now);
  railReportPos();
}

static void doRailStop(float) {
  uint32_t now = millis();
  // Acted on unconditionally, whatever the firmware currently believes about
  // the rail. An emergency control that consults internal state first is one
  // that fails in exactly the case it exists for.
  rail.stop();
  Comms::emit(TELEM_RAIL_CMD, RAIL_CMD_STOP, 0.0f, now);

  // Then hand the reporting to serviceRail() rather than sampling the position
  // here, and do so whether or not a move was thought to be in flight.
  //
  // stopMove() decelerates -- a few milliseconds at this acceleration -- so the
  // counter read on this line would be a mid-ramp value, and for an open-loop
  // rail whose only position record is this log, publishing a position the rail
  // then travels past is a silent corruption rather than a rounding error.
  // Re-arming makes serviceRail() wait for rest and report the true one.
  //
  // Claiming the flag even when it was already clear also covers the case where
  // the backstop below released it while the hardware was in fact still moving.
  g_railBusy     = true;
  g_railDeadline = now + 500u;          // deceleration is milliseconds
}

static const CmdSpec CMD_TABLE[] = {
  PARAM_U32(REWARD_DURATION1,  0,   1000,  applyRewardDuration1),
  PARAM_U32(REWARD_DURATION2,  0,   1000,  applyRewardDuration2),
  PARAM_U32(REWARD_INTERVAL1,  0,   60000, nullptr),
  PARAM_U32(REWARD_INTERVAL2,  0,   60000, nullptr),
  PARAM_U32(LICK_DEBOUNCE_TIME, 0,  100,   applyLickDebounceTime),
  PARAM_U32(MAG_FIX_DURATION,  0,   60000, applyMagFixDuration),
  PARAM_U32(MAG_GRACE_MS,      0,   60000, applyMagGrace),
  PARAM_U32(BUZ_PULSE_WIDTH,   0,   100,   applyBuzPulseWidth),
  PARAM_F32(SCALE_HIGH_THRESH, -50, 50,    nullptr),
  PARAM_F32(SCALE_LOW_THRESH,  -50, 50,    nullptr),
  // TASK is applied lazily, at the next trial boundary -- see serviceTask().
  // The PARAM ack therefore means "request accepted"; the TASK telemetry line
  // marks the cycle on which the switch actually happened.
  PARAM_U32(TASK,              1,   3,     nullptr),
  PARAM_U32(T1_SPOUT1_ENABLE,  0,   1,     nullptr),
  PARAM_U32(T1_SPOUT2_ENABLE,  0,   1,     nullptr),
  // Automatic rail retraction. See behavior_task.h for what it does and the two
  // things to know before switching it on. The window maximum must not exceed
  // LickRewardTask::RAIL_WIN_CAP, which sizes the buffer holding it.
  PARAM_U32(T1_RAIL_AUTO_ENABLE, 0, 1,     applyT1RailAuto),
  PARAM_F32(T1_RAIL_STEP,      -2,  2,     nullptr),
  PARAM_U32(T1_RAIL_WIN,       1,   100,   nullptr),
  PARAM_U32(T1_RAIL_MIN_POS_PCT, 0, 100,   nullptr),
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
  PARAM_U32(T3_EARLY_LICK_PUNISH, 0, 1,    nullptr),
  PARAM_U32(T3_EARLY_LICK_PAUSE_MS, 0, 10000, nullptr),
  PARAM_U32(T3_CUE_FREQ,       100, 20000, nullptr),
  PARAM_U32(T3_CUE_DUR,        1,   5000,  nullptr),
  PARAM_U32(T3_RESPONSE_MS,    1,   30000, nullptr),
  PARAM_U32(T3_CONSUME_MS,     0,   30000, nullptr),
  PARAM_U32(T3_PUNISH_MS,      0,   30000, nullptr),
  PARAM_U32(T3_ITI_MS,         0,   30000, nullptr),
  // 1 = strict alternation. Zero is excluded: it would force the type to flip
  // on every trial AND on itself, which is just alternation with a worse name.
  PARAM_U32(T3_MAX_REPEAT,     1,   100,   nullptr),
  // Percent chance a trial is type 1; type 2 gets the rest. 50 = unbiased.
  // Capped in practice by T3_MAX_REPEAT -- see behavior_task.h.
  PARAM_U32(T3_ANTI_BIAS_PROB1, 0,  100,   nullptr),
  PARAM_U32(T3_ANTI_BIAS_AUTO_ENABLE, 0, 1, applyT3AntiBiasAuto),
  PARAM_U32(T3_ANTI_BIAS_WIN,   1,   100,   nullptr),
  PARAM_U32(T3_ANTI_BIAS_ACC_THRESH, 0, 100, nullptr),
  // Percent chance an unanswered trial is rescued with water at the correct
  // spout. 0 = off. Keep low; see the warning in behavior_task.h.
  PARAM_U32(T3_TEACH_PROB,     0,   100,   nullptr),
  PARAM_U32(T3_TEACH_INCLUDE_ABORT, 0, 1,   nullptr),
  
  ACTION(TARE, doTare),

  // --- rail ----------------------------------------------------------------
  // Actions, not parameters, and that distinction is the safety property: the
  // rig stores no "distance to move", so there is nothing for a reconnect or a
  // DUMP to replay. No command, no movement.
  //
  // The two limits are per-move deltas, not bounds on absolute travel. 2 mm is
  // 6040 pulses at the current calibration, so the pulse form is a whisker
  // tighter than the millimetre form; near the limit the two are not quite
  // interchangeable.
  ACTION_F32(RAIL_MOVE_MM,     -2,    2,    doRailMoveMm),
  ACTION_I32(RAIL_MOVE_PULSES, -6000, 6000, doRailMovePulses),
  ACTION(RAIL_SET_HOME, doRailSetHome),   // redefine here as zero; nothing moves
  ACTION(RAIL_STOP,     doRailStop),      // the only command that may interrupt
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

  // --- rail --------------------------------------------------------------
  // Not a sensor read: the flag is the firmware's own record of a move in
  // flight. It is surfaced as a level because the task layer has no other way
  // to know, and task 1 must not score a reward against a rail position the
  // rail is currently leaving.
  if (g_railBusy) in.levels |= LV_RAIL_BUSY;

  return in;
}


// ===========================================================================
//  SAFETY -- runs every cycle, outside the state machine
//
//  Nothing a task does can suppress this, and no task can express it. Keep
//  interlocks here, not in tasks.cpp.
// ===========================================================================

static void supervise(const Inputs &in) {
  if ((in.has(EV_WEIGHT_HI) || in.has(EV_WEIGHT_LO)) && magnet.haltAllowed()) magnet.halt();
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

      case ACT_RAIL_STEP:
        // The distance comes from the tunable, not the Action: the arguments
        // are unsigned and this step is normally negative. Same entry point as
        // the operator's Move button, so a retraction is interlocked, backstopped
        // and position-logged identically -- only the disposition differs.
        railStartMove(RailHandler::mmToPulses(T1_RAIL_STEP), T1_RAIL_STEP,
                      RAIL_CMD_AUTO);
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
//  RAIL SERVICING
//
//  Ends a move and reports where the rail actually stopped. Every move and
//  every stop is reported from here and nowhere else, so those two cases share
//  one code path and the stopped case has no separate logic to get wrong.
//
//  The reported position comes from the stepper's pulse counter, which counts
//  what was actually emitted. A move cut short therefore reports where the rail
//  really is, not where it was asked to go, which is exactly what an open-loop
//  position log needs.
//
//  The invariant this maintains: every command that could have changed where
//  the rail is -- an accepted move, a set-home, a stop -- is followed by exactly
//  one RAIL_POS, and a rejected command is followed by none. Two stops during
//  one move are two real operator actions and are both recorded as RAIL_CMD
//  lines, but there was only ever one resting position, so there is one
//  RAIL_POS. So "where was the rail after command X" is answerable from the log
//  alone, without differencing positions to guess whether X did anything.
// ===========================================================================

static void serviceRail(uint32_t now) {
  if (!g_railBusy) return;

  // Rail at rest: the queue is empty and the ramp is idle, which is exactly the
  // condition under which the pulse counter is exact rather than short by the
  // command in flight. So this is the only moment worth reading it.
  if (!rail.isRunning()) {
    g_railBusy = false;
    railReportPos();
    return;
  }

  // Backstop. A move that never reports finishing would otherwise latch the
  // rail busy until the next reboot, leaving it unusable and mute. Better to
  // release the interlock and record that it had to be forced: failing open
  // with a log line beats failing closed in silence. Signed compare, so this
  // still works across the millis() wrap.
  //
  // The position published here may be mid-move, since by definition the
  // hardware never said it had stopped -- which is what RAIL_CMD_TIMEOUT
  // exists to warn a reader about. The interlock is not the only thing
  // standing between this and a corrupted count: RailHandler::movePulses()
  // refuses while the stepper reports itself running, so a move ordered after
  // a spurious timeout is still rejected rather than stacked. And a RAIL_STOP
  // re-arms this function, which is how the true resting position gets logged
  // after a timeout that fired early.
  if ((int32_t)(now - g_railDeadline) >= 0) {
    g_railBusy = false;
    Comms::emit(TELEM_RAIL_CMD, RAIL_CMD_TIMEOUT, 0.0f, now);
    railReportPos();
  }
}

// Rig state that a DUMP should refresh but dumpParams() cannot reach, because
// it is not a parameter. Rail position qualifies twice over: no SET can write
// it, and since the rail reports only when it moves, a panel that connected
// after the last move would otherwise have nothing at all to show.
static void dumpExtraState() {
  railReportPos();
}


// ===========================================================================
//  SETUP / LOOP
// ===========================================================================

void setup() {
  buzzer    = BuzzerHandler(BUZZER_PIN);
  buzzer.setPulseWidth((uint8_t)BUZ_PULSE_WIDTH);
  lick1     = LickHandler(LICK1_PIN);
  lick2     = LickHandler(LICK2_PIN);
  rewarder1 = Rewarder(SPOUT1_PIN, REWARD_DURATION1);
  rewarder2 = Rewarder(SPOUT2_PIN, REWARD_DURATION2);
  sw        = SwitchHandler(SWITCH_PIN);
  magnet    = Magneto(MAGNET_PIN, MAG_FIX_DURATION);
  magnet.setGraceDuration(MAG_GRACE_MS);

  // A failure here needs no separate report: with no stepper attached every
  // rail command answers RAIL_CMD_UNAVAILABLE, which says exactly this and says
  // it at the moment someone tries to use the rail.
  rail.begin(RAIL_STEP_PIN, RAIL_DIR_PIN);

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
  Comms::setDumpHook(dumpExtraState);

  rewardNum1 = 0;
  rewardNum2 = 0;

  // Announce the message set, then the parameters in effect, so the host knows
  // both the schema and the settings from the start of the session.
  Comms::dumpSchema();
  Comms::dumpParams();
  // Startup does not go through the DUMP command, so the hook has to be called
  // by hand here. Both paths matter: this one publishes the position a reboot
  // has just reset to zero, the hook covers a host connecting later.
  dumpExtraState();

  // Start whichever task TASK names. g_activeTask is 0, so this always fires.
  serviceTask(millis());
}

void loop() {
  Comms::service();          // apply any parameter changes the host sent

  serviceRail(millis());     // close out a finished rail move and report it
  serviceTask(millis());     // honour a pending "SET TASK n", if it is safe to

  Inputs in = sense();       // 1. SENSE
  supervise(in);             //    safety interlocks, unconditionally

  uint32_t before  = g_task->transitions();
  uint32_t trialsB = g_task->trial();
  g_actions.clear();
  g_task->step(in, g_actions);                               // 2. DECIDE

  uint8_t t3Prob1;
  if (g_task->takeT3Prob1(t3Prob1))
    Comms::emit(TELEM_T3_PROB1, 1, (float)t3Prob1, in.now);

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
