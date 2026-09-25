# Session parameter history

Snapshot captured: 2026-09-25 10:17:05 Pacific time.
Scope: sessions in the command-related list from September 10, 2026 onward; keep durations of at least two hours.
All displayed times are Pacific (America/Los_Angeles). Durations are elapsed session spans, not time actively training.
For sessions without an end time, the last recorded row gives a minimum duration; it does not prove the session is still running.

| Session | Rig | Start | End / last record | Duration | Active task IDs | Note |
|---|---|---|---|---|---|---|
| 74 | 1 | 2026-09-10 12:00:29 | 2026-09-11 19:58:35 | 31h 58m | not recorded | — |
| 75 | 1 | 2026-09-12 12:33:43 | 2026-09-13 20:23:08 | 31h 49m | not recorded | — |
| 76 | 1 | 2026-09-14 11:29:36 | 2026-09-14 20:01:18 | 8h 31m | not recorded | — |
| 77 | 1 | 2026-09-15 07:49:06 | 2026-09-16 20:45:31 | 36h 56m | not recorded | — |
| 78 | 1 | 2026-09-17 10:59:53 | 2026-09-18 19:52:48 | 32h 52m | not recorded | — |
| 79 | 1 | 2026-09-19 14:49:36 | 2026-09-20 22:58:42 | 32h 09m | not recorded | — |
| 80 | 1 | 2026-09-21 15:03:01 | 2026-09-22 19:13:01 | 28h 10m | not recorded | — |
| 81 | 1 | 2026-09-23 11:26:10 | 2026-09-23 19:15:15 | 7h 49m | not recorded | — |
| 116 | 1 | 2026-09-23 19:34:37 | 2026-09-24 19:20:53 | 23h 46m | not recorded | — |

Excluded: session 82 (17.1 seconds); session 115 (164.2 seconds).

## How to read the settings

The first value recorded for each parameter is the initial observed setting, not proof of its value before that record. All numeric values retain firmware units.
Later sessions compare their initial observed values with the previous retained session’s final observed values for the same rig. Differences may reflect resets or changes in excluded sessions; they are not attributed to a command in the retained session.
Within-session tables list every observed value change. Unchanged confirmations are omitted. PARAM records may come from SET, startup, DUMP, or GET; this is not a complete outbound command history.
TASK parameter values describe configuration; active task IDs above come from TASK samples.

## Session 74

### Initial observed parameters

| Parameter | Value | First recorded |
|---|---|---|
| `BUZ_PULSE_WIDTH` | 25 | 2026-09-10 12:00:29 |
| `LICK_DEBOUNCE_TIME` | 20 | 2026-09-10 12:00:29 |
| `MAG_FIX_DURATION` | 8000 | 2026-09-10 12:00:29 |
| `MAG_GRACE_MS` | 2000 | 2026-09-10 12:00:29 |
| `REWARD_DURATION1` | 100 | 2026-09-10 12:00:29 |
| `REWARD_DURATION2` | 94 | 2026-09-10 12:00:29 |
| `REWARD_INTERVAL1` | 3000 | 2026-09-10 12:00:29 |
| `REWARD_INTERVAL2` | 3000 | 2026-09-10 12:00:29 |
| `SCALE_HIGH_THRESH` | 40 | 2026-09-10 12:00:29 |
| `SCALE_LOW_THRESH` | 10 | 2026-09-10 12:00:29 |
| `T1_SPOUT1_ENABLE` | 1 | 2026-09-10 12:00:29 |
| `T1_SPOUT2_ENABLE` | 1 | 2026-09-10 12:00:29 |
| `T2_CUE_DUR` | 100 | 2026-09-10 12:00:29 |
| `T2_CUE_FREQ` | 6000 | 2026-09-10 12:00:29 |
| `T2_CUE_TO_WATER` | 100 | 2026-09-10 12:00:29 |
| `T2_N1` | 3 | 2026-09-10 12:00:29 |
| `T2_N2` | 3 | 2026-09-10 12:00:29 |
| `T3_ANTI_BIAS_ACC_THRESH` | 75 | 2026-09-10 12:00:29 |
| `T3_ANTI_BIAS_AUTO_ENABLE` | 1 | 2026-09-10 12:00:29 |
| `T3_ANTI_BIAS_PROB1` | 50 | 2026-09-10 12:00:29 |
| `T3_ANTI_BIAS_WIN` | 20 | 2026-09-10 12:00:29 |
| `T3_CONSUME_MS` | 1500 | 2026-09-10 12:00:29 |
| `T3_CUE_DUR` | 100 | 2026-09-10 12:00:29 |
| `T3_CUE_FREQ` | 6000 | 2026-09-10 12:00:29 |
| `T3_DELAY_MS` | 300 | 2026-09-10 12:00:29 |
| `T3_EARLY_LICK_PAUSE_MS` | 100 | 2026-09-10 12:00:29 |
| `T3_EARLY_LICK_PUNISH` | 1 | 2026-09-10 12:00:29 |
| `T3_GAP_MS` | 100 | 2026-09-10 12:00:29 |
| `T3_ITI_MS` | 250 | 2026-09-10 12:00:29 |
| `T3_MAX_REPEAT` | 6 | 2026-09-10 12:00:29 |
| `T3_N_PULSES` | 3 | 2026-09-10 12:00:29 |
| `T3_PULSE_MS` | 150 | 2026-09-10 12:00:29 |
| `T3_PUNISH_MS` | 8000 | 2026-09-10 12:00:29 |
| `T3_RESPONSE_MS` | 1500 | 2026-09-10 12:00:29 |
| `T3_SAMPLE_FREQ1` | 12000 | 2026-09-10 12:00:29 |
| `T3_SAMPLE_FREQ2` | 3000 | 2026-09-10 12:00:29 |
| `T3_TEACH_INCLUDE_ABORT` | 0 | 2026-09-10 12:00:29 |
| `T3_TEACH_PROB` | 50 | 2026-09-10 12:00:29 |
| `TASK` | 3 | 2026-09-10 12:00:29 |

