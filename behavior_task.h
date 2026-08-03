#include "async_reward.h"
#include "lick.h"
#include "clock.h"
#include "switch.h"
#include "magnet.h"
#include "grating.h"
#include "buzzer.h"
#include "HX711.h"


// >>> CHANGE THIS to match the physical rig before uploading. <<<
// It is attached to every serial message so the database can tell rigs apart.
const int RIG_ID = 1;


// TFT display
const float ANGLES[]    = {0, 45, 90, 135};
const float CONTRASTS[] = {0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
const int   NUM_ANGLES    = sizeof(ANGLES) / sizeof(ANGLES[0]);
const int   NUM_CONTRASTS = sizeof(CONTRASTS) / sizeof(CONTRASTS[0]);

const float PERIOD = 45.0f;  // grating period, px
const float SPEED  = 160.0f; // drift speed, px/s


// Buzzer
const unsigned int FREQS[] = {3000, 6000, 9000, 12000};
const int   NUM_FREQS = sizeof(FREQS) / sizeof(FREQS[0]);

// Runtime-tunable over serial via "SET <NAME> <VALUE>" (see hathaway.ino).
// Mutable (not const) so the command handler can update them live. This header
// is included only by hathaway.ino, so single-definition is fine.
unsigned long REWARD_DURATION1 = 50;    // spout 1 solenoid open time, ms
unsigned long REWARD_DURATION2 = 42;    // spout 2 solenoid open time, ms
unsigned long REWARD_INTERVAL1 = 3000;  // refractory after a spout-1 reward, ms
unsigned long REWARD_INTERVAL2 = 3000;  // refractory after a spout-2 reward, ms
unsigned long MAG_FIX_DURATION = 5000;
float SCALE_HIGH_THRESH = 40.0;
float SCALE_LOW_THRESH  = 10.0;

// --- task selection --------------------------------------------------------
// Which state machine is running. See TASK_TABLE in tasks.cpp:
//   1 = LICK_REWARD     free licking, either spout, shared refractory gate
//   2 = CUED_REWARD     in position -> go cue -> lick -> water -> gate -> cue...,
//                       one spout at a time (T2_N1 then T2_N2 rewards)
//   3 = DISCRIMINATION  in position -> sample tone -> delay -> go cue ->
//                       lick spout 1 or 2 -> water or timeout
// The switch is DEFERRED until the running task reaches a trial boundary, so
// setting this mid-trial is safe. The TASK telemetry line marks the cycle on
// which it actually took effect.
unsigned long TASK = 1;

// --- task 1 ----------------------------------------------------------------
// Per-spout manual cut-off, for temporarily taking a spout out of use without
// reflashing. A disabled spout still reports its licks -- it just cannot earn
// water, and its licks do not touch the reward gate.
//
// TASK 1 ONLY, which is why the names say so. Task 2 ignores these entirely
// and takes a spout out of use by setting its block length (T2_Nn) to zero.
unsigned long T1_SPOUT1_ENABLE = 1;
unsigned long T1_SPOUT2_ENABLE = 1;

// --- task 2 ----------------------------------------------------------------
unsigned long T2_CUE_FREQ     = 6000;   // go cue frequency, Hz
unsigned long T2_CUE_DUR      = 100;    // go cue duration, ms
unsigned long T2_CUE_TO_WATER = 100;    // cue ONSET -> licks count, ms. Licks
                                        // inside this window earn nothing; the
                                        // first lick after it delivers water.
                                        // 100 = the end of the cue; 0 = licks
                                        // count from cue onset.
// Block lengths for the spout alternation: T2_N1 rewards at spout 1, then
// T2_N2 at spout 2, repeating. The blocked spout's licks are logged and
// ignored.
//
// Zero is how task 2 retires a spout: T2_N1 = 0 puts every trial on spout 2.
// Both zero leaves no spout live at all -- the mouse gets one cue on arrival
// and then nothing, which is the task's way of saying "stop".
unsigned long T2_N1 = 10;
unsigned long T2_N2 = 10;

// --- task 3 ----------------------------------------------------------------
// Two-tone discrimination with a delay. Trial type 1 plays T3_SAMPLE_FREQ1 and
// is answered on spout 1; type 2 plays T3_SAMPLE_FREQ2 and is answered on
// spout 2. Water uses REWARD_DURATION1 / REWARD_DURATION2 above -- task 3 adds
// no valve time of its own.
unsigned long T3_SAMPLE_FREQ1 = 12000;  // sample tone for trial type 1, Hz
unsigned long T3_SAMPLE_FREQ2 = 3000;   // sample tone for trial type 2, Hz

// The sample is a pulse train, not a steady tone: T3_N_PULSES bursts of
// T3_PULSE_MS separated by T3_GAP_MS, with no trailing gap. The defaults give
// 3x150 + 2x100 = 650 ms.
//
// NOTE ON LOUDNESS. The buzzer is driven at a fixed 50% duty cycle, so the
// firmware cannot set sound pressure per frequency. Matching levels across
// 3 / 6 / 12 kHz is a job for the amplifier or an attenuator, not for a
// parameter here -- there is deliberately no T3_..._DB to give the false
// impression that it can be done in software.
unsigned long T3_PULSE_MS = 150;        // one pulse, ms
unsigned long T3_GAP_MS   = 100;        // silence between pulses, ms
unsigned long T3_N_PULSES = 3;          // pulses per sample

// The working-memory delay. Raised through training: 0 -> 300 -> 600 -> 1200.
unsigned long T3_DELAY_MS = 1200;

unsigned long T3_CUE_FREQ = 6000;       // go cue frequency, Hz
unsigned long T3_CUE_DUR  = 100;        // go cue duration, ms. The response
                                        // window opens when the cue ends.

unsigned long T3_RESPONSE_MS = 1500;    // how long an answer is accepted for
unsigned long T3_CONSUME_MS  = 1500;    // drinking time after a hit, from the
                                        // instant the valve is asked to open
unsigned long T3_PUNISH_MS   = 2000;    // timeout after a wrong spout, ms
unsigned long T3_ITI_MS      = 250;     // inter-trial interval, ms. Served
                                        // after every trial, aborts included.

// At most this many consecutive trials of the same type; the next one is then
// forced to the other type. 1 gives strict alternation, which is predictable
// and therefore learnable -- 3 or 4 is the usual choice.
unsigned long T3_MAX_REPEAT = 3;

// What a lick during the sample or the delay does. 1 = replay the trial from
// its sample tone, same trial type. 0 = log the lick and ignore it, which is
// how an animal that cannot yet withhold is trained up.
unsigned long T3_EARLY_LICK_PUNISH = 1;

// The random source task 3 draws its trial type from. Defined here so the task
// layer itself stays free of hardware: tools/task_test.cpp defines its own
// version returning a scripted sequence, which is what makes a trial sequence
// reproducible under test.
uint32_t task_rand32() { return esp_random(); }