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
unsigned long T3_SAMPLE_FREQ1  = 12000;
unsigned long T3_SAMPLE_FREQ2  = 3000;
unsigned long T3_PULSE_MS      = 150;
unsigned long T3_GAP_MS        = 100;
unsigned long T3_N_PULSES      = 3;
unsigned long T3_DELAY_MS      = 1200;
unsigned long T3_CUE_FREQ      = 6000;
unsigned long T3_CUE_DUR       = 100;
unsigned long T3_RESPONSE_MS   = 1500;
unsigned long T3_CONSUME_MS    = 1500;
unsigned long T3_PUNISH_MS     = 2000;
unsigned long T3_ITI_MS        = 250;
unsigned long T3_MAX_REPEAT    = 3;
unsigned long T3_ANTI_BIAS_PROB1 = 50;
unsigned long T3_ANTI_BIAS_AUTO_ENABLE = 0;
unsigned long T3_ANTI_BIAS_WIN = 30;
unsigned long T3_ANTI_BIAS_ACC_THRESH = 65;
unsigned long T3_TEACH_PROB      = 0;
unsigned long T3_TEACH_INCLUDE_ABORT = 0;
unsigned long T3_EARLY_LICK_PUNISH = 1;
unsigned long T3_EARLY_LICK_PAUSE_MS = 100;

// The random source task 3 draws its trial type from. On the firmware this is
// esp_random(); here it is a scripted ring, so a whole trial SEQUENCE can be
// asserted rather than just one trial. The task reads it modulo 100 against a
// percentage, so a scripted value of 10 is "type 1 at any threshold above 10".
static std::vector<uint32_t> g_rand;
static size_t                g_randIdx = 0;

uint32_t task_rand32() {
  if (g_rand.empty()) return 0;
  return g_rand[g_randIdx++ % g_rand.size()];
}

// Load the draw sequence and rewind it. Call before constructing a Harness.
static void setRand(std::vector<uint32_t> seq) {
  g_rand    = seq;
  g_randIdx = 0;
}

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
      else if (a.verb == ACT_TONE_TRAIN)
                                   snprintf(buf, sizeof(buf), "TRAIN(%u)", (unsigned)a.a0);
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
//  TASK 3 -- two-tone discrimination with a delay, gated by position
// ===========================================================================

// Total length of the sample train, the same arithmetic the task does.
static uint32_t trainMs() {
  return T3_N_PULSES * T3_PULSE_MS + (T3_N_PULSES - 1) * T3_GAP_MS;
}

// Every [SAMPLEn] entered, in order, as a string of type digits. Written so a
// whole trial-type sequence can be asserted as one literal.
static std::string sampleSeq(const std::string &t) {
  std::string s;
  for (size_t p = t.find("[SAMPLE"); p != std::string::npos; p = t.find("[SAMPLE", p + 1))
    s += t[p + 7];
  return s;
}

// Walk the machine from IDLE up to the open response window, without ever
// licking, and report which spout is the correct answer for the trial it drew.
static uint8_t toResponse(Harness &h) {
  h.advance(1);                            // IDLE -> SAMPLEn (draws the type)
  const uint8_t correct = (h.task()->state() == T3_SAMPLE1) ? 1 : 2;
  h.advance(trainMs());                    // -> DELAY
  h.advance(T3_DELAY_MS);                  // -> GOCUE
  h.advance(T3_CUE_DUR);                   // -> RESPONSE
  return correct;
}

// One whole trial, answered correctly, leaving the machine back in IDLE.
static void runHitTrial(Harness &h) {
  const uint8_t correct = toResponse(h);
  h.cycle(correct == 1 ? EV_LICK1 : EV_LICK2);
  h.advance(T3_CONSUME_MS);                // -> ITI
  h.advance(T3_ITI_MS);                    // -> IDLE
}

// One whole trial left unanswered, leaving the machine back in IDLE. The
// consumption period is only served if the trial was rescued -- advancing
// through it unconditionally would start the NEXT trial and desync the caller.
static void runNoResponseTrial(Harness &h) {
  toResponse(h);
  h.advance(T3_RESPONSE_MS);                // -> REWARD if rescued, else ITI
  if (h.task()->state() == T3_REWARD) h.advance(T3_CONSUME_MS);
  h.advance(T3_ITI_MS);                     // -> IDLE
}

static bool takeProb1(Harness &h, uint8_t &prob) {
  return h.task()->takeT3Prob1(prob);
}

static void runAnsweredTrial(Harness &h, bool hit) {
  const uint8_t correct = toResponse(h);
  const uint32_t ev = hit
    ? (correct == 1 ? EV_LICK1 : EV_LICK2)
    : (correct == 1 ? EV_LICK2 : EV_LICK1);
  h.cycle(ev);
  h.advance(hit ? T3_CONSUME_MS : T3_PUNISH_MS);
  h.advance(T3_ITI_MS);
}

// Count how many of the sample states entered were of one type.
static size_t countType(const std::string &seq, char type) {
  size_t n = 0;
  for (char c : seq) if (c == type) n++;
  return n;
}