### Changes during the session

| Time | Parameter | Before | After |
|---|---|---|---|
| 2026-09-10 12:01:51 | `REWARD_DURATION1` | 100 | 133 |
| 2026-09-10 12:01:52 | `REWARD_DURATION2` | 94 | 128 |
| 2026-09-10 17:10:20 | `T3_PUNISH_MS` | 8000 | 2000 |
| 2026-09-10 17:13:49 | `T3_DELAY_MS` | 300 | 100 |
| 2026-09-10 17:17:25 | `T3_PUNISH_MS` | 2000 | 3000 |
| 2026-09-10 17:18:31 | `MAG_FIX_DURATION` | 8000 | 10000 |
| 2026-09-10 17:19:32 | `REWARD_DURATION1` | 133 | 100 |
| 2026-09-10 17:19:32 | `REWARD_DURATION2` | 128 | 94 |
| 2026-09-10 17:21:08 | `T3_PUNISH_MS` | 3000 | 8000 |
| 2026-09-10 17:21:23 | `T3_PUNISH_MS` | 8000 | 3000 |

## Session 75

### Initial differences from session 74 final values

| Parameter | Previous final | Current initial |
|---|---|---|
| No observed differences | — | — |

### Changes during the session

| Time | Parameter | Before | After |
|---|---|---|---|
| 2026-09-12 15:42:38 | `MAG_FIX_DURATION` | 10000 | 12000 |
| 2026-09-13 13:30:48 | `T3_DELAY_MS` | 100 | 200 |

## Session 76

### Initial differences from session 75 final values

| Parameter | Previous final | Current initial |
|---|---|---|
| No observed differences | — | — |

### Changes during the session

| Time | Parameter | Before | After |
|---|---|---|---|
| 2026-09-14 11:47:50 | `T3_TEACH_PROB` | 50 | 25 |
| 2026-09-14 11:51:25 | `T3_TEACH_PROB` | 25 | 50 |

## Session 77

### Initial differences from session 76 final values

| Parameter | Previous final | Current initial |
|---|---|---|
| No observed differences | — | — |

### Changes during the session

| Time | Parameter | Before | After |
|---|---|---|---|
| 2026-09-15 20:34:19 | `T3_DELAY_MS` | 200 | 300 |

## Session 78

### Initial differences from session 77 final values

| Parameter | Previous final | Current initial |
|---|---|---|
| No observed differences | — | — |

### Changes during the session

| Time | Parameter | Before | After |
|---|---|---|---|
| 2026-09-17 14:02:01 | `T3_DELAY_MS` | 300 | 400 |
| 2026-09-18 12:58:11 | `T3_DELAY_MS` | 400 | 600 |

## Session 79

### Initial differences from session 78 final values

| Parameter | Previous final | Current initial |
|---|---|---|
| No observed differences | — | — |

### Changes during the session

| Time | Parameter | Before | After |
|---|---|---|---|
| 2026-09-19 18:14:17 | `MAG_FIX_DURATION` | 12000 | 15000 |
| 2026-09-19 18:31:00 | `MAG_FIX_DURATION` | 15000 | 30000 |
| 2026-09-19 18:32:17 | `MAG_FIX_DURATION` | 30000 | 15000 |
| 2026-09-19 18:32:21 | `SCALE_HIGH_THRESH` | 40 | 45 |
| 2026-09-19 19:35:47 | `MAG_FIX_DURATION` | 15000 | 20000 |
| 2026-09-19 19:40:37 | `SCALE_HIGH_THRESH` | 45 | 48 |
| 2026-09-19 19:41:21 | `SCALE_HIGH_THRESH` | 48 | 45 |
| 2026-09-19 19:46:05 | `MAG_FIX_DURATION` | 20000 | 30000 |
| 2026-09-20 11:39:03 | `MAG_FIX_DURATION` | 30000 | 45000 |
| 2026-09-20 15:55:47 | `MAG_FIX_DURATION` | 45000 | 60000 |

