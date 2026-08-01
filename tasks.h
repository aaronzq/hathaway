#pragma once
#include "task.h"

// ---------------------------------------------------------------------------
// tasks.h -- the actual behaviour. One class per training stage.
//
// Each class is a state machine and nothing else: it reads the Inputs snapshot,
// pushes Actions, and returns the next state. It never touches a pin, a
// controller object, Serial or millis(). That restriction is what lets
// tools/task_test.cpp drive these classes on a PC.
//
// Tunable numbers are the mutable globals defined in behavior_task.h and
// registered in CMD_TABLE (hathaway.ino), so they are settable live over serial
// with "SET <NAME> <VALUE>". tasks.cpp declares the ones it reads as extern
// rather than including behavior_task.h -- that header *defines* the globals,
// so a second translation unit including it would fail to link. Same reason
// comms.cpp does not include it.
// ---------------------------------------------------------------------------


// ===========================================================================
//  TASK 1 -- free licking for water
//
//  Either spout delivers its own water when licked. Both spouts share ONE
//  refractory gate, so a reward at either spout gates the other, and the
//  interval used is that of the spout that just rewarded. This reproduces the
//  behaviour of the pre-state-machine firmware exactly.
//
//  A spout disabled with "SET SPOUT2_ENABLE 0" still has its licks detected and
//  logged -- it just cannot earn water, and its licks do not touch the gate.
//
//      ARMED  --lick on an enabled spout--> REFRACTORY  (deliver that spout)
//      REFRACTORY  --REWARD_INTERVALn elapsed-->  ARMED
// ===========================================================================

enum : uint8_t {
  T1_ARMED = 0,
  T1_REFRACTORY,
  T1_STATE_COUNT,
};

class LickRewardTask : public Task {
public:
  const char *name() const override { return "LICK_REWARD"; }
  uint8_t     stateCount() const override { return T1_STATE_COUNT; }
  const char *stateName(uint8_t s) const override;
  bool        safeToSwitch() const override { return state() == T1_ARMED; }

protected:
  uint8_t onEvent(uint8_t s, const Inputs &in, ActionQueue &out) override;
  void    onEntry(uint8_t s, const Inputs &in, ActionQueue &out) override;

private:
  uint32_t interval_ = 0;   // gate length chosen by the spout that rewarded
};


// ===========================================================================
//  TASK 2 -- cued reward, one reward per trial
//
//  The mouse's first lick opens a trial: a go cue plays (T2_CUE_FREQ /
//  T2_CUE_DUR, default 6 kHz for 100 ms), water is delivered at the spout that
//  was licked, and the gate is closed for that spout's REWARD_INTERVAL before
//  the next trial opens. Licks after the first are ignored for the rest of the
//  trial, which is what "multiple licks trigger only one reward" means.
//
//  T2_CUE_TO_WATER sets when water arrives relative to cue ONSET. The default
//  of 100 ms equals T2_CUE_DUR, so water lands at cue offset; set it to 0 to
//  deliver at cue onset instead (one loop cycle later, well under 1 ms now that
//  the grating is out of the loop).
//
//      WAIT_LICK  --lick on an enabled spout-->  CUE   (play the go cue)
//      CUE        --T2_CUE_TO_WATER elapsed-->   REFRACTORY  (deliver water)
//      REFRACTORY --REWARD_INTERVALn elapsed-->  WAIT_LICK   (trial++)
// ===========================================================================

enum : uint8_t {
  T2_WAIT_LICK = 0,
  T2_CUE,
  T2_REFRACTORY,
  T2_STATE_COUNT,
};

class CuedRewardTask : public Task {
public:
  const char *name() const override { return "CUED_REWARD"; }
  uint8_t     stateCount() const override { return T2_STATE_COUNT; }
  const char *stateName(uint8_t s) const override;
  bool        safeToSwitch() const override { return state() == T2_WAIT_LICK; }

protected:
  uint8_t onEvent(uint8_t s, const Inputs &in, ActionQueue &out) override;
  void    onEntry(uint8_t s, const Inputs &in, ActionQueue &out) override;

private:
  uint8_t side_ = 0;        // spout that opened the current trial (1 or 2)
};


// ===========================================================================
//  REGISTRATION
//
//  One row per task. The id is what "SET TASK <id>" selects; the objects have
//  static storage duration so nothing is ever allocated at run time.
// ===========================================================================

struct TaskSpec {
  uint8_t  id;
  Task    *task;
};

// Look up a task by its TASK id. Returns nullptr for an unknown id.
Task       *taskById(uint32_t id);
// Number of registered tasks, for the startup announcement.
uint8_t     taskCount();
const TaskSpec *taskTable();