static void test_task3_hit_trial() {
  printf("task 3: sample -> delay -> go cue -> correct lick -> water -> ITI\n");
  setRand({0});                            // bit 0 clear = trial type 1
  Harness h(taskById(3));

  h.cycle(0, AWAY);
  check(h.trace() == "[IDLE]", "starts in IDLE and stays there out of position");
  h.clearTrace();

  h.cycle();
  check(h.trace() == "[SAMPLE1]TRAIN(12000)",
        "arriving at the port draws a trial type and plays its sample train");
  h.clearTrace();

  h.advance(trainMs() - 1);
  check(h.trace() == "", "the sample epoch lasts the whole 650 ms train");
  h.advance(1);
  check(h.trace() == "[DELAY]", "then the delay begins, in silence");
  h.clearTrace();

  h.advance(T3_DELAY_MS);
  check(h.trace() == "[GOCUE]TONE(6000,100)", "the go cue follows T3_DELAY_MS");
  h.clearTrace();

  h.advance(T3_CUE_DUR);
  check(h.trace() == "[RESPONSE]", "the response window opens when the cue ends");
  h.clearTrace();

  h.cycle(EV_LICK1);
  check(h.trace() == "[REWARD]REWARD(1)",
        "type 1 answered on spout 1 waters spout 1");
  h.clearTrace();

  h.advance(T3_CONSUME_MS);
  check(h.trace() == "[ITI]", "drinking time, then the inter-trial interval");
  check(h.task()->trial() == 1, "the trial is booked on entry to ITI");
  check(h.task()->outcomeCount(OUTCOME_HIT) == 1, "and booked as a hit");
  h.clearTrace();

  h.advance(T3_ITI_MS);
  check(h.trace() == "[IDLE]", "and the machine is ready for the next trial");
}

static void test_task3_incorrect() {
  printf("task 3: the wrong spout earns a timeout and no water\n");
  setRand({0});                            // type 1, so spout 2 is wrong
  Harness h(taskById(3));
  const uint8_t correct = toResponse(h);
  check(correct == 1, "drew trial type 1");
  h.clearTrace();

  h.cycle(EV_LICK2);
  check(h.trace() == "[PUNISH]", "no REWARD action: the wrong spout is not watered");
  h.clearTrace();

  h.advance(T3_PUNISH_MS - 1);
  check(h.trace() == "", "the timeout runs its full T3_PUNISH_MS");
  h.advance(1);
  check(h.trace() == "[ITI]", "then the usual ITI");
  check(h.task()->outcomeCount(OUTCOME_INCORRECT) == 1, "counted as incorrect");
  check(h.task()->outcomeCount(OUTCOME_HIT) == 0, "and not as a hit");
}

static void test_task3_no_response_deadline() {
  printf("task 3: an unanswered response window is a no-response\n");
  setRand({0});
  Harness h(taskById(3));
  toResponse(h);
  h.clearTrace();

  h.advance(T3_RESPONSE_MS - 1);
  check(h.trace() == "", "still accepting an answer at 1499 ms");
  h.advance(1);
  check(h.trace() == "[ITI]", "the window shuts at T3_RESPONSE_MS");
  check(h.task()->outcomeCount(OUTCOME_NO_RESPONSE) == 1, "counted as no-response");
}

static void test_task3_no_response_by_leaving() {
  printf("task 3: leaving AFTER the go cue without answering is a no-response\n");
  setRand({0});
  Harness h(taskById(3));
  toResponse(h);
  h.clearTrace();

  h.cycle(0, AWAY);
  check(h.trace() == "[ITI]", "walking off mid-window ends the trial");
  check(h.task()->outcomeCount(OUTCOME_NO_RESPONSE) == 1,
        "as a no-response, not an abort: the question had been asked");
  check(h.task()->outcomeCount(OUTCOME_ABORT) == 0, "so nothing is booked as an abort");
}

static void test_task3_abort_before_gocue() {
  printf("task 3: leaving BEFORE the go cue aborts, and still serves the ITI\n");
  setRand({0});
  Harness h(taskById(3));
  h.advance(1);
  h.advance(trainMs());                    // -> DELAY
  h.clearTrace();

  h.cycle(0, AWAY);
  check(h.trace() == "[ITI]", "leaving during the delay ends the trial at once");
  check(h.task()->outcomeCount(OUTCOME_ABORT) == 1, "counted as an abort");
  h.clearTrace();

  // The ITI is served even by an abort, so a mouse cannot restart a trial
  // instantly by stepping out and back in.
  h.advance(T3_ITI_MS - 1);
  check(h.trace() == "", "the ITI is served by an abort too");
  h.advance(1);
  check(h.trace() == "[IDLE]", "and only then is a new trial available");
}

static void test_task3_abort_wins_over_a_simultaneous_lick() {
  printf("task 3: a lick on the cycle the mouse leaves is an abort, not a pause\n");
  setRand({0});
  Harness h(taskById(3));
  h.advance(1);                            // -> SAMPLE1
  h.advance(trainMs());                    // -> DELAY
  h.clearTrace();

  h.cycle(EV_LICK1, AWAY);
  check(h.trace() == "[ITI]", "position is checked before the early-lick rule");
  check(h.task()->outcomeCount(OUTCOME_ABORT) == 1, "so the trial aborts");
}

