#include "task.h"

// ---------------------------------------------------------------------------
// The whole transition mechanism lives here, in one place, so that every
// transition is guaranteed to:
//
//   * run onExit on the state being left, then onEntry on the state entered
//   * reset the state timer and clear any armed timeout
//   * clear the abort mask
//   * bump the transition counter, which is how the sketch knows to log a
//     STATE line (a counter rather than a state comparison, so that re-entering
//     the same state is still recorded)
//
// If a task were allowed to assign the state directly, all of the above would
// become the task author's problem, and would be forgotten exactly once.
// ---------------------------------------------------------------------------

void Task::reset(uint32_t now) {
  now_          = now;
  state_        = 0;
  enteredMs_    = now;
  timeoutArmed_ = false;
  abortMask_    = 0;
  abortTarget_  = STAY;
  trial_        = 0;
  lastOutcome_  = OUTCOME_HIT;
  for (uint8_t i = 0; i < OUTCOME_COUNT; i++) outcomes_[i] = 0;
  entered_      = false;   // onEntry(initial state) runs on the next step()
}

void Task::step(const Inputs &in, ActionQueue &out) {
  now_ = in.now;

  // First cycle after reset: enter the initial state properly, so a task never
  // sees onEvent for a state it was never entered into.
  if (!entered_) {
    entered_ = true;
    enter(state_, in, out);
  }

  // Synthesise EV_TIMEOUT locally rather than making the sketch track timers.
  // The subtraction is done signed so it stays correct across the millis()
  // rollover at ~49.7 days.
  Inputs ev = in;
  if (timeoutArmed_ && (int32_t)(in.now - timeoutAt_) >= 0) {
    timeoutArmed_ = false;
    ev.events |= EV_TIMEOUT;
  }

  uint8_t next;
  if (abortMask_ != 0 && (ev.events & abortMask_) != 0) {
    next = abortTarget_;            // declarative guard wins over onEvent
  } else {
    next = onEvent(state_, ev, out);
  }

  if (next != STAY) {
    onExit(state_, ev, out);
    enter(next, ev, out);
  }
}

void Task::enter(uint8_t s, const Inputs &in, ActionQueue &out) {
  state_        = s;
  enteredMs_    = in.now;
  timeoutArmed_ = false;
  abortMask_    = 0;
  abortTarget_  = STAY;
  transitions_++;
  onEntry(s, in, out);
}
