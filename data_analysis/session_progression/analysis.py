"""Session progression: trailing HIT / (HIT + INCORRECT).

Dependencies: python -m pip install pandas matplotlib psycopg2-binary tzdata
Run from data_analysis: python session_progression/analysis.py --window 100
Refresh from PostgreSQL: python session_progression/analysis.py --refresh

Selection: sessions starting on/after 2026-09-10 in America/Los_Angeles,
duration strictly >2 hours (ended_at - started_at). For an unclosed session,
use last observed host timestamp instead of assuming it is still running.
Trials must start strictly after 2026-09-10 17:15 and at or before
2026-09-26 18:40 local time. Only known HIT
and INCORRECT outcomes enter the denominator; other outcomes/fragments remain
in trials.csv. Pool sample types and concatenate sessions within each rig,
chronologically; never pool rigs. Windows span session boundaries, require a
full window, and use only retained trials. Trial number starts at 1 per rig.
The early-lick curve uses the same trailing window: early-lick HIT/INCORRECT
trials divided by all HIT/INCORRECT trials. Windows with unknown early-lick
status are left unplotted.
combined2.png shows the same curves with session-start markers and local times.
combined.png overlays accuracy and early-lick rate. Solid black lines mark
confirmed T3_DELAY_MS changes, placing each change
at the next retained trial start (ordered by device time and sequence).
Labels show the confirmation time in Pacific time; repeated confirmations
of an unchanged value are not changes. This is not a per-trial used-value plot.

User explicitly confirmed all selected sessions are task 3 on 2026-09-26;
this supplies active-task evidence where TASK telemetry is absent. Parameter
snapshots are confirmed settings at start, NOT claimed values used throughout
the trial; complete parameter history is retained in the task-local snapshot.
No parameter-based filtering is performed. Device timestamps define timing.
The database is read only. snapshot.pkl.gz is a trusted local cache generated
by this script; delete it or use --refresh to retrieve inputs independently.
"""

import argparse
from bisect import bisect_left
import json
import os
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import PercentFormatter
import pandas as pd
import psycopg2

ROOT = Path(__file__).resolve().parent
WINDOW = 200  # Tunable number of retained HIT/INCORRECT trials.
ZONE = "America/Los_Angeles"
SINCE = pd.Timestamp("2026-09-10", tz=ZONE)
CUTOFF = pd.Timestamp("2026-09-10 17:15", tz=ZONE)
END = pd.Timestamp("2026-09-26 18:40", tz=ZONE)
MIN_SESSION_HOURS = 2  # Strictly greater than this elapsed duration.
RIG_IDS = (1,)
USER_CONFIRMED_TASK3_SESSIONS = (74, 75, 76, 77, 78, 79, 80, 81, 116, 117)
FIGURE_WIDTH = 13
PNG_DPI = 180
DSN = os.environ.get("HATHAWAY_DSN",
    "host=localhost port=5432 dbname=hathaway user=hathaway password=hathaway")
OUTCOMES = {0: "HIT", 1: "INCORRECT", 2: "NO_RESPONSE", 3: "ABORT", 4: "TEACH"}


def query(connection, sql, args=()):
    with connection.cursor() as cursor:
        cursor.execute(sql, args)
        return pd.DataFrame(cursor.fetchall(), columns=[c.name for c in cursor.description])