static void test_task3_teach_excludes_abort_by_default() {
  printf("task 3: T3_TEACH_INCLUDE_ABORT = 0 keeps aborts unrescued\n");
  unsigned long sp = T3_TEACH_PROB, si = T3_TEACH_INCLUDE_ABORT;
  T3_TEACH_PROB = 100;
  T3_TEACH_INCLUDE_ABORT = 0;
  setRand({0});                            // type 1

  Harness h(taskById(3));
  h.advance(1);                            // -> SAMPLE1
  h.advance(trainMs());                    // -> DELAY
  h.clearTrace();

  h.cycle(0, AWAY);
  check(h.trace() == "[ITI]", "even at 100% teach probability, abort goes straight to ITI");
  check(h.task()->outcomeCount(OUTCOME_ABORT) == 1, "booked as ABORT");
  check(h.task()->outcomeCount(OUTCOME_TEACH) == 0, "not as TEACH");

  T3_TEACH_PROB = sp;
  T3_TEACH_INCLUDE_ABORT = si;
}

static void test_task3_teach_can_include_abort() {
  printf("task 3: T3_TEACH_INCLUDE_ABORT = 1 lets T3_TEACH_PROB rescue aborts\n");
  unsigned long sp = T3_TEACH_PROB, si = T3_TEACH_INCLUDE_ABORT;
  T3_TEACH_PROB = 100;
  T3_TEACH_INCLUDE_ABORT = 1;
  setRand({0, 0});                         // type 1, then rescue draw passes

  Harness h(taskById(3));
  h.advance(1);                            // -> SAMPLE1
  h.advance(trainMs());                    // -> DELAY
  h.clearTrace();

  h.cycle(0, AWAY);
  check(h.trace() == "[REWARD]REWARD(1)",
        "leaving before the go cue can be rescued at the correct spout");
  h.advance(T3_CONSUME_MS);
  check(h.task()->outcomeCount(OUTCOME_TEACH) == 1, "booked as TEACH");
  check(h.task()->outcomeCount(OUTCOME_ABORT) == 0, "not as ABORT");

  T3_TEACH_PROB = sp;
  T3_TEACH_INCLUDE_ABORT = si;
}

static void test_task3_early_lick_replays_delay() {
  printf("task 3: T3_EARLY_LICK_PUNISH pauses only delay licks, then replays delay\n");
  setRand({0});                            // every draw is type 1
  Harness h(taskById(3));
  h.advance(1);                            // -> SAMPLE1
  h.clearTrace();

  h.cycle(EV_LICK2);
  check(h.trace() == "", "a lick during the sample is logged but does not punish");
  h.clearTrace();

  h.advance(trainMs());                    // -> DELAY
  h.advance(400);                          // 800 ms of the delay remains
  h.clearTrace();

  h.cycle(EV_LICK1);
  check(h.trace() == "[EARLY_PAUSE]",
        "a lick during the delay starts the early-lick pause");
  h.clearTrace();

  h.advance(T3_EARLY_LICK_PAUSE_MS);
  check(h.trace() == "[DELAY]", "after the pause, the delay restarts");
  h.clearTrace();

  h.advance(T3_DELAY_MS - 1);
  check(h.trace() == "", "the go cue does not arrive before a full replayed delay is served");
  h.advance(1);
  check(h.trace() == "[GOCUE]TONE(6000,100)",
        "the go cue arrives after pause plus a fresh full delay");
  check(h.task()->trial() == 0, "the pause is not a trial outcome");
}

static void test_task3_aborted_delay_pause_does_not_shorten_next_delay() {
  printf("task 3: aborting an early-lick pause does not shorten the next delay\n");
  unsigned long se = T3_EARLY_LICK_PUNISH;
  unsigned long st = T3_TEACH_PROB;
  unsigned long si = T3_TEACH_INCLUDE_ABORT;
  T3_EARLY_LICK_PUNISH = 1;
  T3_TEACH_PROB = 0;
  T3_TEACH_INCLUDE_ABORT = 0;
  setRand({0, 0});                         // type 1, then type 1 again

  Harness h(taskById(3));
  h.advance(1);                            // -> SAMPLE1
  h.advance(trainMs());                    // -> DELAY
  h.advance(T3_DELAY_MS - 30);             // leave a distinctive 30 ms remainder
  h.clearTrace();

  h.cycle(EV_LICK1);                       // -> EARLY_PAUSE near the end of delay
  check(h.trace() == "[EARLY_PAUSE]", "delay lick enters early pause");
  h.clearTrace();

  h.cycle(0, AWAY);                        // abort during the early-lick pause
  check(h.trace() == "[ITI]", "leaving during early pause aborts the trial");
  h.advance(T3_ITI_MS);                    // -> IDLE
  h.clearTrace();

  h.advance(1);                            // next trial -> SAMPLE1
  h.advance(trainMs());                    // -> DELAY
  h.clearTrace();
  h.advance(T3_DELAY_MS - 1);
  check(h.trace() == "", "next trial does not reuse the old 30 ms delay remainder");
  h.advance(1);
  check(h.trace() == "[GOCUE]TONE(6000,100)", "next trial gets the full delay");

  T3_EARLY_LICK_PUNISH = se;
  T3_TEACH_PROB = st;
  T3_TEACH_INCLUDE_ABORT = si;
}

