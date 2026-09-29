"""Run the complete raster analysis: python analysis.py [--refresh].
Dependencies: python -m pip install psycopg2-binary matplotlib
All user controls are here; helper modules need no manual edits.
"""
import argparse
import json
import os
from pathlib import Path

from prepare_data import load_data
from plot_stacked_raster import main as plot_stacked

# Tunable settings. This reconstruction supports task 3 only.
SESSION_ID = 79
RIG_ID = 1
USER_CONFIRMED_TASK3_SESSIONS = (79,)
SEED = 122000
GROUPS = [
    ("HIT", 15, 0, ("HIT",)),
    ("INCORRECT", 5, 0, ("INCORRECT",)),
    ("EARLY LICK", 5, 1, ("HIT", "INCORRECT")),
]
WINDOW_SECONDS = (0, 6)
LICK_DIAMETER_POINTS = 4.9883063258
PNG_DPI = 180
DSN = os.environ.get("HATHAWAY_DSN",
    "host=localhost port=5432 dbname=hathaway user=hathaway password=hathaway")
ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--refresh", action="store_true")
    args = parser.parse_args()
    trials, data = load_data(refresh=args.refresh, session_id=SESSION_ID, rig_id=RIG_ID,
                            user_confirmed_task3_sessions=USER_CONFIRMED_TASK3_SESSIONS, dsn=DSN)
    manifest = plot_stacked(session_id=SESSION_ID, seed=SEED, groups_config=GROUPS,
                            window_seconds=WINDOW_SECONDS, lick_diameter=LICK_DIAMETER_POINTS,
                            dpi=PNG_DPI, prepared_data=(trials, data))
    audit = json.loads((ROOT / f"session_{SESSION_ID}_preparation.json").read_text())
    lines = [f"# Session {SESSION_ID} raster analysis", "",
        "## Reproduce and tune", "",
        "From this folder: `python -m pip install psycopg2-binary matplotlib`, then `python analysis.py`. Use `python analysis.py --refresh` to collect a new database snapshot and regenerate all current results.",
        "Edit the settings near the top of analysis.py: SESSION_ID, RIG_ID, USER_CONFIRMED_TASK3_SESSIONS, SEED, GROUPS (label, count, early-lick flag, outcomes), WINDOW_SECONDS, LICK_DIAMETER_POINTS, PNG_DPI, and DSN. Helpers receive these settings directly. Missing TASK telemetry requires explicit user identification of the new session as task 3.", "",
        "## Intermediate dataset and pipeline", "",
        f"1. Query PostgreSQL read-only in a repeatable-read transaction for session {SESSION_ID}, rig {RIG_ID}. Save session_{SESSION_ID}_snapshot.json with session metadata, STATE/OUTCOME/LICK/PARAM_* events, TASK samples, MAGNET/T3_PROB1 samples, counts, quality checks, and fetched_at. Raw timestamps and sequences are preserved. Keys: session, records, tasks, usage_samples, counts, quality, fetched_at.",
        "2. Validate session and rig ownership and task evidence. Reconstruct task-3 trials in device-time/sequence order from SAMPLE1/SAMPLE2 through ITI and its following OUTCOME. Check counter increments and reject resets or unmatched outcomes.",
        "3. Reconstruct parameter snapshots and supported parameter-use intervals. Head fixing is 1 only for continuous MAGNET=1 from sample exit through GOCUE, including delay replays and pauses; 0 requires observed MAGNET=0. Preserve Unknown and N/A. Early lick is 1 for recorded licks during DELAY or positive EARLY_PAUSE evidence; lack of evidence in incomplete recordings stays unknown.",
        "4. Exclude missing-start/end fragments, retain complete ABORT trials in the derived dataset, and renumber complete trials chronologically while retaining original row numbers and firmware counters. Save exclusions and quality evidence in the preparation JSON.",
        "5. Select complete HIT/INCORRECT trials according to GROUPS with known head-fixing status and required alignment states. Both sample types and head-fixing states are eligible. Sample without replacement using SEED, without balancing categories; then sort each group chronologically. Save all selected trials and licks in the selection JSON.",
        "6. Subtract each sample onset from device times to plot elapsed seconds. Preserve every delay replay and pause. The displayed time window clips the figure, not the saved records. Draw the go-cue-to-response triangle and enlarge the first lick strictly after response onset using timestamp/sequence order.",
        "7. Save PNG and editable SVG. Check selected-count uniqueness, alignment states, equal cue duration, and physical marker proportions: triangle width/height=1.2, row pitch/triangle height=2, first-lick diameter/triangle height=1. No trial rates are calculated.", "",
        "## Scope and results", "",
        f"Session host span: {data['session']['started_at']} to {data['session']['ended_at']}. Retrieved: {data['fetched_at']}. Stored times are UTC; the plot uses device elapsed seconds, not a wall-clock time zone.",
        f"Task evidence: {audit['task_basis']}. Retained complete trials: {len(trials)}; excluded fragments: {len(audit['excluded_trials'])}. Outcomes: {audit['outcome_counts']}.",
        f"Full-session checks: {audit['sequence_gaps']} missing sequence numbers, {audit['duplicate_sequence_rows']} duplicate rows, {audit['quality']['zero_device_times']} zero device timestamps.",
        f"Settings: seed={SEED}, window={WINDOW_SECONDS} seconds; ordinary lick diameter={LICK_DIAMETER_POINTS:g} points; PNG resolution={PNG_DPI} dots per inch.", "",
        "| Group | Eligible | Selected | Sample counts | Head-fixing counts | Responses outside window |",
        "|---|---:|---:|---|---|---:|"]
    for g in manifest["groups"]:
        lines.append(f"| {g['label']} | {g['eligible_count']} | {g['count']} | {g['sample_counts']} | {g['head_fixing_counts']} | {g['response_outside_window']} |")
    lines += ["", "## Files and limitations", "",
        f"- `session_{SESSION_ID}_snapshot.json`: intermediate database dataset.",
        f"- `session_{SESSION_ID}_trials.json`: derived trials; `session_{SESSION_ID}_preparation.json`: reconstruction, exclusions, parameters, and quality audit.",
        f"- `session_{SESSION_ID}_stacked_selection.json`: plotted-trial dataset with exact selection and lick times.",
        f"- `session_{SESSION_ID}_stacked_raster.png` and `.svg`: current figure.",
        "- `analysis.py` and `report.md`: executable pipeline and this report.",
        "The delivered figure is the stacked raster. The random selection does not guarantee every sample/head-fixing category appears. Reconstruction rejects unsupported setting changes rather than extrapolating parameter use.", "",
        f"![Stacked raster](session_{SESSION_ID}_stacked_raster.png)", ""]
    (ROOT / "report.md").write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
