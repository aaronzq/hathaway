---
name: analyzing-hathaway-data
description: Use when querying, parsing, interpreting, checking, or analyzing Hathaway behavioral data stored in PostgreSQL/TimescaleDB.
---

# Analyzing Hathaway Data

## Purpose

Use the PostgreSQL service, not Docker's volume files. Treat the database as
read-only unless the user explicitly asks for a data change.

For each analysis task, create a descriptively named subfolder under
`data_analysis/`. Save the analysis scripts, task-specific tests, and generated
results in that subfolder. Include the dependencies and exact run command so
the user can reproduce the results independently. Record the data selection,
time zone, and assumptions needed to reproduce the analysis.

Each task must work independently of every other analysis task folder. Include
task-local code that retrieves its own data from PostgreSQL and reconstructs
any derived tables it needs. Saved inputs may be reused within that task, but
missing inputs must be obtainable from the database, with a documented refresh
command. Do not import scripts or read results from another task folder.

This skill and `references/` are the reusable analysis instructions. They must
explain acquisition, interpretation, and validation without assuming any prior
task scripts or results exist. Never link to or refer readers to analysis task
folders from these instructions. Keep session-specific examples and assumptions
inside their task; keep general algorithms and definitions here or in references.

## Required references

Whenever a plot or figure is requested, first read
[references/figure-making.md](references/figure-making.md) for shared colors
and symbol shapes. Only when the request explicitly includes "raster plot"
(case-insensitive, including "lick raster plot" or "raster plots"), or continues
an already requested raster plot, also read
[references/raster-plot.md](references/raster-plot.md) for alignment, sizes,
physical proportions, panel layout, and outputs. Generic scatter plots and
other figures do not activate the raster layout rules.

Read the reference that matches the work before writing a query or analysis:

- Database connection, tables, time fields, and a Python example:
  [references/database.md](references/database.md)
- Telemetry types, channels, values, and parsing rules:
  [references/telemetry.md](references/telemetry.md)
- Behavioral tasks, trial interpretation, and standard analyses:
  [references/tasks-and-analyses.md](references/tasks-and-analyses.md)

For any trial-level analysis, read all three references. Confirm meanings
against the source files named in each reference if the firmware or schema has
changed since the reference was written.
Follow "Trial boundaries and single-session retrieval" in
`references/tasks-and-analyses.md` and "Parameters used by a trial" in
`references/telemetry.md`. Report trials in chronological order with local
row numbers, include outcomes, and distinguish completed aborts from missing
recording boundaries and abandoned attempts.

## Analysis contract

1. State the sessions, rigs, time range, and task being analyzed.
2. Inspect available `type` values and row counts before assuming data exists.
3. Use `t_us` or `dev_ts` for behavioral timing. Use `host_ts` for approximate
   arrival time and efficient broad time filtering.
4. Interpret `samples` as values that hold until changed and `events` as
   instants. Never count sample rows as event occurrences.
5. Preserve task, trial type, outcome, spout channel, and parameter settings in
   intermediate tables. Do not pool them silently.
6. Report exclusions, missing telemetry, sequence gaps, zero device times, and
   the denominator used for every rate.
7. Save reusable code here. Queries should be parameterized and read-only.

## Quick checks

Before trusting a result, check:

- every requested `session_id` belongs to the expected `rig_id`;
- the active task and parameter history cover the analyzed interval;
- `seq` ordering and gaps do not reveal missing records;
- trial outcome counts agree with the derived trial set;
- event timing uses device time, not serial arrival time;
- task 3 accuracy is `HIT / (HIT + INCORRECT)`, excluding `TEACH`,
  `NO_RESPONSE`, and `ABORT`.

## Common mistakes

- `pc_hathaway_pgdata` is storage backing, not a file/API to query.
- `OUTCOME.channel` is the outcome code; `OUTCOME.value` is the completed-trial
  counter.
- `STATE.channel` is a state index; its meaning depends on the active task.
- Pre-outcome `STATE.value` can be one lower than the matching
  `OUTCOME.value`. Do not join all trial events on the raw value alone.
- A `REWARD` event and its generated `REWARD_COUNT` sample describe the same
  delivery. Counting both doubles rewards.
- `PARAM_TASK` confirms the requested/configured task. A `TASK` sample marks
  when that task actually became active at a safe trial boundary.
- Parameter rows are confirmations, not a complete outbound command log.
  `DUMP`/`GET` can repeat unchanged values, while rejected commands and most
  one-shot action acknowledgements are not stored in the database.
