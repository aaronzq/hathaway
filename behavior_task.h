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
//   1 = LICK_REWARD  free licking, either spout, shared refractory gate
//   2 = CUED_REWARD  first lick -> go cue -> water -> gate -> next trial
// The switch is DEFERRED until the running task reaches a trial boundary, so
// setting this mid-trial is safe. The TASK telemetry line marks the cycle on
// which it actually took effect.
unsigned long TASK = 1;

// Per-spout manual cut-off, for temporarily taking a spout out of use without
// reflashing. A disabled spout still reports its licks -- it just cannot earn
// water, and its licks do not touch the reward gate.
unsigned long SPOUT1_ENABLE = 1;
unsigned long SPOUT2_ENABLE = 1;

// --- task 2 ----------------------------------------------------------------
unsigned long T2_CUE_FREQ     = 6000;   // go cue frequency, Hz
unsigned long T2_CUE_DUR      = 100;    // go cue duration, ms
unsigned long T2_CUE_TO_WATER = 100;    // cue ONSET -> water, ms (100 = at cue
                                        // offset; 0 = at cue onset)