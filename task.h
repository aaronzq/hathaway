#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// task.h -- TASK MECHANISM ONLY.
//
// Same split as protocol.h / comms.cpp: this header defines *how* a task is
// structured and knows nothing about spouts, tones, scales or magnets. The
// tasks themselves live in tasks.h/tasks.cpp; the hardware wiring lives in
// hathaway.ino.
//
// The control loop runs three phases, in this order, every cycle:
//
//   1. SENSE   read every sensor once into an Inputs snapshot
//   2. DECIDE  Task::step() -- pure logic: no hardware, no Serial, no millis()
//   3. ACT     drain the ActionQueue into the controller objects
//
// Because phase 2 touches no hardware at all, the whole task layer compiles
// and runs on a PC and can be driven with synthetic event traces
// (see tools/task_test.cpp). That is the point of the split.
//
// To add a task: add a class to tasks.h/tasks.cpp and a row to TASK_TABLE.
// Nothing in this file needs to change.
// ---------------------------------------------------------------------------


// ============================== EDGE EVENTS ================================
//
// One-shot. Set by the sketch for exactly ONE cycle, at the moment the edge is
// detected. A task that does not consume an event on the cycle it appears will
// never see it again -- that is deliberate, and it is why "flags" are split in
// two here. Anything you need to test repeatedly belongs in the level bitmask
// below, not in this one.

enum : uint32_t {
  EV_LICK1      = 1u << 0,
  EV_LICK2      = 1u << 1,
  EV_SWITCH_ON  = 1u << 2,   // mouse arrived in position
  EV_SWITCH_OFF = 1u << 3,   // mouse left
  EV_TONE_DONE  = 1u << 4,   // buzzer finished the note it was playing
  EV_WEIGHT_HI  = 1u << 5,   // load cell crossed SCALE_HIGH_THRESH
  EV_WEIGHT_LO  = 1u << 6,   // load cell crossed SCALE_LOW_THRESH
  EV_TIMEOUT    = 1u << 7,   // synthesised by step(); see setTimeout()
};


// ============================== LEVEL STATES ===============================
//
// Recomputed from scratch every cycle, so they may be tested any number of
// times and in any state without being "used up".

enum : uint32_t {
  LV_IN_POSITION = 1u << 0,
  LV_TONE_ON     = 1u << 1,
  LV_SPOUT1_EN   = 1u << 2,   // SPOUT1_ENABLE parameter
  LV_SPOUT2_EN   = 1u << 3,   // SPOUT2_ENABLE parameter
};


// One cycle's worth of the world. `now` is sampled ONCE at the top of the
// cycle, so every comparison a task makes within a cycle sees the same instant.
struct Inputs {
  uint32_t now     = 0;
  uint32_t events  = 0;
  uint32_t levels  = 0;
  uint32_t t_lick1 = 0;    // capture time of EV_LICK1, if set this cycle
  uint32_t t_lick2 = 0;
  float    weight  = 0.0f;

  bool has(uint32_t ev)   const { return (events & ev) != 0; }
  bool level(uint32_t lv) const { return (levels & lv) != 0; }
};


// ================================ ACTIONS ==================================
//
// What a task asks the hardware to do. The task never calls a controller
// directly -- it pushes a request and the sketch executes it in phase 3. That
// indirection is what keeps the task layer host-testable, and it also gives
// every decision a single place to be logged from.

enum : uint8_t {
  ACT_REWARD,   // a0 = spout (1 or 2), a1 = open ms (0 = that spout's default)
  ACT_TONE,     // a0 = frequency Hz,   a1 = duration ms
};

struct Action {
  uint8_t  verb;
  uint32_t a0, a1;
};

// Fixed capacity, no heap: this lives for the whole session and must not
// fragment. A task that overflows the queue in a single cycle is doing too
// much; push() reports the failure instead of corrupting the queue silently.
class ActionQueue {
public:
  static const uint8_t CAP = 8;

  bool push(uint8_t verb, uint32_t a0 = 0, uint32_t a1 = 0) {
    if (n_ >= CAP) { overflow_++; return false; }
    q_[n_].verb = verb;
    q_[n_].a0   = a0;
    q_[n_].a1   = a1;
    n_++;
    return true;
  }

  uint8_t       size() const            { return n_; }
  const Action &at(uint8_t i) const     { return q_[i]; }
  void          clear()                 { n_ = 0; }
  uint32_t      overflow() const        { return overflow_; }   // health metric

private:
  Action   q_[CAP];
  uint8_t  n_        = 0;
  uint32_t overflow_ = 0;
};


// ============================== THE BASE CLASS =============================

class Task {
public:
  // Returned from onEvent() to remain in the current state.
  //
  // Returning the CURRENT state id instead is not the same thing: it re-enters
  // the state (onExit then onEntry, timer reset, transition logged), which is
  // how a "replay this trial" transition will be written later.
  static const uint8_t STAY = 0xFF;

  virtual ~Task() {}

  // ---- driven by the sketch -------------------------------------------
  void step(const Inputs &in, ActionQueue &out);
  void reset(uint32_t now);     // safe (re)entry; call once on task switch

  // ---- how the task describes itself to the host ----------------------
  virtual const char *name() const = 0;
  virtual uint8_t     stateCount() const = 0;
  virtual const char *stateName(uint8_t s) const = 0;

  // True only when the task is at a trial boundary. The sketch defers a
  // requested task switch until this returns true, so a switch can never land
  // in the middle of a trial.
  virtual bool safeToSwitch() const { return true; }

  uint8_t  state()       const { return state_; }
  uint32_t trial()       const { return trial_; }
  uint32_t transitions() const { return transitions_; }

protected:
  // The only method a task MUST implement. Return STAY or the next state id.
  virtual uint8_t onEvent(uint8_t state, const Inputs &in, ActionQueue &out) = 0;
  virtual void    onEntry(uint8_t state, const Inputs &in, ActionQueue &out) {}
  virtual void    onExit (uint8_t state, const Inputs &in, ActionQueue &out) {}

  // Arm EV_TIMEOUT for `ms` from now. Cancelled automatically on every
  // transition, so one state's timer can never leak into the next state --
  // which is the usual way hand-written state machines go wrong.
  void setTimeout(uint32_t ms) { timeoutAt_ = now_ + ms; timeoutArmed_ = true; }
  void clearTimeout()          { timeoutArmed_ = false; }

  // Declarative guard: while in this state, any event in `mask` transitions
  // straight to `target` without onEvent() being consulted. Set it in
  // onEntry(); it is cleared on every transition. Written for "any lick during
  // the sample or delay epoch replays the trial".
  void abortOn(uint32_t mask, uint8_t target) {
    abortMask_   = mask;
    abortTarget_ = target;
  }

  uint32_t stateElapsed() const { return now_ - enteredMs_; }
  uint32_t now()          const { return now_; }
  void     countTrial()         { trial_++; }
  void     setInitialState(uint8_t s) { state_ = s; }

private:
  void enter(uint8_t s, const Inputs &in, ActionQueue &out);

  uint8_t  state_        = 0;
  uint32_t now_          = 0;
  uint32_t enteredMs_    = 0;
  uint32_t timeoutAt_    = 0;
  bool     timeoutArmed_ = false;
  uint32_t abortMask_    = 0;
  uint8_t  abortTarget_  = STAY;
  uint32_t trial_        = 0;
  uint32_t transitions_  = 0;
  bool     entered_      = false;
};
