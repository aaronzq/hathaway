"""Session 79 task-3 trial reconstruction.
Install: python -m pip install psycopg2-binary tzdata
Run: python analyze_session.py [--refresh]
Default reuses session_79_snapshot.json; --refresh reads the database.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import os
from pathlib import Path
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parent
TZ = ZoneInfo('America/Los_Angeles')
OUTCOMES = {0:'HIT',1:'INCORRECT',2:'NO_RESPONSE',3:'ABORT',4:'TEACH'}


def fetch():
    import psycopg2
    from psycopg2.extras import RealDictCursor
    with psycopg2.connect(os.environ.get('HATHAWAY_DSN',
            'host=localhost port=5432 dbname=hathaway user=hathaway password=hathaway')) as con:
        con.set_session(readonly=True, isolation_level='REPEATABLE READ')
        with con.cursor(cursor_factory=RealDictCursor) as cur:
            cur.execute('SELECT * FROM sessions WHERE session_id=%s',(79,))
            session = cur.fetchone()
            cur.execute('''SELECT session_id,rig_id,seq,t_us,host_ts,type,channel,value
                FROM events_dev WHERE session_id=%s AND
                (type IN ('STATE','OUTCOME','LICK') OR starts_with(type,'PARAM_'))
                ORDER BY t_us,seq''',(79,))
            records = cur.fetchall()
            cur.execute("SELECT * FROM samples_dev WHERE session_id=%s AND type='TASK' ORDER BY t_us,seq",(79,))
            tasks = cur.fetchall()
            cur.execute("SELECT seq,t_us,type,channel,value FROM samples_dev WHERE session_id=%s AND type IN ('MAGNET','T3_PROB1') ORDER BY t_us,seq",(79,))
            usage_samples = cur.fetchall()
            cur.execute('''WITH r AS (
                SELECT rig_id,seq,t_us,type,channel,'events' AS source FROM events WHERE session_id=%s
                UNION ALL SELECT rig_id,seq,t_us,type,channel,'samples' FROM samples WHERE session_id=%s)
                SELECT source,type,channel,count(*) AS rows FROM r
                GROUP BY source,type,channel ORDER BY source,type,channel''',(79,79))
            counts = cur.fetchall()
            cur.execute('''WITH r AS (
                SELECT rig_id,seq,t_us FROM events WHERE session_id=%s
                UNION ALL SELECT rig_id,seq,t_us FROM samples WHERE session_id=%s)
                SELECT count(*) AS rows,count(DISTINCT seq) AS distinct_seq,
                min(seq) AS min_seq,max(seq) AS max_seq,
                count(*) FILTER (WHERE t_us=0) AS zero_device_times,
                min(t_us) FILTER (WHERE t_us>0) AS first_device_us,max(t_us) AS last_device_us,
                array_agg(DISTINCT rig_id) AS rigs FROM r''',(79,79))
            quality = cur.fetchone()
    return dict(session=session, records=records, tasks=tasks, usage_samples=usage_samples, counts=counts,
                quality=quality, fetched_at=datetime.now(timezone.utc).isoformat())


def when(us):
    return datetime.fromtimestamp(us/1e6,TZ).isoformat(timespec='microseconds') if us else 'Missing (before recording)'


def reconstruct(data):
    assert data['session']['rig_id']==1 and data['quality']['rigs']==[1]
    assert all(int(t['value'])==3 for t in data['tasks']), 'Task observations conflict with task 3'
    records = [r for r in data['records'] if r['t_us']>0]
    params, trials, changes = {}, [], []
    pending = None
    previous_state_counter = None
    for r in records:
        kind,channel,value = r['type'],int(r['channel']),r['value']
        if kind.startswith('PARAM_'):
            name=kind[6:]
            old=params.get(name)
            if old is not None and old!=value:
                change=dict(name=name,before=old,after=value,t_us=r['t_us'],seq=r['seq'])
                changes.append(change)
                if pending is not None:
                    pending['during_trial_changes'].append(change)
            params[name]=value
        elif kind=='STATE':
            if previous_state_counter is not None and value<previous_state_counter:
                raise ValueError('Counter reset detected: requires explicit epoch handling')
            previous_state_counter=value
            if channel in (1,2):
                if pending is not None:
                    pending['status']='missing_end_before_next_sample'
                    trials.append(pending)
                pending=dict(trial_id=None,start_us=r['t_us'],start_seq=r['seq'],
                    start_counter=int(value),trial_type=channel,correct_spout=channel,
                    task=3,session_id=79,rig_id=1,parameters_at_start=params.copy(),
                    end_us=None,end_seq=None,outcome=None,during_trial_changes=[],status='missing_end')
            elif channel==8:
                if pending is None:
                    pending=dict(trial_id=None,start_us=None,start_seq=None,start_counter=None,
                        trial_type=None,correct_spout=None,task=3,session_id=79,rig_id=1,
                        parameters_at_start=None,end_us=None,end_seq=None,outcome=None,
                        during_trial_changes=[],status='missing_start')
                else:
                    assert int(value)==pending['start_counter']+1, 'Counter mismatch'
                    pending['status']='complete'
                pending.update(end_us=r['t_us'],end_seq=r['seq'],trial_id=int(value))
                trials.append(pending)
                pending=None
        elif kind=='OUTCOME':
            assert trials and trials[-1]['end_us']==r['t_us'], 'Unmatched outcome'
            trial=trials[-1]
            assert trial['trial_id']==int(value) and trial['outcome'] is None
            assert trial['end_seq']<r['seq']
            trial.update(outcome=OUTCOMES[channel],outcome_seq=r['seq'])
    if pending is not None:
        trials.append(pending)
    assert sum(t['outcome'] is not None for t in trials)==sum(r['type']=='OUTCOME' for r in records)
    assert sum(t['start_us'] is not None for t in trials)==sum(r['type']=='STATE' and r['channel'] in (1,2) for r in records)
    assert all(t['outcome'] is not None for t in trials if t['end_us'] is not None)
    previous={}
    for row_number,t in enumerate(trials,1):
        t['firmware_counter']=t['trial_id']
        t['trial_id']=row_number
        current=t['parameters_at_start']
        t['parameter_changes_at_start']=None if current is None else {k:v for k,v in current.items() if k not in previous or previous[k]!=v}
        if current is not None:
            previous=current
    return trials,changes


def parameter_usage(data,trials):
    """Resolve session-79 variable settings; stop for unsupported task-setting changes."""
    timeline=sorted([r for r in data['records'] if r['type'].startswith('PARAM_') and r['t_us']>0]
                    +data['usage_samples'],key=lambda r:(r['t_us'],r['seq']))
    settings={}
    magnet_state=None
    hold=None
    observations=[]
    probability=None
    for r in timeline:
        kind=r['type']
        if kind.startswith('PARAM_'):
            settings[kind[6:]]=r['value']
        elif kind=='T3_PROB1':
            probability=r['value']
        elif kind=='MAGNET':
            if r['value']==1 and magnet_state==0:
                hold={k:settings.get(k) for k in ('MAG_FIX_DURATION','MAG_GRACE_MS')}
            elif r['value']==1 and magnet_state is None:
                hold={'MAG_FIX_DURATION':None,'MAG_GRACE_MS':None}
            elif r['value']==0:
                hold=None
            magnet_state=r['value']
        observations.append(((r['t_us'],r['seq']),dict(settings),None if hold is None else dict(hold),probability))
    from bisect import bisect_right
    keys=[o[0] for o in observations]
    for t in trials:
        if not t['start_us']:
            t['parameters_used']=None
            continue
        start=(t['start_us'],t['start_seq'])
        end=(t['end_us'],t['end_seq'])
        idx=bisect_right(keys,start)-1
        assert idx>=0
        o=observations[idx]
        # Task-3 phase settings do not vary within any session-79 trial.
        assert all(c['name'] in ('MAG_FIX_DURATION','SCALE_HIGH_THRESH') for c in t['during_trial_changes']), 'Needs phase-specific parameter reconstruction'
        t['parameters_used']={
            'task3_confirmed_settings':{k:v for k,v in t['parameters_at_start'].items() if k.startswith('T3_')},
            'effective_type1_probability':o[3],
            'magnet_hold_at_start':o[2],
            'scale_high_threshold_intervals':[{'from_us':t['start_us'],'value':o[1].get('SCALE_HIGH_THRESH')}],
        }
        for c in t['during_trial_changes']:
            if c['name']=='SCALE_HIGH_THRESH':
                t['parameters_used']['scale_high_threshold_intervals'].append({'from_us':c['t_us'],'value':c['after']})
        # Save all magnet holds overlapping the trial, not merely the start setting.
        holds=[o[2]]
        for obs in observations[idx+1:bisect_right(keys,end)]:
            if obs[2]!=holds[-1]:
                holds.append(obs[2])
        t['parameters_used']['magnet_holds_during_trial']=holds


def classify_magnet(data,trials):
    from bisect import bisect_right
    key=lambda r:(r['t_us'],r['seq'])
    magnets=sorted([r for r in data['usage_samples'] if r['type']=='MAGNET' and r['t_us']>0],key=key)
    keys=[key(r) for r in magnets]
    cues=sorted([r for r in data['records'] if r['type']=='STATE' and r['channel']==4 and r['t_us']>0],key=key)
    cue_keys=[key(r) for r in cues]
    states=sorted([r for r in data['records'] if r['type']=='STATE' and r['t_us']>0],key=key)
    state_keys=[key(r) for r in states]
    previous_end=(0,-1)
    for t in trials:
        begin=(t['start_us'],t['start_seq']) if t['start_us'] else previous_end
        end=(t['end_us'],t['end_seq']) if t['end_us'] else (float('inf'),float('inf'))
        found=cues[bisect_right(cue_keys,begin):bisect_right(cue_keys,end)]
        assert len(found)<=1, 'Multiple go-cue entries need review'
        t['magnet_at_go_cue']=None
        t['go_cue_us']=None
        t['go_cue_seq']=None
        t['magnet_sample_seq']=None
        t['head_fixing']=None
        t['head_fixing_interval_start_us']=None
        t['head_fixing_interval_start_seq']=None
        t['magnet_classification']='N/A' if t['status']=='complete' else 'Unknown'
        if found:
            cue=found[0]
            t.update(go_cue_us=cue['t_us'],go_cue_seq=cue['seq'])
            idx=bisect_right(keys,key(cue))-1
            t['magnet_classification']='Unknown'
            if idx>=0 and magnets[idx]['value'] in (0,1):
                value=int(magnets[idx]['value'])
                t.update(magnet_at_go_cue=value,magnet_sample_seq=magnets[idx]['seq'])
            interval_states=states[bisect_right(state_keys,begin):bisect_right(state_keys,key(cue))]
            if t['start_us'] and interval_states and interval_states[0]['channel']==3:
                first_delay=interval_states[0]
                t.update(head_fixing_interval_start_us=first_delay['t_us'],
                         head_fixing_interval_start_seq=first_delay['seq'])
                left=bisect_right(keys,key(first_delay))
                right=bisect_right(keys,key(cue))
                values=([magnets[left-1]['value']] if left else [None])+[r['value'] for r in magnets[left:right]]
                if 0 in values:
                    t['head_fixing']=0
                elif all(v==1 for v in values):
                    # The stored session-wide audit establishes uninterrupted ingest.
                    q=data['quality']
                    if q['max_seq']-q['min_seq']+1==q['distinct_seq'] and q['zero_device_times']==0:
                        t['head_fixing']=1
                if t['head_fixing'] is not None:
                    t['magnet_classification']=str(t['head_fixing'])
        previous_end=end


def classify_early_lick(data,trials):
    """Use recorded licks during DELAY; EARLY_PAUSE is additional positive evidence."""
    from bisect import bisect_right
    rows=sorted([r for r in data['records'] if r['type'] in ('STATE','LICK') and r['t_us']>0],
                key=lambda r:(r['t_us'],r['seq']))
    keys=[(r['t_us'],r['seq']) for r in rows]
    previous_end=(0,-1)
    for t in trials:
        begin=(t['start_us'],t['start_seq']) if t['start_us'] else previous_end
        end=(t['end_us'],t['end_seq']) if t['end_us'] else (float('inf'),float('inf'))
        current_state=t['trial_type']
        delay_entries=0
        early_pauses=0
        delay_licks=[]
        for r in rows[bisect_right(keys,begin):bisect_right(keys,end)]:
            if r['type']=='STATE':
                current_state=int(r['channel'])
                delay_entries+=current_state==3
                early_pauses+=current_state==9
            elif current_state==3 and r['channel'] in (1,2):
                delay_licks.append(r['seq'])
        if delay_licks or early_pauses:
            indicator='1'
        elif t['status']!='complete':
            indicator='Unknown'
        elif not delay_entries:
            indicator='N/A'
        else:
            indicator='0'
        t.update(early_lick_classification=indicator,
                 early_lick=int(indicator) if indicator in ('0','1') else None,
                 delay_entries=delay_entries,early_pause_entries=early_pauses,
                 delay_lick_sequences=delay_licks)
        previous_end=end


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--refresh',action='store_true')
    args=parser.parse_args()
    path=ROOT/'session_79_snapshot.json'
    if args.refresh or not path.exists():
        path.write_text(json.dumps(fetch(),default=str,indent=2),encoding='utf-8')
    data=json.loads(path.read_text(encoding='utf-8'))
    trials,changes=reconstruct(data)
    parameter_usage(data,trials)
    classify_magnet(data,trials)
    classify_early_lick(data,trials)
    excluded=[t for t in trials if t['start_us'] is None or t['end_us'] is None]
    trials=[t for t in trials if t['start_us'] is not None and t['end_us'] is not None]
    previous_params={}
    for row_number,t in enumerate(trials,1):
        t['unfiltered_row_number']=t['trial_id']
        t['trial_id']=row_number
        current=t['parameters_at_start']
        t['parameter_changes_at_start']={k:v for k,v in current.items() if k not in previous_params or previous_params[k]!=v}
        previous_params=current
    (ROOT/'session_79_trials.json').write_text(json.dumps(trials,indent=2),encoding='utf-8')
    counts=Counter(t['status'] for t in trials)
    q=data['quality']
    gaps=q['max_seq']-q['min_seq']+1-q['distinct_seq']
    lines=['# Session 79 trial table', '',
        'Definitions are documented in references/tasks-and-analyses.md and references/telemetry.md. Trial rows are numbered chronologically; ABORT is a recorded outcome, distinct from a missing recording boundary.', '',
        f"Rig 1; task 3 supplied by the user. Session: {data['session']['started_at']} to {data['session']['ended_at']} (stored host times). Note: {data['session']['note'] or 'none'}.",
        f"Trial timestamps below use device time in Pacific time (UTC−07:00). Snapshot: {data['fetched_at']}.",
        f"Included {len(trials)} trials with recorded start and end times. Excluded {len(excluded)} recording fragments (unfiltered rows: {[t['trial_id'] for t in excluded]}). This filter applies only to this analysis.",
        f"TASK samples: {len(data['tasks'])}. If absent, active task identity relies on the user's session identification; PARAM_TASK alone is not proof of activation.",
        f"Quality: {q['rows']} total rows, {gaps} absent sequence numbers, {q['rows']-q['distinct_seq']} duplicate sequence rows, {q['zero_device_times']} zero device timestamps. No counter decreases detected in STATE records.",
        f"Device-time coverage: {when(q['first_device_us'])} to {when(q['last_device_us'])}.", '',
        'Trial ID is the chronological row number after filtering. Firmware counters and original row numbers are retained in JSON for checks. Start is SAMPLE1/SAMPLE2 entry; end is ITI entry. Trials missing either boundary are excluded.',
        'Parameters are the latest confirmations at sample entry in (device timestamp, sequence) order. The first trial with a recorded start lists available non-magnet settings; later rows list only differences from the previous recorded start. A dash means unchanged. Values retain firmware units.',
        'Head fixing is 1 only when MAGNET stays 1 continuously from sample exit (first DELAY entry) through GOCUE entry, including all delay replays and early-lick pauses. The held value at sample exit and every update through go-cue entry are checked in (t_us, seq) order. Any observed 0 in that interval gives 0, even if magnet returns to 1 before the go cue. A request for trials with head fixing selects 1. N/A means a complete trial never reached the go cue. Unknown means insufficient interval coverage. Magnet duration/grace settings and hold histories are omitted from the table and retained in saved data.',
        'Sample is 1 or 2 from SAMPLE1/SAMPLE2 entry; Unknown means the sample entry was not recorded. Early lick is 1 when a lick is recorded during DELAY (either spout), or EARLY_PAUSE supplies positive evidence; 0 means the complete trial entered DELAY without either. N/A means a complete trial never entered DELAY; Unknown means insufficient recording coverage to establish absence. State/lick ordering uses (t_us, seq).',
        'With T3_EARLY_LICK_PUNISH enabled, delay licks normally trigger EARLY_PAUSE followed by a replay of DELAY. A trial can end during the pause before replay; it still has early lick=1. Without punishment, licks can occur without replay. Licks during sample, go cue, or EARLY_PAUSE alone do not qualify.',
        'A threshold interval annotation identifies the values available for weight processing during that trial. Task-3 phase settings did not change within any trial in this session. Effective type-1 probability comes from T3_PROB1, not the configured manual probability. Serial confirmation timestamps do not directly timestamp every internal parameter read, so exact effects at a concurrent setting/read boundary cannot be proved from these records.',
        'Full parameter snapshots, trial type, correct spout and outcomes for retained trials are in session_79_trials.json; raw selected records, including excluded fragments, and counts by type/channel are in session_79_snapshot.json.', '',
        '## Reproduce', '',
        'From this folder: `python -m pip install psycopg2-binary tzdata`, then `python analyze_session.py`. Add `--refresh` to replace the snapshot from the local database. Queries are read-only. Optional connection override: HATHAWAY_DSN.',
        'The older check_documents.py/check_view.py and their reports are historical artifacts from before removal of trial_params; they are not required for this analysis.', '',
        '## Trials', '', '| Trial ID | Trial start | Trial end | Sample | Outcome | Head fixing | Early lick | Parameters (confirmed configuration and use) |',
        '|---|---|---|---|---|---|---|---|']
    previous_usage=None
    for t in trials:
        delta=t['parameter_changes_at_start']
        setting='Unknown: sample start not recorded' if delta is None else '; '.join(f'{k}={v:g}' for k,v in sorted(delta.items()) if not k.startswith('MAG_')) or '—'
        used=t['parameters_used']
        if used:
            usage_summary={'effective type-1 probability (%)':used['effective_type1_probability']}
            for key,value in usage_summary.items():
                if previous_usage is None or previous_usage[key]!=value:
                    setting+=f'; {key}: {json.dumps(value)}'
            intervals=used['scale_high_threshold_intervals']
            if len(intervals)>1:
                setting+='; SCALE_HIGH_THRESH used over trial: '+ '; '.join(f"{x['value']:g} from {when(x['from_us'])}" for x in intervals)
            previous_usage=usage_summary
        lines.append(f"| {t['trial_id']} | {when(t['start_us'])} | {when(t['end_us']) if t['end_us'] else 'Missing'} | {t['trial_type'] or 'Unknown'} | {t['outcome'] or 'Unknown'} | {t['magnet_classification']} | {t['early_lick_classification']} | {setting} |")
    lines += ['', '## Summary', '',
        f'All counts below use the {len(trials)} retained trials.', '',
        f"- Sample: {dict(sorted(Counter(t['trial_type'] for t in trials).items()))}.",
        f"- Outcome: {dict(Counter(t['outcome'] for t in trials if t['outcome']))}.",
        f"- Head fixing: {dict(Counter(t['magnet_classification'] for t in trials))}.",
        f"- Early lick: {dict(Counter(t['early_lick_classification'] for t in trials))}."]
    (ROOT/'session_79_trials.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print(json.dumps(dict(rows=len(trials),statuses=counts,parameter_changes=len(changes),
        mid_trial_changes=sum(len(t['during_trial_changes']) for t in trials),
        first_id=trials[0]['trial_id'],last_id=trials[-1]['trial_id']),indent=2))


if __name__=='__main__':
    main()
