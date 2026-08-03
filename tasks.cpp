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
extern unsigned long T3_SAMPLE_FREQ1;
extern unsigned long T3_SAMPLE_FREQ2;
extern unsigned long T3_PULSE_MS;
extern unsigned long T3_GAP_MS;
extern unsigned long T3_N_PULSES;
extern unsigned long T3_DELAY_MS;
extern unsigned long T3_CUE_FREQ;
extern unsigned long T3_CUE_DUR;
extern unsigned long T3_RESPONSE_MS;
extern unsigned long T3_CONSUME_MS;
extern unsigned long T3_PUNISH_MS;
extern unsigned long T3_ITI_MS;
extern unsigned long T3_MAX_REPEAT;
extern unsigned long T3_EARLY_LICK_PUNISH;

// The one thing task 3 needs that is not a tunable and not in Inputs: a random
// draw for the trial type. Declared here, defined by whoever is hosting the
// task layer -- esp_random() on the firmware (behavior_task.h), a scripted
// sequence in tools/task_test.cpp. That indirection is what keeps this file
// hardware-free and keeps the trial sequence exactly reproducible under test.
extern uint32_t task_rand32();


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
        // A task-1 trial exists only because water was delivered, so there is
        // no other outcome it could have.
        countTrial(OUTCOME_HIT);
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
        countTrial(OUTCOME_HIT);          // as in task 1: no other outcome exists
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
//  TASK 3 -- two-tone discrimination with a delay, gated by position
// ===========================================================================

const char *DiscriminationTask::stateName(uint8_t s) const {
  switch (s) {
    case T3_IDLE:     return "IDLE";
    case T3_SAMPLE1:  return "SAMPLE1";
    case T3_SAMPLE2:  return "SAMPLE2";
    case T3_DELAY:    return "DELAY";
    case T3_GOCUE:    return "GOCUE";
    case T3_RESPONSE: return "RESPONSE";
    case T3_REWARD:   return "REWARD";
    case T3_PUNISH:   return "PUNISH";
    case T3_ITI:      return "ITI";
    default:          return "?";
  }
}

void DiscriminationTask::reset(uint32_t now) {
  Task::reset(now);
  type_     = 1;
  lastType_ = 0;      // no history, so the first draw is never forced
  runLen_   = 0;
  pending_  = OUTCOME_ABORT;
}

uint8_t DiscriminationTask::sampleState() const {
  return (type_ == 1) ? T3_SAMPLE1 : T3_SAMPLE2;
}

// nPulses pulses with a gap between each pair, and no trailing gap. The same
// arithmetic BuzzerHandler::playTrain() does, so the task's timeout expires at
// the moment the train ends without either side waiting on the other.
uint32_t DiscriminationTask::trainMs() const {
  if (T3_N_PULSES == 0) return 0;
  return T3_N_PULSES * T3_PULSE_MS + (T3_N_PULSES - 1) * T3_GAP_MS;
}

// Even odds, except that T3_MAX_REPEAT identical trials in a row force the
// other type next. Without the cap a fair coin still produces long runs, and an
// animal that has just been rewarded four times on spout 1 learns the wrong
// lesson from the fifth.
void DiscriminationTask::selectType() {
  uint8_t t = (task_rand32() & 1u) ? 2 : 1;

  if (t == lastType_ && runLen_ >= T3_MAX_REPEAT) {
    t = (t == 1) ? 2 : 1;                 // the cap overrides the draw
  }

  if (t == lastType_) {
    runLen_++;
  } else {
    lastType_ = t;
    runLen_   = 1;
  }
  type_ = t;
}

