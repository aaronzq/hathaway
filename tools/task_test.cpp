// ---------------------------------------------------------------------------
// task_test.cpp -- drive the task state machines on a PC.
//
//   cd tools && ./run_tasks.sh
//
// Requires g++ only: no ESP32, no upload, no animal. The task layer never calls
// millis(), touches a pin or writes to Serial, so it can be stepped through a
// scripted timeline here and its states and actions asserted exactly.
//
// Add a case whenever you add or change a task. A failing trace here is a bug
// you found in a second instead of at the rig.
// ---------------------------------------------------------------------------
#include "../task.h"
#include "../tasks.h"

#include <cstdio>
#include <string>
#include <vector>

// The tunables the tasks read. On the firmware these live in behavior_task.h
// and are registered in CMD_TABLE; here the test owns them so it can vary them.
unsigned long REWARD_INTERVAL1 = 3000;
unsigned long REWARD_INTERVAL2 = 2000;
unsigned long T2_CUE_FREQ      = 6000;
unsigned long T2_CUE_DUR       = 100;
unsigned long T2_CUE_TO_WATER  = 100;

static int g_failures = 0;

static void check(bool ok, const std::string &what) {
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) g_failures++;
}

// ---------------------------------------------------------------------------
// A tiny harness: holds the clock, drives one cycle at a time, and records the
// state names entered and the actions emitted so a whole trial can be asserted
// as one string.
// ---------------------------------------------------------------------------
class Harness {
public:
  explicit Harness(Task *t, uint32_t t0 = 1000) : task_(t), now_(t0) {
    task_->reset(now_);
  }

  // Run one cycle with the given events, then clear the log-visible trace.
  void cycle(uint32_t events = 0, uint32_t levels = LV_SPOUT1_EN | LV_SPOUT2_EN) {
    Inputs in;
    in.now    = now_;
    in.events = events;
    in.levels = levels;

    uint32_t before = task_->transitions();
    q_.clear();
    task_->step(in, q_);

    if (task_->transitions() != before) {
      trace_ += "[";
      trace_ += task_->stateName(task_->state());
      trace_ += "]";
    }
    for (uint8_t i = 0; i < q_.size(); i++) {
      const Action &a = q_.at(i);
      char buf[64];
      if (a.verb == ACT_REWARD)    snprintf(buf, sizeof(buf), "REWARD(%u)", (unsigned)a.a0);
      else if (a.verb == ACT_TONE) snprintf(buf, sizeof(buf), "TONE(%u,%u)",
                                            (unsigned)a.a0, (unsigned)a.a1);
      else                         snprintf(buf, sizeof(buf), "?%u", a.verb);
      trace_ += buf;
    }
  }

  // Advance the clock, running one cycle per step so timeouts are noticed at a
  // realistic granularity rather than being jumped over.
  void advance(uint32_t ms, uint32_t step = 1) {
    for (uint32_t i = 0; i < ms; i += step) { now_ += step; cycle(); }
  }

  const std::string &trace() const { return trace_; }
  void  clearTrace()               { trace_.clear(); }
  Task *task() const               { return task_; }
  uint32_t now() const             { return now_; }
  void     bump(uint32_t ms)       { now_ += ms; }

private:
  Task        *task_;
  uint32_t     now_;
  ActionQueue  q_;
  std::string  trace_;
};


// ===========================================================================
//  TASK 1 -- free licking for water
// ===========================================================================

static void test_task1_basic() {
  printf("task 1: lick -> reward -> shared gate -> re-arm\n");
  Harness h(taskById(1));

  h.cycle();                                    // enters ARMED
  check(h.trace() == "[ARMED]", "starts in ARMED");
  h.clearTrace();

  h.cycle(EV_LICK1);
  check(h.trace() == "[REFRACTORY]REWARD(1)",
        "lick on spout 1 rewards spout 1 and closes the gate");
  h.clearTrace();

  // Still inside REWARD_INTERVAL1: further licks earn nothing.
  h.advance(500);
  h.cycle(EV_LICK1);
  h.cycle(EV_LICK2);
  check(h.trace() == "", "licks during the gate earn nothing, from either spout");
  h.clearTrace();

  h.advance(3000);
  check(h.trace() == "[ARMED]", "re-arms after REWARD_INTERVAL1");
  check(h.task()->trial() == 1, "one completed trial counted");
}