def acquire():
    with psycopg2.connect(DSN) as connection:
        connection.set_session(readonly=True, isolation_level="REPEATABLE READ")
        sessions = query(connection, "SELECT * FROM sessions WHERE started_at >= %s AND started_at <= %s AND rig_id=ANY(%s) ORDER BY started_at",
                         (SINCE.to_pydatetime(), END.to_pydatetime(), list(RIG_IDS)))
        counts, quality, raw = [], [], []
        for session in sessions.itertuples():
            sid = int(session.session_id)
            end = session.ended_at
            if pd.isna(end):
                end = query(connection, """SELECT max(host_ts) AS last_seen FROM (
                    SELECT max(host_ts) AS host_ts FROM events WHERE session_id=%s
                    UNION ALL SELECT max(host_ts) FROM samples WHERE session_id=%s
                    ) x""", (sid, sid)).iloc[0, 0]
            duration = (end - session.started_at).total_seconds() if pd.notna(end) else 0
            sessions.loc[sessions.session_id == sid, "duration_hours"] = duration / 3600
            if duration <= MIN_SESSION_HOURS * 3600:
                continue
            print(f"Retrieving session {sid} ({duration / 3600:.2f} hours)", flush=True)
            # Inspect available signals before requesting the analysis subset.
            counts.append(query(connection, """SELECT %s AS session_id, source, type, channel, count(*) AS n
                FROM (SELECT 'events' AS source,type,channel FROM events WHERE session_id=%s
                UNION ALL SELECT 'samples',type,channel FROM samples WHERE session_id=%s) x
                GROUP BY source,type,channel ORDER BY source,type,channel""", (sid, sid, sid)))
            quality.append(query(connection, """WITH all_rows AS (
                SELECT seq,t_us,rig_id FROM events WHERE session_id=%s
                UNION ALL SELECT seq,t_us,rig_id FROM samples WHERE session_id=%s)
                SELECT %s AS session_id, count(*) AS rows, count(DISTINCT seq) AS unique_seq,
                count(*)-count(DISTINCT seq) AS duplicate_seq,
                max(seq)-min(seq)+1-count(DISTINCT seq) AS missing_seq,
                count(*) FILTER (WHERE t_us=0) AS zero_device_times,
                min(t_us) FILTER (WHERE t_us>0) AS first_t_us, max(t_us) AS last_t_us,
                array_agg(DISTINCT rig_id) AS rigs FROM all_rows""", (sid, sid, sid)))
            records = query(connection, """SELECT session_id,rig_id,seq,t_us,host_ts,type,channel,value
                FROM events WHERE session_id=%s AND (type IN ('STATE','OUTCOME','LICK') OR type LIKE 'PARAM_%%')
                UNION ALL SELECT session_id,rig_id,seq,t_us,host_ts,type,channel,value
                FROM samples WHERE session_id=%s AND type IN ('TASK','MAGNET')
                ORDER BY t_us,seq""", (sid, sid))
            raw.append(records)
        if not raw:
            raise ValueError("No sessions satisfy the selection.")
    return dict(sessions=sessions, counts=pd.concat(counts, ignore_index=True),
                quality=pd.concat(quality, ignore_index=True), raw=pd.concat(raw, ignore_index=True),
                retrieved_utc=pd.Timestamp.now(tz="UTC").isoformat(), query_scope=query_scope())


def query_scope():
    return dict(session_since=SINCE.isoformat(), session_start_through=END.isoformat(),
                min_session_hours=MIN_SESSION_HOURS, rig_ids=list(RIG_IDS))