static void test_task3_early_lick_tolerated() {
  printf("task 3: T3_EARLY_LICK_PUNISH = 0 tolerates licks before the go cue\n");
  unsigned long saved = T3_EARLY_LICK_PUNISH;
  T3_EARLY_LICK_PUNISH = 0;

  setRand({0});
  Harness h(taskById(3));
  h.advance(1);                            // -> SAMPLE1
  h.clearTrace();

  for (int i = 0; i < 10; i++) { h.bump(10); h.cycle(EV_LICK1 | EV_LICK2); }
  check(h.trace() == "", "licks during the sample change nothing at all");

  h.advance(trainMs());                    // -> DELAY (and past it, harmlessly)
  h.clearTrace();
  h.cycle(EV_LICK1);
  check(h.trace() == "", "and neither do licks during the delay");

  T3_EARLY_LICK_PUNISH = saved;
}

static void test_task3_repeat_cap() {
  printf("task 3: the same trial type cannot run more than T3_MAX_REPEAT times\n");
  setRand({0});                            // a rigged coin: ALWAYS type 1
  Harness h(taskById(3));

  for (int i = 0; i < 8; i++) runHitTrial(h);

  // Trials 1-3 take the draw; trial 4 is forced to the other type, which resets
  // the run. So a coin stuck on type 1 still yields one type-2 trial in four.
  check(sampleSeq(h.trace()) == "11121112",
        "three type-1 trials, then a forced type 2, repeating");
  check(h.task()->trial() == 8, "eight trials, all of them answered correctly");
  check(h.task()->outcomeCount(OUTCOME_HIT) == 8, "and all eight booked as hits");
}

static void test_task3_prob1_drives_the_split() {
  printf("task 3: T3_ANTI_BIAS_PROB1 sets the percentage of type-1 trials\n");
  // Two scripted draws, 10 and 70, replayed in a ring. Which type each becomes
  // depends entirely on where T3_ANTI_BIAS_PROB1 sits between them.
  unsigned long saved = T3_ANTI_BIAS_PROB1;

  T3_ANTI_BIAS_PROB1 = 50;                 // 10 -> type 1, 70 -> type 2
  setRand({10, 70});
  Harness a(taskById(3));
  for (int i = 0; i < 6; i++) runHitTrial(a);
  check(sampleSeq(a.trace()) == "121212", "at 50 the two draws straddle it and alternate");

  T3_ANTI_BIAS_PROB1 = 80;                 // now BOTH draws are under it
  setRand({10, 70});
  Harness b(taskById(3));
  for (int i = 0; i < 8; i++) runHitTrial(b);
  check(sampleSeq(b.trace()) == "11121112",
        "at 80 both draws say type 1, and only T3_MAX_REPEAT breaks the run");

  T3_ANTI_BIAS_PROB1 = 0;                  // neither draw is under it
  setRand({10, 70});
  Harness c(taskById(3));
  for (int i = 0; i < 8; i++) runHitTrial(c);
  check(sampleSeq(c.trace()) == "22212221", "at 0 every trial is type 2, capped the same way");

  T3_ANTI_BIAS_PROB1 = saved;
}

static void test_task3_max_repeat_caps_the_bias() {
  printf("task 3: T3_MAX_REPEAT bounds the achievable bias to N/(N+1)\n");
  unsigned long sp = T3_ANTI_BIAS_PROB1, sc = T3_MAX_REPEAT;
  T3_ANTI_BIAS_PROB1 = 100;                // ask for every trial to be type 1

  T3_MAX_REPEAT = 3;
  setRand({0});
  Harness a(taskById(3));
  for (int i = 0; i < 20; i++) runHitTrial(a);
  std::string sa = sampleSeq(a.trace());
  check(countType(sa, '1') == 15 && sa.size() == 20,
        "cap 3 yields 15/20 type 1 -- 75%, not the 100% asked for");

  T3_MAX_REPEAT = 9;                       // raise the cap, raise the ceiling
  setRand({0});
  Harness b(taskById(3));
  for (int i = 0; i < 20; i++) runHitTrial(b);
  std::string sb = sampleSeq(b.trace());
  check(countType(sb, '1') == 18 && sb.size() == 20,
        "cap 9 yields 18/20 -- 90%, so the cap and not the probability is the limit");

  T3_ANTI_BIAS_PROB1 = sp;
  T3_MAX_REPEAT = sc;
}

static void test_task3_teach_rescues_a_deadline() {
  printf("task 3: T3_TEACH_PROB waters the correct spout on an unanswered trial\n");
  T3_TEACH_PROB = 100;                     // rescue every one, for the test
  setRand({0});                            // type 1, so spout 1 is correct

  Harness h(taskById(3));
  toResponse(h);
  h.clearTrace();

  h.advance(T3_RESPONSE_MS);
  check(h.trace() == "[REWARD]REWARD(1)",
        "the window closes and water arrives at the CORRECT spout");
  h.clearTrace();

  h.advance(T3_CONSUME_MS);
  check(h.trace() == "[ITI]", "then the usual consumption period and ITI");
  check(h.task()->outcomeCount(OUTCOME_TEACH) == 1, "booked as TEACH");
  check(h.task()->outcomeCount(OUTCOME_HIT) == 0, "and never as a hit");
  check(h.task()->outcomeCount(OUTCOME_NO_RESPONSE) == 0, "nor as a no-response");

  T3_TEACH_PROB = 0;
}

