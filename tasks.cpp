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
//  TASK 2 -- cued reward, one reward per trial
// ===========================================================================

const char *CuedRewardTask::stateName(uint8_t s) const {
  switch (s) {
    case T2_WAIT_LICK:  return "WAIT_LICK";
    case T2_CUE:        return "CUE";
    case T2_REFRACTORY: return "REFRACTORY";
    default:            return "?";
  }
}

uint8_t CuedRewardTask::onEvent(uint8_t s, const Inputs &in, ActionQueue &out) {
  switch (s) {
    case T2_WAIT_LICK:
      if (in.has(EV_LICK1) && in.level(LV_SPOUT1_EN)) { side_ = 1; return T2_CUE; }
      if (in.has(EV_LICK2) && in.level(LV_SPOUT2_EN)) { side_ = 2; return T2_CUE; }
      return STAY;

    case T2_CUE:
      // No lick transition: every lick from here to the end of the trial is
      // recorded and ignored. "Only the first lick counts."
      if (in.has(EV_TIMEOUT)) {
        out.push(ACT_REWARD, side_, 0);
        return T2_REFRACTORY;
      }
      return STAY;

    case T2_REFRACTORY:
      if (in.has(EV_TIMEOUT)) {
        countTrial();
        return T2_WAIT_LICK;
      }
      return STAY;
  }
  return STAY;
}

void CuedRewardTask::onEntry(uint8_t s, const Inputs &in, ActionQueue &out) {
  (void)in;
  switch (s) {
    case T2_CUE:
      out.push(ACT_TONE, T2_CUE_FREQ, T2_CUE_DUR);
      setTimeout(T2_CUE_TO_WATER);
      break;
    case T2_REFRACTORY:
      setTimeout(side_ == 1 ? REWARD_INTERVAL1 : REWARD_INTERVAL2);
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