def reconstruct(records):
    """Pair ordered SAMPLE starts, ITI ends, and validated following OUTCOMEs."""
    rows = []
    for (sid, rig), group in records.groupby(["session_id", "rig_id"], sort=False):
        if not group.type.eq("TASK").any() and sid not in USER_CONFIRMED_TASK3_SESSIONS:
            raise ValueError(f"Session {sid}: missing TASK telemetry; explicit user identification required")
        params, current, end = {}, None, None
        state, magnet, counter, task = None, None, None, 3
        session_rows = []

        def fragment(reason):
            nonlocal current, end
            if current is not None:
                current.update(outcome="Unknown", boundary_status=reason)
                session_rows.append(current)
            current, end = None, None

        def begin(record=None):
            return dict(session_id=sid, rig_id=rig, task=task,
                        start_t_us=record.t_us if record else None,
                        start_seq=record.seq if record else None,
                        Sample=int(record.channel) if record else "Unknown",
                        start_counter=record.value if record else None,
                        parameters_at_start=json.dumps(params, sort_keys=True) if record else None,
                        response_spout=None, **{"Early lick": "Unknown", "Head fixing": "Unknown"},
                        _delay=False, _magnet_bad=False, _magnet_unknown=False, _go=False)

        for record in group.sort_values(["t_us", "seq"]).itertuples(index=False):
            if record.t_us <= 0:
                continue
            kind = record.type
            if kind.startswith("PARAM_"):
                params[kind[6:]] = float(record.value)
            elif kind == "TASK":
                observed = int(record.value)
                if observed != 3:
                    raise ValueError(f"Session {sid}: TASK={observed} contradicts task-3 confirmation")
                task = observed
            elif kind == "MAGNET":
                magnet = int(record.value)
                if current and current["_delay"] and not current["_go"]:
                    current["_magnet_bad"] |= magnet == 0
            elif kind == "STATE":
                if counter is not None and record.value < counter:
                    fragment("counter_reset")
                counter = record.value
                state = int(record.channel)
                if state in (1, 2):
                    fragment("missing_end_before_next_start")
                    current = begin(record)
                elif state == 8:
                    current = current or begin()
                    end = record
                    current.update(end_t_us=record.t_us, end_seq=record.seq)
                elif state == 0:
                    fragment("missing_outcome_or_abandoned")
                if current and state == 3:
                    if not current["_delay"]:
                        current["Early lick"] = 0 if current["start_t_us"] else "Unknown"
                    current["_delay"] = True
                    current["_magnet_bad"] |= magnet == 0
                    current["_magnet_unknown"] |= magnet is None
                if current and state == 9:
                    current["Early lick"] = 1
                if current and state == 4:
                    current["_go"] = True
                    current["_magnet_bad"] |= magnet == 0
                    current["Head fixing"] = (0 if current["_magnet_bad"] else
                        "Unknown" if current["_magnet_unknown"] or not current["_delay"] else 1)
            elif kind == "LICK" and current:
                if state == 3:
                    current["Early lick"] = 1
                if state == 5 and current["response_spout"] is None:
                    current["response_spout"] = int(record.channel)
            elif kind == "OUTCOME":
                matched = end is not None and record.t_us == end.t_us and record.seq > end.seq and record.value == end.value
                if not matched:
                    fragment("end_outcome_mismatch")
                    current = begin()
                current.update(outcome=OUTCOMES.get(int(record.channel), "Unknown"),
                               outcome_code=int(record.channel), outcome_t_us=record.t_us,
                               outcome_seq=record.seq, firmware_counter=record.value,
                               boundary_status="complete" if matched and current["start_t_us"] else "missing_start" if matched else "unmatched_outcome")
                if current["boundary_status"] == "complete":
                    if not current["_delay"]:
                        current["Early lick"] = "N/A"
                    if not current["_go"]:
                        current["Head fixing"] = "N/A"
                session_rows.append(current)
                current, end = None, None
        fragment("recording_ended")
        for number, row in enumerate(session_rows, 1):
            row["session_trial_number"] = number
            rows.append({key: value for key, value in row.items() if not key.startswith("_")})
    return pd.DataFrame(rows)