static void test_task1_shared_gate_uses_that_spouts_interval() {
  printf("task 1: the gate length is the rewarding spout's own interval\n");
  Harness h(taskById(1));
  h.cycle();
  h.clearTrace();

  h.cycle(EV_LICK2);                            // REWARD_INTERVAL2 = 2000
  check(h.trace() == "[REFRACTORY]REWARD(2)", "spout 2 rewards spout 2");
  h.clearTrace();

  h.advance(1999);
  check(h.trace() == "", "still gated at 1999 ms");
  h.advance(2);
  check(h.trace() == "[ARMED]", "re-armed by 2001 ms, not spout 1's 3000");
}

static void test_task1_disabled_spout() {
  printf("task 1: a disabled spout cannot earn water or touch the gate\n");
  Harness h(taskById(1));
  h.cycle(0, LV_SPOUT1_EN);                     // spout 2 disabled
  h.clearTrace();

  h.cycle(EV_LICK2, LV_SPOUT1_EN);
  check(h.trace() == "", "lick on the disabled spout does nothing");

  h.cycle(EV_LICK1, LV_SPOUT1_EN);
  check(h.trace() == "[REFRACTORY]REWARD(1)",
        "the gate was still open: the enabled spout still works");
}

static void test_task1_simultaneous_licks() {
  printf("task 1: both licks on one cycle resolve deterministically\n");
  Harness h(taskById(1));
  h.cycle();
  h.clearTrace();

  h.cycle(EV_LICK1 | EV_LICK2);
  check(h.trace() == "[REFRACTORY]REWARD(1)", "spout 1 wins, and only one reward");
}


// ===========================================================================
//  TASK 2 -- cued reward, one reward per trial
// ===========================================================================

static void test_task2_full_trial() {
  printf("task 2: first lick -> cue -> water -> gate -> next trial\n");
  Harness h(taskById(2));

  h.cycle();
  check(h.trace() == "[WAIT_LICK]", "starts in WAIT_LICK");
  h.clearTrace();

  h.cycle(EV_LICK2);
  check(h.trace() == "[CUE]TONE(6000,100)", "lick opens the trial and plays the go cue");
  h.clearTrace();

  h.advance(99);
  check(h.trace() == "", "no water before T2_CUE_TO_WATER");
  h.advance(2);
  check(h.trace() == "[REFRACTORY]REWARD(2)",
        "water at cue offset, on the spout that was licked");
  h.clearTrace();

  h.advance(2000);                              // REWARD_INTERVAL2
  check(h.trace() == "[WAIT_LICK]", "next trial opens after the spout's interval");
  check(h.task()->trial() == 1, "one completed trial counted");
}

static void test_task2_only_first_lick_counts() {
  printf("task 2: extra licks within a trial are ignored\n");
  Harness h(taskById(2));
  h.cycle();
  h.clearTrace();

  h.cycle(EV_LICK1);
  h.clearTrace();

  // Lick storm through the cue and the whole gate: exactly one reward total.
  for (int i = 0; i < 40; i++) { h.bump(50); h.cycle(EV_LICK1 | EV_LICK2); }

  size_t rewards = 0;
  const std::string &t = h.trace();
  for (size_t p = t.find("REWARD"); p != std::string::npos; p = t.find("REWARD", p + 1))
    rewards++;
  check(rewards == 1, "exactly one reward despite 40 cycles of licking");
  check(t.find("REWARD(1)") != std::string::npos, "and it went to the licked spout");
}

