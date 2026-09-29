"""Run: python analysis.py; refresh database snapshot: python analysis.py --refresh.
Dependencies: python -m pip install psycopg2-binary tzdata
Reads PostgreSQL only. Default reruns use the saved snapshot for reproducibility.
"""
import argparse
import json
import os
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parent
PACIFIC = ZoneInfo('America/Los_Angeles')
START = datetime(2026, 9, 10, tzinfo=PACIFIC)
END = datetime.fromisoformat('2026-09-25T17:17:05.793739+00:00')
MIN_DURATION_HOURS = 2
DSN = os.environ.get('HATHAWAY_DSN',
    'host=localhost port=5432 dbname=hathaway user=hathaway password=hathaway')


def fetch():
    import psycopg2
    from psycopg2.extras import RealDictCursor
    connection = psycopg2.connect(DSN, connect_timeout=10)
    connection.set_session(readonly=True, isolation_level='REPEATABLE READ')
    with connection, connection.cursor(cursor_factory=RealDictCursor) as cursor:
        cursor.execute('SELECT %s::timestamptz AS cutoff', (END,))
        cutoff = cursor.fetchone()['cutoff']
        # Same scope as the original list: sessions with command-related records.
        cursor.execute("""SELECT s.* FROM sessions s WHERE EXISTS (
            SELECT 1 FROM events e WHERE e.session_id=s.session_id
            AND e.host_ts >= %s AND e.host_ts <= %s
            AND (starts_with(e.type,'PARAM_') OR e.type IN ('MAG_CMD','RAIL_CMD')))
            ORDER BY s.started_at,s.session_id""", (START, cutoff))
        sessions = cursor.fetchall()
        ids = [s['session_id'] for s in sessions]
        cursor.execute("""SELECT session_id,rig_id,host_ts,seq,t_us,type,channel,value
            FROM events WHERE session_id=ANY(%s) AND host_ts <= %s
            AND (starts_with(type,'PARAM_') OR type IN ('MAG_CMD','RAIL_CMD'))
            ORDER BY host_ts,seq""", (ids, cutoff))
        records = cursor.fetchall()
        cursor.execute("""SELECT session_id,rig_id,host_ts,seq,t_us,value
            FROM samples WHERE session_id=ANY(%s) AND host_ts <= %s AND type='TASK'
            ORDER BY host_ts,seq""", (ids, cutoff))
        tasks = cursor.fetchall()
        cursor.execute("""WITH records AS (
            SELECT session_id,rig_id,seq,t_us,host_ts,type,channel,'events' AS source
            FROM events WHERE session_id=ANY(%s) AND host_ts <= %s
            UNION ALL
            SELECT session_id,rig_id,seq,t_us,host_ts,type,channel,'samples' AS source
            FROM samples WHERE session_id=ANY(%s) AND host_ts <= %s)
            SELECT session_id, min(host_ts) AS first_record,max(host_ts) AS last_record,
            min(t_us) FILTER (WHERE t_us>0) AS first_device_us,
            max(t_us) FILTER (WHERE t_us>0) AS last_device_us,
            count(*) AS rows,count(*) FILTER (WHERE t_us=0) AS zero_device_times,
            min(seq) AS min_seq,max(seq) AS max_seq,count(DISTINCT seq) AS distinct_seq,
            array_agg(DISTINCT rig_id) AS rigs
            FROM records GROUP BY session_id ORDER BY session_id""", (ids,cutoff,ids,cutoff))
        quality = cursor.fetchall()
        cursor.execute("""SELECT session_id,'events' AS source,type,channel,count(*) AS rows
            FROM events WHERE session_id=ANY(%s) AND host_ts <= %s
            GROUP BY session_id,type,channel UNION ALL
            SELECT session_id,'samples',type,channel,count(*) FROM samples
            WHERE session_id=ANY(%s) AND host_ts <= %s
            GROUP BY session_id,type,channel ORDER BY session_id,source,type,channel""",
            (ids,cutoff,ids,cutoff))
        counts = cursor.fetchall()
    connection.close()
    return dict(cutoff=cutoff, start=START, fetched_at=datetime.now(timezone.utc), sessions=sessions, records=records,
                tasks=tasks,quality=quality,counts=counts)


