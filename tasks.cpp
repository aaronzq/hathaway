#include "tasks.h"

// ---------------------------------------------------------------------------
// Tunables read by the tasks. Declared extern, not included from
// behavior_task.h, because that header DEFINES them (see the note in tasks.h).
// The host sets them live with "SET <NAME> <VALUE>"; the ranges and acks are
// handled by CMD_TABLE in hathaway.ino.
//
// tools/task_test.cpp defines these itself, which is how the tasks can be run
// on a PC without any of the firmware.
// ---------------------------------------------------------------------------
extern unsigned long REWARD_INTERVAL1;
extern unsigned long REWARD_INTERVAL2;
extern unsigned long T2_CUE_FREQ;
extern unsigned long T2_CUE_DUR;
extern unsigned long T2_CUE_TO_WATER;
extern unsigned long T2_N1;
extern unsigned long T2_N2;


// ===========================================================================
//  TASK 1 -- free licking for water
// ===========================================================================

const char *LickRewardTask::stateName(uint8_t s) const {
  switch (s) {
    case T1_ARMED:      return "ARMED";
    case T1_REFRACTORY: return "REFRACTORY";
    default:            return "?";
  }
}

uint8_t LickRewardTask::onEvent(uint8_t s, const Inputs &in, ActionQueue &out) {
  switch (s) {
    case T1_ARMED:
      // Spout 1 wins if both licks debounce on the same cycle. Arbitrary, but
      // fixed: the alternative is a nondeterministic bug that shows up once a
      // week and cannot be reproduced.
      //
      if (in.has(EV_LICK1) && in.level(LV_SPOUT1_EN)) {
        out.push(ACT_REWARD, 1, 0);      // 0 = spout 1's own REWARD_DURATION1
        interval_ = REWARD_INTERVAL1;
        return T1_REFRACTORY;
      }
      if (in.has(EV_LICK2) && in.level(LV_SPOUT2_EN)) {
        out.push(ACT_REWARD, 2, 0);
        interval_ = REWARD_INTERVAL2;
        return T1_REFRACTORY;
      }
      return STAY;

    case T1_REFRACTORY:
      // Licks here are logged by the sketch but earn nothing: the state simply
      // has no lick transition. That is the whole implementation of "the gate
      // is closed".
      if (in.has(EV_TIMEOUT)) {
        countTrial();
        return T1_ARMED;
      }
      return STAY;
  }
  return STAY;
}

void LickRewardTask::onEntry(uint8_t s, const Inputs &in, ActionQueue &out) {
  (void)in; (void)out;
  if (s == T1_REFRACTORY) setTimeout(interval_);
}


// ===========================================================================
//  TASK 2 -- cued reward at one spout at a time, gated by position
// ===========================================================================

const char *CuedRewardTask::stateName(uint8_t s) const {
  switch (s) {
    case T2_IDLE:       return "IDLE";
    case T2_CUE:        return "CUE";
    case T2_WAIT_LICK:  return "WAIT_LICK";
    case T2_REFRACTORY: return "REFRACTORY";
    default:            return "?";
  }
}

void CuedRewardTask::reset(uint32_t now) {
  Task::reset(now);
  side_ = 1;                 // every session starts at the top of a spout-1 block
  done_ = 0;
}

// Which spout this trial belongs to, resolved once per cue so that a T2_Nn
// changed mid-session takes effect at the next trial and not mid-trial.
//
// A block length of zero retires that spout, so the cases below are: nothing
// left to run, the current spout has just been retired, and coming back from
// having had nothing to run.
void CuedRewardTask::selectSide() {
  if (T2_N1 == 0 && T2_N2 == 0)      { side_ = 0; done_ = 0; return; }
  if (side_ == 1 && T2_N1 == 0)      { side_ = 2; done_ = 0; return; }
  if (side_ == 2 && T2_N2 == 0)      { side_ = 1; done_ = 0; return; }
  if (side_ == 0)                    { side_ = (T2_N1 > 0) ? 1 : 2; done_ = 0; }
}

// Counted at the moment water is delivered, not at the end of the gate, so a
// mouse that walks off during the refractory still gets credit for the reward
// it earned. ">=" rather than "==" so shrinking T2_Nn mid-block ends the block
// at the next reward instead of running away. A flip onto a retired spout is
// corrected by selectSide() at the next cue.
void CuedRewardTask::advanceBlock() {
  done_++;
  const uint32_t n = (side_ == 1) ? T2_N1 : T2_N2;
  if (done_ >= n) {
    side_ = (side_ == 1) ? 2 : 1;
    done_ = 0;
  }
}

uint8_t CuedRewardTask::onEvent(uint8_t s, const Inputs &in, ActionQueue &out) {
  // Leaving position aborts whatever is running, from every state except IDLE.
  // Tested on the LEVEL, not on EV_SWITCH_OFF, so the task cannot be left armed
  // by an edge that arrived on a cycle it was not looking.
  if (s != T2_IDLE && !in.level(LV_IN_POSITION)) return T2_IDLE;

  switch (s) {
    case T2_IDLE:
      if (in.level(LV_IN_POSITION)) return T2_CUE;
      return STAY;

    case T2_CUE:
      // No lick transition: licks between cue onset and T2_CUE_TO_WATER are
      // recorded and earn nothing.
      if (in.has(EV_TIMEOUT)) return T2_WAIT_LICK;
      return STAY;

    case T2_WAIT_LICK:
      // Only this block's spout is live. The other spout's licks are logged by
      // the sketch and ignored here, which is the whole of the alternation gate.
      // side_ == 0 means both spouts are retired, so nothing is live and the
      // task waits here for as long as the mouse stays at the port.
      if (side_ != 0 && in.has(side_ == 1 ? EV_LICK1 : EV_LICK2)) {
        out.push(ACT_REWARD, side_, 0);   // 0 = that spout's own REWARD_DURATION
        interval_ = (side_ == 1) ? REWARD_INTERVAL1 : REWARD_INTERVAL2;
        countTrial();
        advanceBlock();                   // may flip side_, so read it above
        return T2_REFRACTORY;
      }
      return STAY;

    case T2_REFRACTORY:
      // Still in position -- checked at the top of this function -- so the next
      // cue follows the gate with nothing else to satisfy.
      if (in.has(EV_TIMEOUT)) return T2_CUE;
      return STAY;
  }
  return STAY;
}

void CuedRewardTask::onEntry(uint8_t s, const Inputs &in, ActionQueue &out) {
  (void)in;
  switch (s) {
    case T2_CUE:
      selectSide();                       // the block this cue belongs to
      out.push(ACT_TONE, T2_CUE_FREQ, T2_CUE_DUR);
      setTimeout(T2_CUE_TO_WATER);
      break;
    case T2_REFRACTORY:
      setTimeout(interval_);
      break;
    default:
      break;
  }
}


// ===========================================================================
//  REGISTRATION
// ===========================================================================

static LickRewardTask g_task1;
static CuedRewardTask g_task2;

static const TaskSpec TASK_TABLE[] = {
  { 1, &g_task1 },
  { 2, &g_task2 },
};
static const uint8_t TASK_TABLE_N = sizeof(TASK_TABLE) / sizeof(TASK_TABLE[0]);

Task *taskById(uint32_t id) {
  for (uint8_t i = 0; i < TASK_TABLE_N; i++)
    if (TASK_TABLE[i].id == id) return TASK_TABLE[i].task;
  return nullptr;
}

uint8_t         taskCount() { return TASK_TABLE_N; }
const TaskSpec *taskTable() { return TASK_TABLE; }