static void test_task2_cue_at_onset() {
  printf("task 2: T2_CUE_TO_WATER = 0 delivers at cue onset\n");
  unsigned long saved = T2_CUE_TO_WATER;
  T2_CUE_TO_WATER = 0;

  Harness h(taskById(2));
  h.cycle();
  h.clearTrace();

  h.cycle(EV_LICK1);
  check(h.trace() == "[CUE]TONE(6000,100)", "cue still plays for its full 100 ms");
  h.clearTrace();

  h.advance(1);
  check(h.trace() == "[REFRACTORY]REWARD(1)", "water on the very next cycle");

  T2_CUE_TO_WATER = saved;
}

static void test_task2_disabled_spout() {
  printf("task 2: a disabled spout cannot open a trial\n");
  Harness h(taskById(2));
  h.cycle(0, LV_SPOUT2_EN);                     // spout 1 disabled
  h.clearTrace();

  h.cycle(EV_LICK1, LV_SPOUT2_EN);
  check(h.trace() == "", "lick on the disabled spout does not start a trial");

  h.cycle(EV_LICK2, LV_SPOUT2_EN);
  check(h.trace() == "[CUE]TONE(6000,100)", "the enabled spout still does");
}


// ===========================================================================
//  FRAMEWORK invariants
// ===========================================================================

static void test_switch_safety() {
  printf("framework: safeToSwitch only true at a trial boundary\n");

  Harness h1(taskById(1));
  h1.cycle();
  check(h1.task()->safeToSwitch(), "task 1 in ARMED is switchable");
  h1.cycle(EV_LICK1);
  check(!h1.task()->safeToSwitch(), "task 1 mid-gate is not");

  Harness h2(taskById(2));
  h2.cycle();
  check(h2.task()->safeToSwitch(), "task 2 in WAIT_LICK is switchable");
  h2.cycle(EV_LICK1);
  check(!h2.task()->safeToSwitch(), "task 2 mid-trial is not");
}

static void test_reset_is_clean() {
  printf("framework: reset() clears trial count, state and timers\n");
  Task *t = taskById(2);
  Harness h(t, 5000);
  h.cycle();
  h.cycle(EV_LICK1);
  // A whole trial is T2_CUE_TO_WATER + REWARD_INTERVAL1, so allow for both.
  h.advance(T2_CUE_TO_WATER + REWARD_INTERVAL1 + 10);
  check(t->trial() >= 1, "a trial was completed before the reset");

  Harness fresh(t, 90000);                      // reset() via the constructor
  fresh.cycle();
  check(t->trial() == 0, "trial count cleared");
  check(fresh.trace() == "[WAIT_LICK]", "back in the initial state");
}

static void test_timeout_does_not_leak() {
  printf("framework: an armed timeout is cancelled by any transition\n");
  // Task 1 arms REWARD_INTERVAL1 on entering REFRACTORY. After returning to
  // ARMED, no stale EV_TIMEOUT may fire: ARMED must be left only by a lick.
  Harness h(taskById(1));
  h.cycle();
  h.cycle(EV_LICK1);
  h.advance(3000);                              // -> ARMED
  h.clearTrace();
  h.advance(10000);                             // sit in ARMED, no licks
  check(h.trace() == "", "ARMED never self-transitions on a stale timer");
}

static void test_millis_rollover() {
  printf("framework: timeouts survive the millis() rollover\n");
  Harness h(taskById(1), 0xFFFFFF00u);          // 256 ms before wrapping
  h.cycle();
  h.clearTrace();
  h.cycle(EV_LICK1);                            // arms a 3000 ms gate across 0
  check(h.trace() == "[REFRACTORY]REWARD(1)", "rewarded just before the wrap");
  h.clearTrace();

  h.advance(2999);
  check(h.trace() == "", "still gated across the rollover");
  h.advance(2);
  check(h.trace() == "[ARMED]", "re-arms correctly after the wrap");
}


int main() {
  test_task1_basic();
  test_task1_shared_gate_uses_that_spouts_interval();
  test_task1_disabled_spout();
  test_task1_simultaneous_licks();

  test_task2_full_trial();
  test_task2_only_first_lick_counts();
  test_task2_cue_at_onset();
  test_task2_disabled_spout();

  test_switch_safety();
  test_reset_is_clean();
  test_timeout_does_not_leak();
  test_millis_rollover();

  printf("\n%s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