static void test_task3_teach_rescues_a_walk_off_too() {
  printf("task 3: leaving the port mid-window is rescued on the same odds\n");
  T3_TEACH_PROB = 100;
  setRand({50});                           // type 2, so spout 2 is correct

  Harness h(taskById(3));
  toResponse(h);
  h.clearTrace();

  h.cycle(0, AWAY);
  check(h.trace() == "[REWARD]REWARD(2)",
        "walking off is rescued too, and on that trial's own correct spout");
  h.advance(T3_CONSUME_MS);
  check(h.task()->outcomeCount(OUTCOME_TEACH) == 1, "also booked as TEACH");

  T3_TEACH_PROB = 0;
}

static void test_task3_teach_off_gives_a_plain_no_response() {
  printf("task 3: T3_TEACH_PROB = 0 leaves an unanswered trial unrescued\n");
  setRand({0});
  Harness h(taskById(3));                  // T3_TEACH_PROB is 0 by default here
  toResponse(h);
  h.clearTrace();

  h.advance(T3_RESPONSE_MS);
  check(h.trace() == "[ITI]", "no REWARD action at all: straight to the ITI");
  check(h.task()->outcomeCount(OUTCOME_NO_RESPONSE) == 1, "booked as no-response");
  check(h.task()->outcomeCount(OUTCOME_TEACH) == 0, "and nothing booked as TEACH");
}

static void test_task3_teach_draw_only_when_enabled() {
  printf("task 3: the rescue draw is taken only when T3_TEACH_PROB is set\n");
  // Draws 0 and 50 in a ring: 0 -> type 1, 50 -> type 2 at the default 50%.
  // Off, an unanswered trial takes no draw, so the next trial gets the 50.
  // On, the rescue eats the 50 and the next trial gets the 0 again.
  T3_TEACH_PROB = 0;
  setRand({0, 50});
  Harness a(taskById(3));
  runNoResponseTrial(a);
  runNoResponseTrial(a);
  check(sampleSeq(a.trace()) == "12", "off: two trials consume two draws");

  T3_TEACH_PROB = 100;
  setRand({0, 50});
  Harness b(taskById(3));
  runNoResponseTrial(b);
  runNoResponseTrial(b);
  check(sampleSeq(b.trace()) == "11",
        "on: the first trial's rescue consumes the 50, so the second draws 0 again");
  check(b.task()->outcomeCount(OUTCOME_TEACH) == 2, "both trials were rescued");

  T3_TEACH_PROB = 0;
}

static void test_task3_type2_maps_to_spout2() {
  printf("task 3: trial type 2 plays its own tone and is answered on spout 2\n");
  setRand({50});                           // 50 is not < 50, so trial type 2
  Harness h(taskById(3));

  h.cycle(0, AWAY);
  h.clearTrace();
  h.cycle();
  check(h.trace() == "[SAMPLE2]TRAIN(3000)", "type 2 plays T3_SAMPLE_FREQ2");
  h.clearTrace();

  h.advance(trainMs());
  h.advance(T3_DELAY_MS);
  h.advance(T3_CUE_DUR);                   // -> RESPONSE
  h.clearTrace();

  h.cycle(EV_LICK1);
  check(h.trace() == "[PUNISH]", "spout 1 is now the wrong answer");

  h.advance(T3_PUNISH_MS);                 // outcomes are booked on entry to ITI
  check(h.task()->outcomeCount(OUTCOME_INCORRECT) == 1, "and is scored incorrect");
}

static void test_task3_simultaneous_licks_score_a_hit() {
  printf("task 3: licking both spouts at once is scored as a hit\n");
  setRand({0});
  Harness h(taskById(3));
  toResponse(h);
  h.clearTrace();

  h.cycle(EV_LICK1 | EV_LICK2);
  check(h.trace() == "[REWARD]REWARD(1)", "the correct spout is tested first");
}

static void test_task3_outcomes_account_for_every_trial() {
  printf("task 3: the five outcome counts always add up to trial()\n");
  setRand({0, 50, 0, 0, 50, 0});
  Harness h(taskById(3));

  // One of every outcome: a hit, an incorrect, a deadline no-response, an abort,
  // and a rescued no-response. Loop over the whole enum at the end rather than
  // naming four of them, so adding a sixth outcome without a test fails here.
  runHitTrial(h);

  uint8_t correct = toResponse(h);
  h.cycle(correct == 1 ? EV_LICK2 : EV_LICK1);   // wrong spout
  h.advance(T3_PUNISH_MS);
  h.advance(T3_ITI_MS);

  toResponse(h);
  h.advance(T3_RESPONSE_MS);                     // no answer, no rescue
  h.advance(T3_ITI_MS);

  h.advance(1);                                  // -> SAMPLEn
  h.cycle(0, AWAY);                              // abort
  h.advance(T3_ITI_MS);

  T3_TEACH_PROB = 100;
  runNoResponseTrial(h);                         // no answer, rescued
  T3_TEACH_PROB = 0;

  Task *t = h.task();
  uint32_t sum = 0;
  for (uint8_t o = 0; o < OUTCOME_COUNT; o++) sum += t->outcomeCount(o);
  check(t->trial() == 5, "five trials ran");
  check(sum == t->trial(), "and every one of them is booked under exactly one outcome");

  bool one_each = true;
  for (uint8_t o = 0; o < OUTCOME_COUNT; o++)
    if (t->outcomeCount(o) != 1) one_each = false;
  check(one_each, "one of each, including TEACH");
}

