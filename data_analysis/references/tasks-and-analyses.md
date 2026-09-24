# Behavioral tasks and standard analyses

The authoritative behavior is documented in `tasks.h`, implemented in
`tasks.cpp`, and configured in `behavior_task.h`. Confirm those files when
firmware changes.

## Trial numbering rule

`STATE.value` is the task's completed-trial counter when that state is entered.
The counter increments at a task-specific point. `OUTCOME.value` is the counter
after completion. Therefore, pre-completion states for the trial whose outcome
is `N` commonly carry `N - 1`; the completion state can carry `N`.

Build trials from ordered state intervals and the following `OUTCOME`, using
`t_us` then `seq`. Do not join every state, lick, and outcome only on equal
numeric `value`.

## Task 1: `LICK_REWARD`

Either enabled spout rewards a lick. Both share a refractory gate. Disabled
spouts still log licks but cannot reward. A completed trial has outcome `HIT`.

| State | Code |
|---|---:|
| `ARMED` | 0 |
| `REFRACTORY` | 1 |

The reward occurs on entry to `REFRACTORY`; the trial/outcome is booked when
the refractory interval ends and the task returns to `ARMED`.

Standard analyses:

- licks, rewards, and inter-event intervals by spout;
- reward-producing licks versus unrewarded/refractory licks;
- position state at reward time;
- rewards per minute and cumulative rewards;
- rail position, automatic rail moves, and recent in-position reward rate;
- parameter settings and changes during the session.

## Task 2: `CUED_REWARD`

While in position, a cue starts a trial. After the cue-to-water delay, the next
lick on the active spout delivers water. Spout 1 and 2 alternate in configured
reward blocks. The inactive spout's licks are logged and ignored. Leaving
position aborts the in-progress sequence but does not create a failure outcome.
Completed rewarded trials are `HIT`.

| State | Code |
|---|---:|
| `IDLE` | 0 |
| `CUE` | 1 |
| `WAIT_LICK` | 2 |
| `REFRACTORY` | 3 |

Standard analyses:

- rewarded trials and rewards by active spout/block;
- latency from `WAIT_LICK` entry to the rewarded lick;
- ignored licks during cue delay or on the inactive spout;
- time in/out of position and interrupted attempts;
- reward rate, block progression, and parameter history.

## Task 3: `DISCRIMINATION`

Type 1 uses sample tone 1 and correct spout 1; type 2 uses sample tone 2 and
correct spout 2. The animal hears a pulsed sample, waits through a delay, hears
a go cue, and answers during the response window. Teaching may rescue enabled
failure types with water but is recorded as `TEACH`, never `HIT`.

| State | Code |
|---|---:|
| `IDLE` | 0 |
| `SAMPLE1` | 1 |
| `SAMPLE2` | 2 |
| `DELAY` | 3 |
| `GOCUE` | 4 |
| `RESPONSE` | 5 |
| `REWARD` | 6 |
| `PUNISH` | 7 |
| `ITI` | 8 |
| `EARLY_PAUSE` | 9 |

Derive trial type from `SAMPLE1`/`SAMPLE2`, not from outcome. Derive reaction
time from `RESPONSE` entry to the first response lick. Do not use outcome time:
the outcome is emitted on entry to `ITI`, after reward consumption or punishment
when applicable. Licks during sample and go cue are logged but ignored; delay
licks may be ignored or cause `EARLY_PAUSE` depending on the parameter.

Standard analyses:

- outcome counts/rates, keeping all five outcomes separate;
- discrimination accuracy: `HIT / (HIT + INCORRECT)`;
- accuracy, response bias, and reaction time by trial type and spout;
- no-response, abort, teach, and early-lick rates;
- confusion matrix using trial type and first response spout;
- trial-type balance, repeat runs, and `T3_PROB1`/anti-bias history;
- performance before/after parameter changes;
- sample, delay, response, reward/punish, and inter-trial interval durations.

## Task 4: `REWARD_TONE`

While in position, a lick on the active spout delivers water and starts the
reward-associated tone in the same control cycle. Spout blocks alternate using
task-4 settings. Completed rewarded trials are `HIT`.

| State | Code |
|---|---:|
| `IDLE` | 0 |
| `WAIT_LICK` | 1 |
| `REFRACTORY` | 2 |

Standard analyses:

- rewards and reward-triggering licks by spout/block;
- alignment of reward and tone onset;
- tone duration/frequency and missing tone-off records;
- licks during the refractory interval;
- time in position, reward rate, and parameter history.

## Standard session and data-quality report

For every task, begin with:

1. Session ID, rig, note, host start/end, device-time span, and active tasks.
2. Counts by table, telemetry type, and channel.
3. `seq` gaps, duplicate sequence numbers, zero `t_us`, and unexpected types.
4. Parameter values at start plus every within-session change.
5. Trial/outcome totals and task-specific denominators.
6. Sensor/state coverage needed by the requested analysis.

When an interpretation depends on an assumption—physical spout side, weight
units, subject identity, or intended parameter regime—state it and ask rather
than inventing it.
