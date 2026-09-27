"""Reproduce the document review: python check_documents.py (Python standard library only)."""
from pathlib import Path
import hashlib

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SOURCES = {
    'tasks.h': [
        'Exactly one per completed trial, counted on entry to ITI',
        'IDLE      --in position--> SAMPLE1 | SAMPLE2',
        'ITI       --T3_ITI_MS-->   IDLE',
    ],
    'tasks.cpp': ['countTrial(pending_);', 'setTimeout(T3_ITI_MS);'],
    'data_analysis/references/tasks-and-analyses.md': [
        'the outcome is emitted on entry to `ITI`',
        'Do not join every state, lick, and outcome only on equal',
    ],
}

lines = [
    '# Task 3 trial definition: document review', '',
    'Scope: current task-3 firmware rules. This document check does not identify or validate historical sessions.', '',
    'Conclusion: a trial begins on entry to SAMPLE1 (STATE channel 1) or SAMPLE2 (channel 2), and completes on entry to ITI (channel 8). The outcome is booked there. The ITI waiting period follows completion and ends with a return to IDLE.', '',
    'Early pauses and returns to DELAY remain part of the same trial. Aborted trials also end at ITI. A recording that stops before ITI contains an incomplete trial.', '',
    'For reconstruction, pair each sample-state entry with its following ITI entry, checking for intervening sample starts, recording boundaries, and missing records. Use device time and sequence order. Do not equate the sample STATE.value with OUTCOME.value: the completed-trial counter increments at completion.', '',
    'Retrieve STATE, OUTCOME, and PARAM_<NAME> records from events_dev and reconstruct trials within the analysis. Active TASK samples are available separately in samples_dev. Missing task identity, boundaries, or parameter history must be reported explicitly.', '',
    'Evidence below is from current local documentation and implementation, not a database validation of historical firmware. The documents settle the requested definition, so no database query was needed.', '',
    '## Source evidence', '',
]
for relative, needles in SOURCES.items():
    path = REPO / relative
    raw = path.read_bytes()
    content = raw.decode('utf-8-sig').splitlines()
    lines += [f'### {relative}', '', f'SHA-256: `{hashlib.sha256(raw).hexdigest()}`', '']
    for needle in needles:
        matches = [(i, line.strip()) for i,line in enumerate(content, 1) if needle in line]
        if not matches:
            raise RuntimeError(f'Expected evidence changed in {relative}: {needle}')
        for i,line in matches:
            lines += [f'- Line {i}: `{line}`']
    lines += ['']
lines += ['## Reproduce', '',
          'Run `python check_documents.py` from this folder. No additional packages are required. The script checks the current local source text and regenerates this report; source changes may change the report or cause an evidence check to fail.', '']
(HERE / 'report.md').write_text('\n'.join(lines), encoding='utf-8')
print(HERE / 'report.md')