static void test_task3_zero_delay() {
  printf("task 3: T3_DELAY_MS = 0 goes from the sample straight to the go cue\n");
  unsigned long saved = T3_DELAY_MS;
  T3_DELAY_MS = 0;

  setRand({0});
  Harness h(taskById(3));
  h.advance(1);                            // -> SAMPLE1
  h.advance(trainMs());                    // -> DELAY
  h.clearTrace();

  h.advance(1);
  check(h.trace() == "[GOCUE]TONE(6000,100)",
        "the delay state is passed through on the next cycle, not skipped");

  T3_DELAY_MS = saved;
}

static void test_task3_prob1_reports_manual_value() {
  printf("task 3: T3_PROB1 reports the manual probability when auto is off\n");
  unsigned long sa = T3_ANTI_BIAS_AUTO_ENABLE, sp = T3_ANTI_BIAS_PROB1;
  T3_ANTI_BIAS_AUTO_ENABLE = 0;
  T3_ANTI_BIAS_PROB1 = 80;

  setRand({0});
  Harness h(taskById(3));
  h.cycle();

  uint8_t prob = 0;
  check(takeProb1(h, prob) && prob == 80,
        "the one-shot hook reports the manual draw probability");
  check(!takeProb1(h, prob), "and reports it only once per trial start");

  T3_ANTI_BIAS_AUTO_ENABLE = sa;
  T3_ANTI_BIAS_PROB1 = sp;
}

static void test_task3_auto_bias_cold_start_is_unbiased() {
  printf("task 3: automatic anti-bias is 50 until the answered window is full\n");
  unsigned long sa = T3_ANTI_BIAS_AUTO_ENABLE, sw = T3_ANTI_BIAS_WIN;
  T3_ANTI_BIAS_AUTO_ENABLE = 1;
  T3_ANTI_BIAS_WIN = 2;

  setRand({0});
  Harness h(taskById(3));
  h.cycle();

  uint8_t prob = 0;
  check(takeProb1(h, prob) && prob == 50, "cold start reports 50");

  T3_ANTI_BIAS_AUTO_ENABLE = sa;
  T3_ANTI_BIAS_WIN = sw;
}

static void test_task3_auto_bias_ignores_non_answer_outcomes() {
  printf("task 3: no-response, abort and teach do not occupy the anti-bias window\n");
  unsigned long sa = T3_ANTI_BIAS_AUTO_ENABLE, sw = T3_ANTI_BIAS_WIN;
  unsigned long sp = T3_ANTI_BIAS_PROB1, sm = T3_MAX_REPEAT, st = T3_TEACH_PROB;
  T3_ANTI_BIAS_AUTO_ENABLE = 0;
  T3_ANTI_BIAS_WIN = 2;
  T3_ANTI_BIAS_PROB1 = 50;
  T3_MAX_REPEAT = 100;
  T3_TEACH_PROB = 0;

  setRand({0, 50, 50, 50, 0, 50, 0});
  Harness h(taskById(3));
  runAnsweredTrial(h, true);       // type 1 hit
  runNoResponseTrial(h);           // type 2 no-response: ignored by the window
  h.advance(1);                    // type 2 abort: ignored by the window
  h.cycle(0, AWAY);
  h.advance(T3_ITI_MS);
  T3_TEACH_PROB = 100;
  runNoResponseTrial(h);           // type 2 teach: ignored by the window
  T3_TEACH_PROB = 0;
  runAnsweredTrial(h, false);      // type 2 incorrect

  T3_ANTI_BIAS_AUTO_ENABLE = 1;
  h.advance(1);

  uint8_t prob = 0;
  check(takeProb1(h, prob) && prob == 10,
        "the filled window contains type-1 hit and type-2 incorrect only");

  T3_ANTI_BIAS_AUTO_ENABLE = sa;
  T3_ANTI_BIAS_WIN = sw;
  T3_ANTI_BIAS_PROB1 = sp;
  T3_MAX_REPEAT = sm;
  T3_TEACH_PROB = st;
}

static void test_task3_auto_bias_accuracy_gate() {
  printf("task 3: automatic anti-bias returns to 50 once both accuracies pass threshold\n");
  unsigned long sa = T3_ANTI_BIAS_AUTO_ENABLE, sw = T3_ANTI_BIAS_WIN;
  unsigned long sp = T3_ANTI_BIAS_PROB1, sm = T3_MAX_REPEAT, sth = T3_ANTI_BIAS_ACC_THRESH;
  T3_ANTI_BIAS_AUTO_ENABLE = 0;
  T3_ANTI_BIAS_WIN = 2;
  T3_ANTI_BIAS_PROB1 = 50;
  T3_MAX_REPEAT = 100;
  T3_ANTI_BIAS_ACC_THRESH = 65;

  setRand({0, 50, 0});
  Harness h(taskById(3));
  runAnsweredTrial(h, true);       // type 1 hit
  runAnsweredTrial(h, true);       // type 2 hit

  T3_ANTI_BIAS_AUTO_ENABLE = 1;
  h.advance(1);

  uint8_t prob = 0;
  check(takeProb1(h, prob) && prob == 50, "both sides above criterion gives 50");

  T3_ANTI_BIAS_AUTO_ENABLE = sa;
  T3_ANTI_BIAS_WIN = sw;
  T3_ANTI_BIAS_PROB1 = sp;
  T3_MAX_REPEAT = sm;
  T3_ANTI_BIAS_ACC_THRESH = sth;
}

