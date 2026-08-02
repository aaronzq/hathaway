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
//  A spout disabled with "SET T1_SPOUT2_ENABLE 0" still has its licks detected and
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
//  TASK 2 -- cued reward at one spout at a time, gated by position
//
//  The whole task is gated by the position switch. Out of position, nothing
//  happens. In position, the machine cues, waits for a lick, waters, waits out
//  the gate, and cues again -- indefinitely, until the mouse leaves.
//
//  A trial is: go cue (T2_CUE_FREQ / T2_CUE_DUR) -> T2_CUE_TO_WATER, during
//  which licks are recorded but earn nothing -> the next lick on the active
//  spout delivers water -> that spout's REWARD_INTERVAL -> cue again. There is
//  no response deadline: the machine waits for the lick as long as the mouse
//  stays in position.
//
//  ALTERNATION. Only one spout is active at a time: spout 1 for T2_N1 rewards,
//  then spout 2 for T2_N2 rewards, repeating. The inactive spout's licks are
//  logged and ignored. This is a rule of the task, not a setting, so it has no
//  on/off switch -- only the block lengths are exposed.
//
//  A block length of zero retires that spout: T2_N1 = 0 puts every trial on
//  spout 2. This, and not T1_SPOUTn_ENABLE, is how task 2 takes a spout out of
//  use -- those two parameters belong to task 1 and this task never reads them.
//  With both lengths zero no spout is live, and the machine does what it does
//  in every other dead end: cues once when the mouse arrives, then waits.
//
//  LEAVING POSITION aborts the trial in progress from any state and releases
//  the gate: come straight back and a new cue plays at once. What survives is
//  the block counter, so a mouse that fidgets in and out still works through
//  its T2_N1 rewards on spout 1 rather than restarting the block every time.
//
//      IDLE       --in position-->                 CUE  (play the go cue)
//      CUE        --T2_CUE_TO_WATER elapsed-->      WAIT_LICK
//      WAIT_LICK  --lick on the active spout-->     REFRACTORY  (water, trial++)
//      REFRACTORY --REWARD_INTERVALn elapsed-->     CUE
//      any state  --out of position-->              IDLE
// ===========================================================================

enum : uint8_t {
  T2_IDLE = 0,
  T2_CUE,
  T2_WAIT_LICK,
  T2_REFRACTORY,
  T2_STATE_COUNT,
};

class CuedRewardTask : public Task {
public:
  const char *name() const override { return "CUED_REWARD"; }
  uint8_t     stateCount() const override { return T2_STATE_COUNT; }
  const char *stateName(uint8_t s) const override;
  bool        safeToSwitch() const override { return state() == T2_IDLE; }
  void        reset(uint32_t now) override;   // also restarts the block

protected:
  uint8_t onEvent(uint8_t s, const Inputs &in, ActionQueue &out) override;
  void    onEntry(uint8_t s, const Inputs &in, ActionQueue &out) override;

private:
  void selectSide();                    // resolve this trial's spout, once per cue
  void advanceBlock();                  // count a reward, alternate when due

  uint8_t  side_     = 1;   // spout the current block is running (1, 2, 0 = none)
  uint32_t done_     = 0;   // rewards delivered so far in this block
  uint32_t interval_ = 0;   // gate length chosen by the spout that rewarded
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