def rolling_trials(trials, window):
    selected = trials.loc[(trials.start_t_us > CUTOFF.value // 1000) &
                          (trials.start_t_us <= END.value // 1000) &
                          trials.outcome.isin(["HIT", "INCORRECT"])].copy()
    selected = selected.sort_values(["rig_id", "start_t_us", "start_seq", "session_id"])
    selected["trial_number"] = selected.groupby("rig_id").cumcount() + 1
    selected["hit"] = selected.outcome.eq("HIT").astype(int)
    selected["rolling_accuracy"] = selected.groupby("rig_id")["hit"].transform(
        lambda values: values.rolling(window, min_periods=window).mean())
    return selected


def delay_changes(records, group):
    """Initial confirmed setting and actual changes over the displayed range."""
    first = group.iloc[0]
    keys = list(zip(group.start_t_us, group.start_seq))
    history = records.loc[records.rig_id.eq(first.rig_id) &
                          records.type.eq("PARAM_T3_DELAY_MS") &
                          records.t_us.gt(0)].sort_values(["t_us", "seq"])
    initial = history.loc[history.session_id.eq(first.session_id) &
                          ((history.t_us < first.start_t_us) |
                           (history.t_us.eq(first.start_t_us) & history.seq.le(first.start_seq)))]
    if initial.empty:
        raise ValueError("No initial T3_DELAY_MS confirmation in the first session")
    previous = float(initial.iloc[-1].value)
    changes = [dict(trial_number=1, value=previous,
                    time=pd.to_datetime(int(initial.iloc[-1].t_us), unit="us", utc=True).tz_convert(ZONE).isoformat())]
    for record in history.itertuples():
        key = (record.t_us, record.seq)
        if key <= keys[0] or key > keys[-1] or record.value == previous:
            continue
        previous = float(record.value)
        changes.append(dict(trial_number=int(group.iloc[bisect_left(keys, key)].trial_number),
                            value=previous,
                            time=pd.to_datetime(record.t_us, unit="us", utc=True).tz_convert(ZONE).isoformat()))
    return changes


def plot_session_starts(selected, sessions, window):
    """Same rolling curves, with session boundaries at first retained trials."""
    fig, ax = plt.subplots(figsize=(FIGURE_WIDTH, 6.5), layout="constrained")
    metadata = sessions.set_index("session_id")
    markers = []
    for rig, group in selected.groupby("rig_id"):
        suffix = f" (rig {rig})" if selected.rig_id.nunique() > 1 else ""
        ax.plot(group.trial_number, group.rolling_accuracy, color="#1f77b4",
                linewidth=1, label="Accuracy" + suffix)
        ax.plot(group.trial_number, group.rolling_early_lick_rate, color="#B0A6BA",
                linewidth=1, label="Early-lick rate" + suffix)
        for sid, session in group.groupby("session_id", sort=False):
            x = int(session.trial_number.iloc[0])
            start = pd.Timestamp(metadata.loc[sid, "started_at"]).tz_convert(ZONE)
            ax.axvline(x, color="black", linestyle="-", linewidth=0.9)
            ax.text(x, 1.02, f"Session {sid} | {start:%m/%d %H:%M:%S}",
                    transform=ax.get_xaxis_transform(), rotation=90,
                    ha="left", va="bottom", fontsize=8, color="black")
            markers.append(dict(session_id=int(sid), rig_id=int(rig), trial_number=x,
                                session_start=start.isoformat()))
    ax.set(xlabel="Trial number (HIT and INCORRECT only)", ylabel="Rate", ylim=(0, 1.03))
    fig.suptitle(f"Accuracy and early-lick rate | trailing {window} trials\n"
                 "Session starts labeled in Pacific time", fontsize=12)
    ax.yaxis.set_major_formatter(PercentFormatter(1))
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="y", color="0.92")
    ax.legend(loc="lower left", frameon=False)
    fig.savefig(ROOT / "combined2.png", dpi=PNG_DPI)
    plt.close(fig)
    return markers


def write_report(snapshot, summary, selected):
    lines = ["# Session progression", "", "## Reproduce and tune", "",
        "From this folder: `python -m pip install pandas matplotlib psycopg2-binary tzdata`, then `python analysis.py`. Use `python analysis.py --refresh` to replace the database snapshot and rebuild every output. `python analysis.py --window 1000` overrides the default rolling window.",
        "All user settings are near the top of analysis.py: WINDOW, ZONE, SINCE, CUTOFF, END, MIN_SESSION_HOURS, RIG_IDS, USER_CONFIRMED_TASK3_SESSIONS, FIGURE_WIDTH, PNG_DPI, and DSN. Changing acquisition settings automatically refreshes the snapshot; changing the window or within-snapshot lower trial cutoff reuses saved inputs.", "",
        "## Intermediate dataset", "",
        "`snapshot.pkl.gz` is the database dataset used by this task: a compressed pandas pickle containing sessions, raw, counts, quality, retrieved_utc, and query_scope. Load this locally generated file with `pandas.read_pickle('snapshot.pkl.gz')`. The raw table contains session_id, rig_id, seq, t_us, host_ts, type, channel, and value. It retains STATE/OUTCOME/LICK/PARAM_* events and TASK/MAGNET samples; full-session counts and quality summaries cover both database tables.",
        f"Retrieved: {snapshot['retrieved_utc']}. Acquisition scope: `{json.dumps(snapshot['query_scope'])}`. Derived CSV files remain separate from this snapshot.", "",
        "## Pipeline", "",
        f"1. In a read-only, repeatable-read PostgreSQL transaction, select sessions starting from {SINCE.isoformat()} through {END.isoformat()} on rigs {list(RIG_IDS)}. Keep elapsed durations strictly greater than {MIN_SESSION_HOURS:g} hours, using ended_at minus started_at, or the last observed host timestamp if unclosed. This is recording span, not active training time.",
        "2. Inspect counts by table/type/channel, retrieve the required signals, and audit all session rows for rig ownership, duplicate/missing sequence numbers, and zero device timestamps. Save the snapshot before deriving trial tables.",
        "3. Reconstruct task-3 trials in device-time/sequence order: SAMPLE1/SAMPLE2 starts, ITI ends, then matching OUTCOME at the same timestamp and completed counter. Keep boundary fragments, resets, and unmatched outcomes explicit. Active TASK telemetry is checked; for the listed historical sessions, missing TASK telemetry is covered by the user's explicit task-3 confirmation. No confirmation is assumed for new sessions.",
        "4. Retain trial sample, response spout, boundaries, outcome, firmware counter, and confirmed parameters at sample entry. These parameter snapshots are not claimed to be values used throughout the trial. Early lick is positive for a LICK during DELAY or EARLY_PAUSE evidence. Head fixing checks continuous MAGNET=1 from first DELAY through GOCUE; unknown coverage stays unknown.",
        f"5. Require trial starts strictly after {CUTOFF.isoformat()} and at or before {END.isoformat()}. Count only HIT and INCORRECT. Unknown starts cannot be filtered reliably and are excluded. Pool sample types and sessions chronologically within each rig, retaining original session row numbers; never pool rigs.",
        f"6. Use a trailing window of {summary['window']} retained trials, including the current trial. Accuracy = HIT/(HIT+INCORRECT). Early-lick rate = early-lick HIT/INCORRECT trials divided by all HIT/INCORRECT trials in that window. Require a full window; no-response, teach, and abort trials never enter either denominator. Windows cross session boundaries. Unknown early-lick status makes that window unplotted.",
        "7. Read the last T3_DELAY_MS confirmation before the first plotted trial within its session. Suppress repeated equal confirmations and place each later recorded change at the next counted trial start, honoring sequence order at equal timestamps. Labels show the new setting and its recorded Pacific time. These markers track confirmed configuration, not an inferred exact parameter-use instant.",
        "8. Write the derived tables, summary, both combined figures, and this report. Validate outcome reconciliation, rig identity, and uniqueness of counted outcomes. combined.png marks delay changes; combined2.png marks each session at its first retained trial with a solid black line. Session labels use the actual sessions.started_at in Pacific time, which can precede the trial cutoff (especially session 74). Both figures use identical rolling curves.", "",
        "## Results and checks", "",
        f"Sessions: {summary['session_ids']}. Rigs: {sorted(selected.rig_id.unique().tolist())}. Counted trials: {summary['retained_trials']:,}; HIT: {summary['hits']:,}; INCORRECT: {summary['incorrect']:,}; early lick: {summary['early_lick_trials']:,}; unknown early-lick status: {summary['unknown_early_lick_trials']}.",
        f"First/last retained start: {selected.start_local.min()} to {selected.start_local.max()}. Exclusions: {summary['exclusions']}. The empty exclusion string denotes included trials; counts are mutually exclusive with unknown start taking precedence, then time cutoff, then outcome.",
        f"Boundary status before analysis filtering: {summary['boundary_status']}. Full-session outcome counts: {summary['all_outcomes']}.",
        f"Full-session audit totals: {int(snapshot['quality'].missing_seq.sum())} missing sequence numbers, {int(snapshot['quality'].duplicate_seq.sum())} duplicate sequence rows, {int(snapshot['quality'].zero_device_times.sum())} zero device timestamps. See quality.csv and counts.csv for session details.", "",
        "## Delivered files", "",
        "| File | Contents |", "|---|---|",
        "| analysis.py | Complete acquisition, reconstruction, analysis, plotting, and reporting entry point |",
        "| snapshot.pkl.gz | Intermediate database dataset and acquisition metadata |",
        "| sessions.csv, counts.csv, quality.csv | Session selection and database quality summaries |",
        "| trials.csv | All reconstructed trials/fragments, parameter snapshots, and exclusion reasons |",
        "| rolling_accuracy.csv | Counted trials and both rolling rates |",
        "| summary.json | Filters, counts, quality, delay changes, and session-start markers |",
        "| combined.png | Overlaid accuracy and early-lick rate with delay-change markers |",
        "| combined2.png | Same curves with session IDs and session-start times |",
        "| report.md | This pipeline, settings, outputs, and limitations |", "",
        "Overlapping rolling windows are descriptive and are not independent observations. Session gaps are removed from the trial-number axis. The saved snapshot fixes the data available at retrieval; refresh explicitly to obtain newly recorded data. Unclosed-session duration is only an observed lower bound.", "",
        "![Combined accuracy and early-lick rate](combined.png)", "",
        "![Combined curves with session starts](combined2.png)", ""]
    (ROOT / "report.md").write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--window", type=int, default=WINDOW)
    parser.add_argument("--refresh", action="store_true")
    args = parser.parse_args()
    if args.window < 1:
        parser.error("--window must be positive")
    cache = ROOT / "snapshot.pkl.gz"
    snapshot = pd.read_pickle(cache, compression="gzip") if cache.exists() else None
    if args.refresh or snapshot is None or snapshot.get("query_scope") != query_scope():
        snapshot = acquire()
        pd.to_pickle(snapshot, cache, compression="gzip")
    for name in ("sessions", "counts", "quality"):
        snapshot[name].to_csv(ROOT / f"{name}.csv", index=False)
    for record in snapshot["quality"].itertuples():
        expected = snapshot["sessions"].set_index("session_id").loc[record.session_id, "rig_id"]
        assert record.rigs == [expected], "Unexpected rig in session"
    trials = reconstruct(snapshot["raw"])
    # A full-session sequence gap can make an apparently continuous hold unknown.
    gap_sessions = snapshot["quality"].loc[snapshot["quality"].missing_seq > 0, "session_id"]
    trials.loc[trials.session_id.isin(gap_sessions) & trials["Head fixing"].eq(1), "Head fixing"] = "Unknown"
    trials["start_local"] = pd.to_datetime(trials.start_t_us, unit="us", utc=True).dt.tz_convert(ZONE)
    trials["exclusion"] = ""
    trials.loc[~trials.outcome.isin(["HIT", "INCORRECT"]), "exclusion"] = "not HIT/INCORRECT"
    trials.loc[trials.start_t_us <= CUTOFF.value // 1000, "exclusion"] = "at/before cutoff"
    trials.loc[trials.start_t_us > END.value // 1000, "exclusion"] = "after end cutoff"
    trials.loc[trials.start_t_us.isna(), "exclusion"] = "unknown trial start"
    selected = rolling_trials(trials, args.window)
    early_lick = pd.to_numeric(selected["Early lick"], errors="coerce")
    selected["rolling_early_lick_rate"] = early_lick.groupby(selected.rig_id).transform(
        lambda values: values.rolling(args.window, min_periods=args.window).mean())
    assert len(trials.dropna(subset=["outcome_seq"])) == int(snapshot["raw"].type.eq("OUTCOME").sum())
    assert not selected.duplicated(["session_id", "outcome_seq"]).any()
    trials.to_csv(ROOT / "trials.csv", index=False)
    selected.to_csv(ROOT / "rolling_accuracy.csv", index=False)
    if selected.empty:
        raise ValueError("No eligible trials")
    fig, ax = plt.subplots(figsize=(FIGURE_WIDTH, 6.5), layout="constrained")
    delay_history = {}
    for rig, group in selected.groupby("rig_id"):
        suffix = f" (rig {rig})" if selected.rig_id.nunique() > 1 else ""
        ax.plot(group.trial_number, group.rolling_accuracy, color="#1f77b4",
                linewidth=1, label="Accuracy" + suffix)
        ax.plot(group.trial_number, group.rolling_early_lick_rate, color="#B0A6BA",
                linewidth=1, label="Early-lick rate" + suffix)
        changes = delay_changes(snapshot["raw"], group)
        delay_history[int(rig)] = changes
        for index, change in enumerate(changes):
            x = change["trial_number"]
            if index:
                ax.axvline(x, color="black", linestyle="-", linewidth=0.9)
            label = f"{change['value']:g} ms"
            label += " (initial)" if index == 0 else " | " + pd.Timestamp(change["time"]).strftime("%m/%d %H:%M:%S")
            ax.text(x, 1.02, label, transform=ax.get_xaxis_transform(), rotation=90,
                    ha="left", va="bottom", fontsize=8, color="black")
    ax.set(xlabel="Trial number (HIT and INCORRECT only)", ylabel="Rate",
           ylim=(0, 1.03))
    fig.suptitle(f"Accuracy and early-lick rate | trailing {args.window} trials\n"
                 "T3_DELAY_MS changes labeled in Pacific time", fontsize=12)
    ax.yaxis.set_major_formatter(PercentFormatter(1))
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="y", color="0.92")
    ax.legend(loc="lower left", frameon=False)
    fig.savefig(ROOT / "combined.png", dpi=PNG_DPI)
    plt.close(fig)
    session_starts = plot_session_starts(selected, snapshot["sessions"], args.window)
    summary = dict(retrieved_utc=snapshot["retrieved_utc"], timezone=ZONE,
                   session_since=SINCE.isoformat(), trial_start_after=CUTOFF.isoformat(),
                   trial_start_at_or_before=END.isoformat(),
                   task_evidence="User confirmed all selected sessions are task 3; TASK checked when available",
                   session_ids=snapshot["quality"].session_id.tolist(), window=args.window,
                   delay_changes=delay_history, session_starts=session_starts,
                   retained_trials=len(selected), hits=int(selected.hit.sum()),
                   early_lick_trials=int(early_lick.eq(1).sum()),
                   unknown_early_lick_trials=int(early_lick.isna().sum()),
                   incorrect=int((1-selected.hit).sum()),
                   exclusions=trials.exclusion.value_counts().to_dict(),
                   boundary_status=trials.boundary_status.value_counts().to_dict(),
                   all_outcomes=trials.outcome.value_counts().to_dict(),
                   quality=snapshot["quality"].to_dict("records"))
    (ROOT / "summary.json").write_text(json.dumps(summary, indent=2, default=str), encoding="utf-8")
    write_report(snapshot, summary, selected)
    print(json.dumps({k: v for k, v in summary.items() if k != "quality"}, indent=2))


if __name__ == "__main__":
    main()
