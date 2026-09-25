# Telemetry interpretation

The current authoritative registry is `TELEM_TABLE` in `hathaway.ino`.
`serial_logging_test/pc/ingest.py` learns sample/event kinds from firmware
`#DEF` messages and uses `KNOWN_KINDS` only as a fallback.

## Samples

Samples represent a level that holds until the next update.

| Type | Channel | Value |
|---|---:|---|
| `WEIGHT` | 1 | Current load-cell reading |
| `POSITION` | 1 | `1` in position, `0` out of position |
| `MAGNET` | 1 | Magnet state, normally `0`/`1` |
| `TONE` | 1 | Frequency in Hz; `0` means silent |
| `TASK` | task ID | Active task ID, also repeated in `value` |
| `T3_PROB1` | 1 | Effective probability, percent, used for a task-3 type-1 draw |
| `RAIL_POS` | 1 | Rail position in millimetres after a completed move |
| `REWARD_COUNT` | spout 1 or 2 | Cumulative reward count generated from each `REWARD` event |

Historical data may contain older names such as `LOADCELL`. Always inspect
distinct types before querying.

## Events

Events represent instants.

| Type | Channel | Value |
|---|---|---|
| `LICK` | Spout 1 or 2 | Usually `1` |
| `REWARD` | Spout 1 or 2 | That spout's cumulative reward number |
| `STATE` | Task-specific state index | Trial counter at state entry |
| `OUTCOME` | Outcome code | Completed-trial counter |
| `RAIL_CMD` | Command disposition | Requested millimetres |
| `PARAM_<NAME>` | 0 | Parameter value confirmed by the rig |

Each `REWARD` produces both an event and a `REWARD_COUNT` sample. Use the event
for reward counts and timing; use the sample for step plots.

## Outcome codes

Outcome codes are append-only because they are stored in `OUTCOME.channel`.

| Code | Name | Interpretation |
|---:|---|---|
| 0 | `HIT` | Correct response; tasks 1, 2, and 4 report their completed trials as hits |
| 1 | `INCORRECT` | Task 3 responded on the wrong spout |
| 2 | `NO_RESPONSE` | Task-3 response window ended without an answer |
| 3 | `ABORT` | Task-3 trial ended before it could be answered |
| 4 | `TEACH` | Task-3 failed trial rescued with water; not a hit |

Task-3 discrimination accuracy is `HIT / (HIT + INCORRECT)`. State the
denominator explicitly for response, completion, teaching, and abort rates.

## Rail command codes

| Code | Meaning |
|---:|---|
| 1 | Accepted manual move |
| 2 | Refused because already moving |
| 3 | Position set as home/zero; no movement |
| 4 | Move stopped |
| 5 | Backstop timeout |
| 6 | Rail unavailable |
| 7 | Accepted automatic task-1 retraction |

## Parameters

The control panel writes firmware acknowledgements as event types named
`PARAM_<NAME>`. Their value is the setting confirmed by the rig at that device
timestamp. A row can be an acknowledgement of `SET` or a current-value snapshot
produced by `DUMP`/`GET`; the database does not label which caused it. Therefore
do not count parameter rows as changes. Compare each value with the preceding
value when identifying actual changes.

Use the last parameter event at or before a trial in `(t_us, seq)` order, within
the same session and rig. Retrieve `PARAM_<NAME>` records from `events_dev`
and perform this lookup within the analysis at the reconstructed trial start. A confirmation later in sequence at the same timestamp does not
apply retroactively. Missing starts cannot support a parameter snapshot. Older
parameter records may have `t_us = 0`; they cannot be placed reliably within a
session and are excluded. Values are confirmed settings, not proof that a
latched task parameter changed mid-trial. Requested `PARAM_TASK` remains a
parameter and does not establish active task identity.

### Parameters used by a trial

The analysis goal is the values used, not a command-change audit. When a value
is constant over all relevant read points, its confirmed value can be used.
When it varies, inspect where the firmware reads or latches it; do not assign
one trial-start value to the whole trial without checking. Missing coverage
or ambiguous ordering must be reported as unknown, not filled from defaults.

For task 3, `tasks.cpp` reads sample settings at sample entry, delay duration
at every DELAY entry (including replays), go-cue settings at GOCUE entry,
response duration at RESPONSE entry, consumption duration at REWARD entry,
punishment duration at PUNISH entry, and early-pause duration at EARLY_PAUSE
entry. Teaching settings are evaluated at the rescue decision. Valve duration
is read when the reward action executes. ITI duration is read at ITI entry
and describes the following inter-trial wait. Task-selection probability can
be adjusted by anti-bias; use `T3_PROB1` telemetry for the effective draw
probability rather than equating it with PARAM_T3_ANTI_BIAS_PROB1.

`magnet.cpp` latches MAG_FIX_DURATION and MAG_GRACE_MS at magnet activation;
their setters change defaults for the next activation, not the running hold.
Use the parameter values before the activation, with MAGNET transitions and
manual-start telemetry where available. An initial MAGNET=1 snapshot alone
does not establish when the hold started or which duration it latched.
SCALE_HIGH_THRESH and SCALE_LOW_THRESH are read for each weight measurement;
if they vary during a trial, there is no single threshold for that whole trial.

Present values at their relevant phases or intervals when necessary. Keep
configured settings and reconstructed values used clearly distinguished;
report-only compression of unchanged values must not discard full parameter
information from the saved analysis results.

## PC-to-rig commands and database visibility

The control panel sends commands over serial. The database records selected
firmware responses and telemetry, not the raw outbound command stream.

| PC command | What the database receives |
|---|---|
| `SET <NAME> <VALUE>` | If accepted, `PARAM_<NAME>` confirms the stored value. A rejected command returns `#ERR` to the control-panel log but creates no database row. |
| `SET TASK <ID>` | `PARAM_TASK` confirms the requested/configured task ID. The switch is deferred until the running task reaches a safe trial boundary. A later `TASK` sample marks when the new task actually became active. |
| `DUMP` or `GET` | Re-emits the schema and all current parameters. The control panel stores the resulting `PARAM_<NAME>` rows, even when values did not change. There is no database row identifying the `DUMP`/`GET` command itself. |
| `TARE` | Returns `#TARE ok` or `#ERR` to the control-panel log. There is no dedicated tare-command database event. |
| `RAIL_MOVE_MM` / `RAIL_MOVE_PULSES` | Produces `RAIL_CMD` telemetry describing accepted, refused, unavailable, or timed-out execution; a completed move produces `RAIL_POS`. |
| `RAIL_SET_HOME` / `RAIL_STOP` | Produces the corresponding `RAIL_CMD` disposition and an updated `RAIL_POS` when applicable. |

For task-based analysis, use `TASK` samples from `samples_dev` to determine the
active task over time. Use `PARAM_TASK` only to show the requested configuration.
Never assume that their timestamps are identical. If a recording starts after
task activation, task identity may be missing; report this rather than guessing.

For audit questions such as “exactly which buttons did the operator press,”
state that the database is incomplete: repeated snapshots, rejected `SET`
commands, `TARE`, and generic action acknowledgements cannot be reconstructed
as a complete command history.

## Parsing safeguards

- Never infer a telemetry kind from its numeric shape. Read the database
  `type`, the current firmware table, and schema announcements.
- Partition by `session_id` and `rig_id` before interpreting `seq` or trials.
- Keep spout numbers as `1` and `2`. Physical left/right mapping is rig-specific
  and is not defined by the firmware.
- Reconstruct held sample values with an as-of/last-observation lookup, not by
  assuming a sample exists at every event timestamp.