uint8_t DiscriminationTask::onEvent(uint8_t s, const Inputs &in, ActionQueue &out) {
  const bool licked = in.has(EV_LICK1) || in.has(EV_LICK2);

  // Everything up to and including the go cue requires the animal to stay at
  // the port: leaving is an abort. Tested on the LEVEL rather than on
  // EV_SWITCH_OFF for the same reason as task 2 -- an edge seen on a cycle the
  // task was not looking would otherwise leave the trial running with nobody
  // at the port. Checked before anything else so that a lick arriving on the
  // very cycle the animal leaves cannot replay a trial it has walked out of.
  switch (s) {
    case T3_SAMPLE1:
    case T3_SAMPLE2:
    case T3_DELAY:
    case T3_GOCUE:
      if (!in.level(LV_IN_POSITION)) {
        pending_ = OUTCOME_ABORT;
        return T3_ITI;
      }
      break;
    default:
      break;
  }

  switch (s) {
    case T3_IDLE:
      // The draw happens here, not in onEntry, because it decides WHICH state
      // is entered next. (Task 2's selectSide() can live in onEntry because
      // there the side does not change the state.)
      if (in.level(LV_IN_POSITION)) {
        selectType();
        return sampleState();
      }
      return STAY;

    case T3_SAMPLE1:
    case T3_SAMPLE2:
      // Returning s re-enters this state rather than staying in it: the tone
      // restarts and the timer is rearmed. Same trial type, so a resample
      // cannot be used to fish for an easier trial.
      if (licked && T3_EARLY_LICK_PUNISH) return s;
      if (in.has(EV_TIMEOUT)) return T3_DELAY;
      return STAY;

    case T3_DELAY:
      // Replays from the sample, not from the delay: the animal has to hear the
      // tone again, which is the point of the punishment.
      if (licked && T3_EARLY_LICK_PUNISH) return sampleState();
      if (in.has(EV_TIMEOUT)) return T3_GOCUE;
      return STAY;

    case T3_GOCUE:
      // No lick transition. The cue IS the signal to respond, so a lick during
      // its 100 ms is early by a rounding error rather than by strategy; it is
      // logged by the sketch and changes nothing.
      if (in.has(EV_TIMEOUT)) return T3_RESPONSE;
      return STAY;

    case T3_RESPONSE: {
      const uint32_t correct = (type_ == 1) ? EV_LICK1 : EV_LICK2;
      const uint32_t wrong   = (type_ == 1) ? EV_LICK2 : EV_LICK1;

      // Correct first, so that licking both spouts on one cycle is scored as a
      // hit rather than resolved by bit order. Arbitrary but fixed, exactly as
      // in task 1: the alternative is an irreproducible once-a-week bug.
      if (in.has(correct)) {
        out.push(ACT_REWARD, type_, 0);   // 0 = that spout's own REWARD_DURATION
        pending_ = OUTCOME_HIT;
        return T3_REWARD;
      }
      if (in.has(wrong)) {
        pending_ = OUTCOME_INCORRECT;
        return T3_PUNISH;
      }
      // The window closing and the animal walking off are the same thing here:
      // the trial was asked and not answered.
      if (in.has(EV_TIMEOUT) || !in.level(LV_IN_POSITION)) {
        pending_ = OUTCOME_NO_RESPONSE;
        return T3_ITI;
      }
      return STAY;
    }

    case T3_REWARD:
      // Not gated on position: the animal has earned the water and may drink it
      // however it likes.
      if (in.has(EV_TIMEOUT)) return T3_ITI;
      return STAY;

    case T3_PUNISH:
      // Also ungated -- a timeout the animal could end by stepping away would
      // not be a timeout.
      if (in.has(EV_TIMEOUT)) return T3_ITI;
      return STAY;

    case T3_ITI:
      if (in.has(EV_TIMEOUT)) return T3_IDLE;
      return STAY;
  }
  return STAY;
}

void DiscriminationTask::onEntry(uint8_t s, const Inputs &in, ActionQueue &out) {
  (void)in;
  switch (s) {
    case T3_SAMPLE1:
      out.push(ACT_TONE_TRAIN, T3_SAMPLE_FREQ1);
      setTimeout(trainMs());
      break;
    case T3_SAMPLE2:
      out.push(ACT_TONE_TRAIN, T3_SAMPLE_FREQ2);
      setTimeout(trainMs());
      break;
    case T3_DELAY:
      setTimeout(T3_DELAY_MS);
      break;
    case T3_GOCUE:
      out.push(ACT_TONE, T3_CUE_FREQ, T3_CUE_DUR);
      setTimeout(T3_CUE_DUR);
      break;
    case T3_RESPONSE:
      setTimeout(T3_RESPONSE_MS);
      break;
    case T3_REWARD:
      // Measured from the moment the valve is asked to open, not from when it
      // shuts: the consumption period is time at the spout, and REWARD_DURATIONn
      // is a fraction of it.
      setTimeout(T3_CONSUME_MS);
      break;
    case T3_PUNISH:
      setTimeout(T3_PUNISH_MS);
      break;
    case T3_ITI:
      // The single place a task-3 trial is booked. Every route out of a trial
      // passes through here, which is what makes the four outcome counts add up
      // to trial() by construction rather than by inspection.
      countTrial(pending_);
      setTimeout(T3_ITI_MS);
      break;
    default:
      break;
  }
}


// ===========================================================================
//  REGISTRATION
// ===========================================================================

static LickRewardTask     g_task1;
static CuedRewardTask     g_task2;
static DiscriminationTask g_task3;

static const TaskSpec TASK_TABLE[] = {
  { 1, &g_task1 },
  { 2, &g_task2 },
  { 3, &g_task3 },
};
static const uint8_t TASK_TABLE_N = sizeof(TASK_TABLE) / sizeof(TASK_TABLE[0]);

Task *taskById(uint32_t id) {
  for (uint8_t i = 0; i < TASK_TABLE_N; i++)
    if (TASK_TABLE[i].id == id) return TASK_TABLE[i].task;
  return nullptr;
}

uint8_t         taskCount() { return TASK_TABLE_N; }
const TaskSpec *taskTable() { return TASK_TABLE; }
