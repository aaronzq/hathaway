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

Use the last parameter event at or before a trial. `trial_params` performs this
lookup for trial starts. Older parameter records may have `t_us = 0`; they
cannot be placed reliably within a session.

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

For task-based analysis, use `TASK` samples to determine the active task over
time. Use `PARAM_TASK` only to show the requested configuration. Never assume
that their timestamps are identical.

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