## Session 80

### Initial differences from session 79 final values

| Parameter | Previous final | Current initial |
|---|---|---|
| No observed differences | — | — |

### Changes during the session

| Time | Parameter | Before | After |
|---|---|---|---|
| 2026-09-21 17:32:28 | `T3_DELAY_MS` | 600 | 800 |
| 2026-09-21 19:43:59 | `T3_DELAY_MS` | 800 | 1000 |
| 2026-09-22 16:47:43 | `SCALE_HIGH_THRESH` | 45 | 48 |

## Session 81

### Initial differences from session 80 final values

| Parameter | Previous final | Current initial |
|---|---|---|
| No observed differences | — | — |

### Changes during the session

| Time | Parameter | Before | After |
|---|---|---|---|
| No observed changes | — | — | — |

## Session 116

### Initial differences from session 81 final values

| Parameter | Previous final | Current initial |
|---|---|---|
| `T1_RAIL_AUTO_ENABLE` | not previously recorded | 0 |
| `T1_RAIL_MIN_POS_PCT` | not previously recorded | 90 |
| `T1_RAIL_STEP` | not previously recorded | 0 |
| `T1_RAIL_WIN` | not previously recorded | 20 |
| `T3_TEACH_INCLUDE_INCORRECT` | not previously recorded | 0 |
| `T3_TEACH_INCLUDE_NO_RESPONSE` | not previously recorded | 1 |
| `T4_CUE_DUR` | not previously recorded | 100 |
| `T4_CUE_FREQ` | not previously recorded | 6000 |
| `T4_N1` | not previously recorded | 3 |
| `T4_N2` | not previously recorded | 3 |

### Changes during the session

| Time | Parameter | Before | After |
|---|---|---|---|
| 2026-09-23 19:35:20 | `MAG_FIX_DURATION` | 60000 | 80000 |
| 2026-09-23 19:42:03 | `MAG_FIX_DURATION` | 80000 | 60000 |

## Data checks

Counts and all parameter confirmations are saved in snapshot.json. No trial rates were calculated.
Sequence gaps below count absent sequence numbers between the minimum and maximum across both tables. Duplicate counts are extra rows sharing a sequence number; neither metric alone identifies the cause.

| Session | Rows | Sequence gaps | Duplicate sequence rows | Zero device times | Device-time span (UTC) |
|---|---|---|---|---|---|
| 74 | 1248937 | 0 | 0 | 0 | 2026-09-10T19:00:29.220058+00:00 to 2026-09-12T02:58:35.601058+00:00 |
| 75 | 1233487 | 0 | 0 | 0 | 2026-09-12T19:33:43.073119+00:00 to 2026-09-14T03:23:07.885119+00:00 |
| 76 | 339756 | 0 | 0 | 0 | 2026-09-14T18:29:36.774517+00:00 to 2026-09-15T03:01:18.315517+00:00 |
| 77 | 1418344 | 0 | 0 | 0 | 2026-09-15T14:49:06.418792+00:00 to 2026-09-17T03:45:24.636792+00:00 |
| 78 | 1260066 | 0 | 0 | 0 | 2026-09-17T17:59:53.349786+00:00 to 2026-09-19T02:52:47.984786+00:00 |
| 79 | 1230006 | 0 | 0 | 0 | 2026-09-19T21:49:36.284198+00:00 to 2026-09-21T05:58:41.844198+00:00 |
| 80 | 1083434 | 0 | 0 | 0 | 2026-09-21T22:03:01.725625+00:00 to 2026-09-23T02:13:01.579625+00:00 |
| 81 | 309318 | 0 | 0 | 0 | 2026-09-23T18:26:10.078290+00:00 to 2026-09-24T02:15:15.319290+00:00 |
| 116 | 904286 | 0 | 0 | 0 | 2026-09-24T02:34:37.771065+00:00 to 2026-09-25T02:20:37.750065+00:00 |

## Reproduce

From this folder, run:

```powershell
python -m pip install psycopg2-binary tzdata
python analyze.py
```

The default uses snapshot.json and rewrites report.md. To query the current database and replace the snapshot and report, run `python analyze.py --refresh`.
Database refresh requires the local PostgreSQL service. Optional connection override: HATHAWAY_DSN environment variable.