static void test_task3_auto_bias_requires_both_types_in_window() {
  printf("task 3: automatic anti-bias is 50 if one type has no answered trials\n");
  unsigned long sa = T3_ANTI_BIAS_AUTO_ENABLE, sw = T3_ANTI_BIAS_WIN;
  unsigned long sp = T3_ANTI_BIAS_PROB1, sm = T3_MAX_REPEAT;
  T3_ANTI_BIAS_AUTO_ENABLE = 0;
  T3_ANTI_BIAS_WIN = 2;
  T3_ANTI_BIAS_PROB1 = 50;
  T3_MAX_REPEAT = 100;

  setRand({0, 0, 0});
  Harness h(taskById(3));
  runAnsweredTrial(h, false);      // type 1 incorrect
  runAnsweredTrial(h, false);      // type 1 incorrect again

  T3_ANTI_BIAS_AUTO_ENABLE = 1;
  h.advance(1);

  uint8_t prob = 0;
  check(takeProb1(h, prob) && prob == 50, "type 2 accuracy is undefined, so use 50");

  T3_ANTI_BIAS_AUTO_ENABLE = sa;
  T3_ANTI_BIAS_WIN = sw;
  T3_ANTI_BIAS_PROB1 = sp;
  T3_MAX_REPEAT = sm;
}

static void test_task3_auto_bias_delta_direction_and_clamp() {
  printf("task 3: failure-rate delta moves probability toward the worse trial type\n");
  unsigned long sa = T3_ANTI_BIAS_AUTO_ENABLE, sw = T3_ANTI_BIAS_WIN;
  unsigned long sp = T3_ANTI_BIAS_PROB1, sm = T3_MAX_REPEAT;
  T3_ANTI_BIAS_WIN = 2;
  T3_ANTI_BIAS_PROB1 = 50;
  T3_MAX_REPEAT = 100;

  T3_ANTI_BIAS_AUTO_ENABLE = 0;
  setRand({0, 50, 0});
  Harness a(taskById(3));
  runAnsweredTrial(a, false);      // type 1 incorrect
  runAnsweredTrial(a, true);       // type 2 hit
  T3_ANTI_BIAS_AUTO_ENABLE = 1;
  a.advance(1);
  uint8_t prob = 0;
  check(takeProb1(a, prob) && prob == 90, "type 1 worse clamps probability at 90");

  T3_ANTI_BIAS_AUTO_ENABLE = 0;
  setRand({0, 50, 0});
  Harness b(taskById(3));
  runAnsweredTrial(b, true);       // type 1 hit
  runAnsweredTrial(b, false);      // type 2 incorrect
  T3_ANTI_BIAS_AUTO_ENABLE = 1;
  b.advance(1);
  check(takeProb1(b, prob) && prob == 10, "type 2 worse clamps probability at 10");

  T3_ANTI_BIAS_AUTO_ENABLE = sa;
  T3_ANTI_BIAS_WIN = sw;
  T3_ANTI_BIAS_PROB1 = sp;
  T3_MAX_REPEAT = sm;
}

static void test_task3_auto_bias_window_changes_live() {
  printf("task 3: changing T3_ANTI_BIAS_WIN changes the newest answered trials used\n");
  unsigned long sa = T3_ANTI_BIAS_AUTO_ENABLE, sw = T3_ANTI_BIAS_WIN;
  unsigned long sp = T3_ANTI_BIAS_PROB1, sm = T3_MAX_REPEAT;
  T3_ANTI_BIAS_AUTO_ENABLE = 0;
  T3_ANTI_BIAS_PROB1 = 50;
  T3_MAX_REPEAT = 100;

  setRand({0, 50, 0, 50, 0, 0});
  Harness h(taskById(3));
  runAnsweredTrial(h, true);       // type 1 hit
  runAnsweredTrial(h, false);      // type 2 incorrect
  runAnsweredTrial(h, false);      // type 1 incorrect
  runAnsweredTrial(h, true);       // type 2 hit

  T3_ANTI_BIAS_AUTO_ENABLE = 1;
  T3_ANTI_BIAS_WIN = 2;            // newest two: type 1 incorrect, type 2 hit
  h.advance(1);
  uint8_t prob = 0;
  check(takeProb1(h, prob) && prob == 90, "window 2 sees type 1 as worse");

  h.cycle(0, AWAY);                // abort the probe trial; it does not enter history
  h.advance(T3_ITI_MS);
  T3_ANTI_BIAS_WIN = 4;            // all four: both types are 1 hit / 1 incorrect
  h.advance(1);
  check(takeProb1(h, prob) && prob == 50, "window 4 sees balanced accuracy");

  T3_ANTI_BIAS_AUTO_ENABLE = sa;
  T3_ANTI_BIAS_WIN = sw;
  T3_ANTI_BIAS_PROB1 = sp;
  T3_MAX_REPEAT = sm;
}

