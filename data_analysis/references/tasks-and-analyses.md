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

## Trial boundaries and single-session retrieval

Use these boundaries for the current firmware. State numbers are task-specific.

| Task | Start | Completed end and outcome |
|---|---|---|
| 1 | Entry to `REFRACTORY` (1), when an accepted lick triggers reward | Entry to `ARMED` (0) after the refractory timeout; `HIT` |
| 2 | Entry to `CUE` (1) | Entry to `REFRACTORY` (3), when the accepted lick triggers reward; `HIT`. The refractory wait follows completion. |
| 3 | Entry to `SAMPLE1` (1) or `SAMPLE2` (2) | Entry to `ITI` (8); outcome is `HIT`, `INCORRECT`, `NO_RESPONSE`, `ABORT`, or `TEACH`. The ITI wait follows completion. |
| 4 | Entry to `WAIT_LICK` (1) | Entry to `REFRACTORY` (2), when the accepted lick triggers reward and tone; `HIT`. The refractory wait follows completion. |

For a requested session:

1. Confirm the rig and active task. Read `STATE`, `OUTCOME`, and `PARAM_*`
   from `events_dev`, and `TASK` from `samples_dev`. If TASK observations are
   absent, use an explicit user-provided task identification and disclose it;
   do not substitute PARAM_TASK as proof of activation.
2. Within the same session and rig, order by `(t_us, seq)`. Pair a start with
   its task-specific end before another start, task switch, or reset. Task-3
   `EARLY_PAUSE` and repeated `DELAY` entries remain within the same trial.
3. The firmware emits the completion STATE before OUTCOME in the same cycle,
   with the same device timestamp and completed counter. Match that following
   OUTCOME to the end; validate its timestamp, sequence order and counter.
   Read the outcome code from OUTCOME.channel, not OUTCOME.value. If a match
   is absent or ambiguous, mark the outcome unknown rather than inferring it.
4. Number report rows 1, 2, 3, ... in chronological order within the session.
   Retain firmware counters only as diagnostic fields. They are not globally
   unique trial IDs. A counter decrease is a reset boundary: do not connect
   a start before it to an end after it.
5. Keep missing-start or missing-end fragments explicitly marked; never
   invent a boundary or outcome. An observed task-3 ABORT with sample and ITI
   boundaries is a complete trial, not a recording fragment. In tasks 2/4,
   return to IDLE before completion ends an abandoned attempt without a
   firmware outcome. Keep it separate from completed trials and do not label
   it OUTCOME_ABORT. Leaving during refractory does not undo their completion.
6. Retrieve parameters using the rules in `telemetry.md`. A start snapshot
   describes confirmed settings, not necessarily every value actually used.
   Retain trial type, outcome, spout, boundaries, and relevant parameter-use
   information in intermediate results. Verify starts, ends and outcomes
   reconcile, reporting fragments and unmatched records separately.

An incomplete trial can still have a known outcome when only its start is
missing. Its trial-start parameters remain unknown.

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

Classify trials as with magnet (`1`) or without magnet (`0`) using the
MAGNET sample value in effect at entry to `GOCUE` (STATE channel 4).
Use the latest valid MAGNET sample at or before that entry in `(t_us, seq)`
order, within the same session and rig; samples hold until changed. Do not
substitute magnet state at sample entry, reward, or any other trial time.
Show this binary indicator in every trial-table row instead of magnet
duration/grace settings or hold histories. Keep raw settings in saved data.
A complete trial that never enters GOCUE is `N/A`, not `0`. Missing go-cue
coverage in a recording fragment, or no preceding valid MAGNET sample, is
`Unknown`. Keep these rows separate from the two classified groups.

Include a `Sample` column: `1` for SAMPLE1 entry, `2` for SAMPLE2 entry;
use `Unknown` if the sample entry is missing, without inferring it from outcome.

Include an `Early lick` indicator: `1` if either spout has a LICK event while
the current state is DELAY (3), otherwise `0` for a fully observed trial that
entered DELAY. Order LICK and STATE events by `(t_us, seq)`; a state holds
until the next transition. A complete trial that never enters DELAY is `N/A`.
For recording fragments, positive evidence can establish `1`; lack of evidence
is `Unknown`, not `0` or `N/A`. Do not count licks during sample, GOCUE, or
EARLY_PAUSE as delay licks.

When T3_EARLY_LICK_PUNISH is enabled, a delay lick normally causes EARLY_PAUSE
(9) followed by a replay of DELAY. EARLY_PAUSE is positive evidence even if
the trial aborts before the replay begins. Replay is not a universal substitute
for LICK events: punishment can be disabled, and leaving position takes
priority over early-lick punishment. Use raw delay licks as the primary rule
and state transitions as confirmation; report missing telemetry or disagreement.

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