def dt(value):
    return datetime.fromisoformat(value) if isinstance(value,str) else value


def stamp(value):
    return dt(value).astimezone(PACIFIC).strftime('%Y-%m-%d %H:%M:%S')


def number(value):
    return f'{value:g}'


def duration(seconds):
    minutes = int(seconds // 60)
    return f'{minutes // 60}h {minutes % 60:02d}m'


def report(data):
    quality = {q['session_id']:q for q in data['quality']}
    kept, excluded = [], []
    for session in data['sessions']:
        sid = session['session_id']
        assert quality[sid]['rigs'] == [session['rig_id']], 'Rig ownership mismatch'
        endpoint = min(dt(session['ended_at']), dt(data['cutoff'])) if session['ended_at'] else quality[sid]['last_record']
        seconds = (dt(endpoint)-dt(session['started_at'])).total_seconds()
        (kept if seconds >= MIN_DURATION_HOURS * 3600 else excluded).append((session,endpoint,seconds))
    lines = ['# Session parameter history', '',
        f"Snapshot captured: {stamp(data['cutoff'])} Pacific time.",
        f'Scope: sessions with command-related records from {stamp(data["start"])} through {stamp(data["cutoff"])}; keep durations of at least {MIN_DURATION_HOURS:g} hours.',
        'All displayed times are Pacific (America/Los_Angeles). Durations are elapsed session spans, not time actively training.',
        'For sessions without an end time, the last recorded row gives a minimum duration; it does not prove the session is still running.', '',
        '| Session | Rig | Start | End / last record | Duration | Active task IDs | Note |',
        '|---|---|---|---|---|---|---|']
    for s,end,seconds in kept:
        task_ids = sorted({int(t['value']) for t in data['tasks'] if t['session_id']==s['session_id']})
        lines.append(f"| {s['session_id']} | {s['rig_id']} | {stamp(s['started_at'])} | {stamp(end)}{' (last record)' if not s['ended_at'] else ''} | {'at least ' if not s['ended_at'] else ''}{duration(seconds)} | {', '.join(map(str,task_ids)) or 'not recorded'} | {s['note'] or '—'} |")
    lines += ['', 'Excluded: '+ '; '.join(f"session {s['session_id']} ({seconds:.1f} seconds)" for s,_,seconds in excluded)+'.', '',
        '## How to read the settings', '',
        'The first value recorded for each parameter is the initial observed setting, not proof of its value before that record. All numeric values retain firmware units.',
        'Later sessions compare their initial observed values with the previous retained session’s final observed values for the same rig. Differences may reflect resets or changes in excluded sessions; they are not attributed to a command in the retained session.',
        'Within-session tables list every observed value change. Unchanged confirmations are omitted. PARAM records may come from SET, startup, DUMP, or GET; this is not a complete outbound command history.',
        'TASK parameter values describe configuration; active task IDs above come from TASK samples.', '']
    previous = {}
    for index,(s,_,_) in enumerate(kept):
        sid,rig = s['session_id'],s['rig_id']
        params = [r for r in data['records'] if r['session_id']==sid and r['type'].startswith('PARAM_')]
        initial,final,changes = {},{},[]
        for r in params:
            name,value = r['type'][6:],r['value']
            initial.setdefault(name,r)
            if name in final and final[name] != value:
                changes.append((r,name,final[name],value))
            final[name] = value
        lines += [f'## Session {sid}', '']
        if rig not in previous:
            lines += ['### Initial observed parameters', '', '| Parameter | Value | First recorded |','|---|---|---|']
            lines += [f"| `{name}` | {number(r['value'])} | {stamp(r['host_ts'])} |" for name,r in sorted(initial.items())]
        else:
            previous_id,old = previous[rig]
            lines += [f'### Initial differences from session {previous_id} final values', '', '| Parameter | Previous final | Current initial |','|---|---|---|']
            diff = [(name,r['value']) for name,r in sorted(initial.items()) if name not in old or old[name]!=r['value']]
            lines += [f"| `{name}` | {number(old[name]) if name in old else 'not previously recorded'} | {number(value)} |" for name,value in diff]
            if not diff:
                lines += ['| No observed differences | — | — |']
            missing = sorted(set(old)-set(initial))
            if missing:
                lines += ['', 'Previously recorded parameters missing in this session: '+', '.join(missing)+'.']
        lines += ['', '### Changes during the session', '', '| Time | Parameter | Before | After |','|---|---|---|---|']
        lines += [f"| {stamp(r['host_ts'])} | `{name}` | {number(before)} | {number(after)} |" for r,name,before,after in changes]
        if not changes:
            lines += ['| No observed changes | — | — | — |']
        lines += ['']
        previous[rig] = (sid,final)
    lines += ['## Data checks', '',
        'Counts and all parameter confirmations are saved in snapshot.json. No trial rates were calculated.',
        'Sequence gaps below count absent sequence numbers between the minimum and maximum across both tables. Duplicate counts are extra rows sharing a sequence number; neither metric alone identifies the cause.', '',
        '| Session | Rows | Sequence gaps | Duplicate sequence rows | Zero device times | Device-time span (UTC) |',
        '|---|---|---|---|---|---|']
    for s,_,_ in kept:
        q=quality[s['session_id']]
        bounds = [datetime.fromtimestamp(q[k]/1e6,timezone.utc).isoformat() if q[k] else 'missing' for k in ('first_device_us','last_device_us')]
        lines.append(f"| {s['session_id']} | {q['rows']} | {q['max_seq']-q['min_seq']+1-q['distinct_seq']} | {q['rows']-q['distinct_seq']} | {q['zero_device_times']} | {' to '.join(bounds)} |")
    lines += ['', '## Reproduce', '',
        'From this folder, run:', '', '```powershell',
        'python -m pip install psycopg2-binary tzdata',
        'python analysis.py', '```', '',
        'The default uses snapshot.json and rewrites report.md. To query the database and replace the snapshot and report, run `python analysis.py --refresh`.',
        'Database refresh requires the local PostgreSQL service. Optional connection override: HATHAWAY_DSN environment variable.', '']
    lines += ['## Pipeline and delivered files', '',
        '1. Open a read-only, repeatable-read PostgreSQL transaction. Select sessions with PARAM_*, MAG_CMD, or RAIL_CMD events whose host arrival time is within START and END.',
        '2. Save snapshot.json: start/cutoff scope, sessions, command-related records, active TASK samples, counts by table/type/channel, and full-session quality summaries through the cutoff. Records retain session, rig, device time, arrival time, sequence, type, channel, and value.',
        '3. Check rig ownership and compute elapsed duration from session opening to its end (capped at END), or last recorded row if unclosed. Apply MIN_DURATION_HOURS, inclusive.',
        '4. In recorded host-time/sequence order, retain the first parameter confirmation and compare later confirmations with the preceding value. Suppress unchanged confirmations only in this report; preserve every record in the snapshot.',
        '5. Compare the initial settings with the preceding retained session for the same rig, then generate the tables and quality checks above. No trial reconstruction, rate calculation, or figures are part of this task.', '',
        'Tunable settings near the top of analysis.py: START, END, MIN_DURATION_HOURS, PACIFIC, and DSN. A changed acquisition interval automatically refreshes the snapshot; changing only the duration threshold reuses it.',
        'Times in the parameter tables are approximate host arrival times, not device capture times. No physical parameter-use timing or outbound command history is inferred.',
        'Deliverables: analysis.py (complete pipeline), snapshot.json (intermediate database dataset), report.md (pipeline and results).',
        f'Retrieval timestamp: {data.get("fetched_at", data["cutoff"])}. Query cutoff: {data["cutoff"]}.', '']
    return '\n'.join(lines)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--refresh',action='store_true')
    args=parser.parse_args()
    path=ROOT/'snapshot.json'
    cached=json.loads(path.read_text(encoding='utf-8')) if path.exists() else None
    if args.refresh or cached is None or dt(cached['start']) != START or dt(cached['cutoff']) != END:
        data=fetch()
        path.write_text(json.dumps(data,default=str,indent=2),encoding='utf-8')
    data=json.loads(path.read_text(encoding='utf-8'))
    (ROOT/'report.md').write_text(report(data),encoding='utf-8')
    print(ROOT/'report.md')
