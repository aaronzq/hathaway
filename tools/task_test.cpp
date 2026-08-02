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
unsigned long T2_N1            = 3;
unsigned long T2_N2            = 2;

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
  // The default levels are "mouse in position, both spouts on": task 1 ignores
  // the position bit, and task 2 is gated by it, so tests that want the mouse
  // away pass the levels explicitly.
  void cycle(uint32_t events = 0,
             uint32_t levels = LV_IN_POSITION | LV_SPOUT1_EN | LV_SPOUT2_EN) {
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
  h.cycle(0, LV_IN_POSITION | LV_SPOUT1_EN);    // spout 2 disabled
  h.clearTrace();

  h.cycle(EV_LICK2, LV_IN_POSITION | LV_SPOUT1_EN);
  check(h.trace() == "", "lick on the disabled spout does nothing");

  h.cycle(EV_LICK1, LV_IN_POSITION | LV_SPOUT1_EN);
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
//  TASK 2 -- cued reward at one spout at a time, gated by position
// ===========================================================================

// "Mouse away from the port", for the tests that need it.
static const uint32_t AWAY = LV_SPOUT1_EN | LV_SPOUT2_EN;

// Every REWARD(n) in the trace, in order, as a string of spout digits. Written
// so a whole alternation block can be asserted as one literal.
static std::string rewardSeq(const std::string &t) {
  std::string s;
  for (size_t p = t.find("REWARD("); p != std::string::npos; p = t.find("REWARD(", p + 1))
    s += t[p + 7];
  return s;
}

static void test_task2_full_trial() {
  printf("task 2: in position -> cue -> lick -> water -> gate -> cue again\n");
  Harness h(taskById(2));

  h.cycle(0, AWAY);
  check(h.trace() == "[IDLE]", "starts in IDLE and stays there out of position");
  h.clearTrace();

  h.cycle();
  check(h.trace() == "[CUE]TONE(6000,100)", "arriving in position plays the go cue");
  h.clearTrace();

  h.cycle(EV_LICK1);
  check(h.trace() == "", "a lick inside T2_CUE_TO_WATER earns nothing");
  h.advance(101);
  check(h.trace() == "[WAIT_LICK]", "the lick window opens after T2_CUE_TO_WATER");
  h.clearTrace();

  h.advance(5000);
  check(h.trace() == "", "and then waits indefinitely: there is no response deadline");
  h.clearTrace();

  h.cycle(EV_LICK2);
  check(h.trace() == "", "a lick on the blocked spout does nothing");

  h.cycle(EV_LICK1);
  check(h.trace() == "[REFRACTORY]REWARD(1)", "a lick on the active spout waters it");
  check(h.task()->trial() == 1, "counted at the moment of reward, not at gate end");
  h.clearTrace();

  h.advance(2999);
  check(h.trace() == "", "still gated at 2999 ms, spout 1's own interval");
  h.advance(2);
  check(h.trace() == "[CUE]TONE(6000,100)", "cues again with no lick needed to start");
}

static void test_task2_one_reward_per_gate() {
  printf("task 2: a lick storm inside one gate still earns exactly one reward\n");
  Harness h(taskById(2));
  h.cycle();
  h.clearTrace();

  // 2000 ms of licking, well inside REWARD_INTERVAL1 = 3000.
  for (int i = 0; i < 40; i++) { h.bump(50); h.cycle(EV_LICK1 | EV_LICK2); }
  check(rewardSeq(h.trace()) == "1", "one reward, on the active spout, despite 40 licks");
}

static void test_task2_alternation() {
  printf("task 2: T2_N1 rewards at spout 1, then T2_N2 at spout 2, repeating\n");
  Harness h(taskById(2));       // reset() must also restart the block at spout 1

  // Lick both spouts on every cycle: only the active one can ever be rewarded,
  // so the sequence below is produced entirely by the alternation rule.
  for (uint32_t i = 0; i < 20000; i++) { h.bump(1); h.cycle(EV_LICK1 | EV_LICK2); }

  std::string seq = rewardSeq(h.trace());
  check(seq.size() >= 6, "at least six rewards in 20 s");
  check(seq.substr(0, 6) == "111221",
        "T2_N1 = 3 at spout 1, T2_N2 = 2 at spout 2, then back to spout 1");
}

static void test_task2_block_survives_leaving() {
  printf("task 2: leaving position aborts the trial but keeps the block counter\n");
  Harness h(taskById(2));

  // One reward on spout 1 (block is 3 long), then walk off mid-gate.
  h.cycle();
  h.advance(101);
  h.cycle(EV_LICK1);
  h.clearTrace();
  h.advance(500);
  h.cycle(0, AWAY);
  check(h.trace() == "[IDLE]", "leaving mid-gate drops straight to IDLE");
  h.clearTrace();

  h.cycle();
  check(h.trace() == "[CUE]TONE(6000,100)",
        "the gate is released, not resumed: coming back cues at once");
  h.clearTrace();

  // Two more rewards finish the spout-1 block; the third must flip to spout 2.
  for (uint32_t i = 0; i < 10000; i++) { h.bump(1); h.cycle(EV_LICK1 | EV_LICK2); }
  check(rewardSeq(h.trace()).substr(0, 3) == "112",
        "the block resumed at reward 2 of 3 rather than restarting");
}

static void test_task2_licks_from_onset() {
  printf("task 2: T2_CUE_TO_WATER = 0 makes licks count from cue onset\n");
  unsigned long saved = T2_CUE_TO_WATER;
  T2_CUE_TO_WATER = 0;

  Harness h(taskById(2));
  h.cycle();
  check(h.trace() == "[CUE]TONE(6000,100)", "the cue still plays for its full 100 ms");
  h.clearTrace();

  h.cycle();
  check(h.trace() == "[WAIT_LICK]", "the lick window opens on the very next cycle");
  h.clearTrace();

  h.cycle(EV_LICK1);
  check(h.trace() == "[REFRACTORY]REWARD(1)", "and the next lick waters immediately");

  T2_CUE_TO_WATER = saved;
}

static void test_task2_ignores_the_task1_flags() {
  printf("task 2: T1_SPOUTn_ENABLE is task 1's, and task 2 does not read it\n");
  Harness h(taskById(2));
  const uint32_t lv = LV_IN_POSITION;           // BOTH spouts off as far as task 1 cares

  for (uint32_t i = 0; i < 20000; i++) { h.bump(1); h.cycle(EV_LICK1 | EV_LICK2, lv); }
  check(rewardSeq(h.trace()).substr(0, 6) == "111221",
        "the alternation runs exactly as if the flags were on");
}

static void test_task2_zero_block_retires_a_spout() {
  printf("task 2: T2_N1 = 0 puts every trial on spout 2\n");
  unsigned long saved = T2_N1;
  T2_N1 = 0;

  Harness h(taskById(2));
  for (uint32_t i = 0; i < 20000; i++) { h.bump(1); h.cycle(EV_LICK1 | EV_LICK2); }

  std::string seq = rewardSeq(h.trace());
  check(seq.size() >= 6, "trials still run");
  check(seq.find('1') == std::string::npos,
        "and none of them are on spout 1, however long T2_N2 makes the blocks");

  T2_N1 = saved;
}

static void test_task2_both_blocks_zero_stops_the_task() {
  printf("task 2: both block lengths zero -- one cue on arrival, then nothing\n");
  unsigned long s1 = T2_N1, s2 = T2_N2;
  T2_N1 = 0;
  T2_N2 = 0;

  Harness h(taskById(2));
  h.cycle();
  check(h.trace() == "[CUE]TONE(6000,100)", "the mouse still gets its one cue");
  h.clearTrace();

  for (uint32_t i = 0; i < 20000; i++) { h.bump(1); h.cycle(EV_LICK1 | EV_LICK2); }
  check(h.trace() == "[WAIT_LICK]",
        "then it opens the lick window once and waits there for good");
  check(h.task()->trial() == 0, "no trial, no water, no second cue");

  T2_N1 = s1;
  T2_N2 = s2;
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
  h2.cycle(0, AWAY);
  check(h2.task()->safeToSwitch(), "task 2 in IDLE is switchable");
  h2.cycle();
  check(!h2.task()->safeToSwitch(), "task 2 mid-trial is not");
}

static void test_reset_is_clean() {
  printf("framework: reset() clears trial count, state and timers\n");
  Task *t = taskById(2);
  Harness h(t, 5000);
  h.cycle();
  h.advance(T2_CUE_TO_WATER + 1);               // through the cue into WAIT_LICK
  h.cycle(EV_LICK1);
  check(t->trial() >= 1, "a trial was completed before the reset");

  Harness fresh(t, 90000);                      // reset() via the constructor
  fresh.cycle(0, AWAY);
  check(t->trial() == 0, "trial count cleared");
  check(fresh.trace() == "[IDLE]", "back in the initial state");
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
  test_task2_one_reward_per_gate();
  test_task2_alternation();
  test_task2_block_survives_leaving();
  test_task2_licks_from_onset();
  test_task2_ignores_the_task1_flags();
  test_task2_zero_block_retires_a_spout();
  test_task2_both_blocks_zero_stops_the_task();

  test_switch_safety();
  test_reset_is_clean();
  test_timeout_does_not_leak();
  test_millis_rollover();

  printf("\n%s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
