#include "async_reward.h"
#include "lick.h"
#include "clock.h"
#include "switch.h"
#include "magnet.h"
#include "grating.h"
#include "buzzer.h"


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

const unsigned long REWARD_DURATION = 30;
const unsigned long REWARD_INTERVAL = 5000;