static void test_task3_auto_bias_still_obeys_repeat_cap() {
  printf("task 3: T3_MAX_REPEAT can still force the type after auto probability\n");
  unsigned long sa = T3_ANTI_BIAS_AUTO_ENABLE, sw = T3_ANTI_BIAS_WIN;
  unsigned long sp = T3_ANTI_BIAS_PROB1, sm = T3_MAX_REPEAT;
  T3_ANTI_BIAS_AUTO_ENABLE = 0;
  T3_ANTI_BIAS_WIN = 2;
  T3_ANTI_BIAS_PROB1 = 50;
  T3_MAX_REPEAT = 1;

  setRand({0, 50, 0, 0});
  Harness h(taskById(3));
  runAnsweredTrial(h, false);      // type 1 incorrect
  runAnsweredTrial(h, true);       // type 2 hit
  runAnsweredTrial(h, false);      // type 1 incorrect; last type is now 1

  T3_ANTI_BIAS_AUTO_ENABLE = 1;
  h.advance(1);                    // auto probability asks for type 1, cap forces type 2

  uint8_t prob = 0;
  check(takeProb1(h, prob) && prob == 90, "auto probability was 90 before the cap");
  check(h.task()->state() == T3_SAMPLE2, "but strict alternation forced SAMPLE2");

  T3_ANTI_BIAS_AUTO_ENABLE = sa;
  T3_ANTI_BIAS_WIN = sw;
  T3_ANTI_BIAS_PROB1 = sp;
  T3_MAX_REPEAT = sm;
}

static void test_task3_auto_bias_clear_history_hook() {
  printf("task 3: clearing anti-bias history returns auto probability to cold start\n");
  unsigned long sa = T3_ANTI_BIAS_AUTO_ENABLE, sw = T3_ANTI_BIAS_WIN;
  unsigned long sp = T3_ANTI_BIAS_PROB1, sm = T3_MAX_REPEAT;
  T3_ANTI_BIAS_AUTO_ENABLE = 0;
  T3_ANTI_BIAS_WIN = 2;
  T3_ANTI_BIAS_PROB1 = 50;
  T3_MAX_REPEAT = 100;

  setRand({0, 50, 0, 50});
  Harness h(taskById(3));
  runAnsweredTrial(h, false);      // type 1 incorrect
  runAnsweredTrial(h, true);       // type 2 hit

  T3_ANTI_BIAS_AUTO_ENABLE = 1;
  h.advance(1);
  uint8_t prob = 0;
  check(takeProb1(h, prob) && prob == 90, "history is full and biases type 1");

  h.task()->clearT3AntiBiasHistory();
  h.cycle(0, AWAY);                // abort the probe trial; it does not enter history
  h.advance(T3_ITI_MS);
  h.advance(1);
  check(takeProb1(h, prob) && prob == 50, "cleared history starts auto mode at 50");

  T3_ANTI_BIAS_AUTO_ENABLE = sa;
  T3_ANTI_BIAS_WIN = sw;
  T3_ANTI_BIAS_PROB1 = sp;
  T3_MAX_REPEAT = sm;
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

  setRand({0});
  Harness h3(taskById(3));
  h3.cycle(0, AWAY);
  check(h3.task()->safeToSwitch(), "task 3 in IDLE is switchable");
  h3.cycle();
  check(!h3.task()->safeToSwitch(), "task 3 mid-trial is not");
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

  test_task3_hit_trial();
  test_task3_incorrect();
  test_task3_no_response_deadline();
  test_task3_no_response_by_leaving();
  test_task3_abort_before_gocue();
  test_task3_abort_wins_over_a_simultaneous_lick();
  test_task3_teach_excludes_abort_by_default();
  test_task3_teach_can_include_abort();
  test_task3_early_lick_replays_delay();
  test_task3_aborted_delay_pause_does_not_shorten_next_delay();
  test_task3_early_lick_tolerated();
  test_task3_repeat_cap();
  test_task3_prob1_drives_the_split();
  test_task3_max_repeat_caps_the_bias();
  test_task3_teach_rescues_a_deadline();
  test_task3_teach_rescues_a_walk_off_too();
  test_task3_teach_off_gives_a_plain_no_response();
  test_task3_teach_draw_only_when_enabled();
  test_task3_type2_maps_to_spout2();
  test_task3_simultaneous_licks_score_a_hit();
  test_task3_outcomes_account_for_every_trial();
  test_task3_zero_delay();
  test_task3_prob1_reports_manual_value();
  test_task3_auto_bias_cold_start_is_unbiased();
  test_task3_auto_bias_ignores_non_answer_outcomes();
  test_task3_auto_bias_accuracy_gate();
  test_task3_auto_bias_requires_both_types_in_window();
  test_task3_auto_bias_delta_direction_and_clamp();
  test_task3_auto_bias_window_changes_live();
  test_task3_auto_bias_still_obeys_repeat_cap();
  test_task3_auto_bias_clear_history_hook();

  test_switch_safety();
  test_reset_is_clean();
  test_timeout_does_not_leak();
  test_millis_rollover();

  printf("\n%s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
