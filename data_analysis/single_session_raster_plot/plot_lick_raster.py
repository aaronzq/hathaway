"""Session 79: eight sample/outcome/early-lick groups, 30 random trials each.
Install: python -m pip install matplotlib
Run from this folder: python plot_lick_raster.py
Uses session_79_trials.json and session_79_snapshot.json; no database writes.
"""
from bisect import bisect_left, bisect_right
from collections import Counter
import hashlib
import json
from pathlib import Path
import random
import statistics

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

ROOT=Path(__file__).resolve().parent
SEED=79
N=30
FIRST_LICK_DIAMETER_PT=6
ROW_PITCH_PT=1.2*FIRST_LICK_DIAMETER_PT


def main(sample_id, outcome, stem, early_lick=0):
    trials=json.loads((ROOT/'session_79_trials.json').read_text(encoding='utf-8'))
    data=json.loads((ROOT/'session_79_snapshot.json').read_text(encoding='utf-8'))
    records=sorted(data['records'],key=lambda r:(r['t_us'],r['seq']))
    keys=[(r['t_us'],r['seq']) for r in records]
    eligible=[]
    missing_response=0
    for trial in trials:
        if not (trial['trial_type']==sample_id and trial['outcome']==outcome and trial['early_lick']==early_lick
                and trial['head_fixing'] in (0,1) and trial['start_us'] and trial['end_us']):
            continue
        rows=records[bisect_left(keys,(trial['start_us'],trial['start_seq'])):
                     bisect_right(keys,(trial['end_us'],trial['end_seq']))]
        phases={c:[r for r in rows if r['type']=='STATE' and r['channel']==c] for c in (sample_id,3,4,5)}
        if len(phases[5])!=1:
            missing_response+=1
            continue
        assert all(len(phases[c])==1 for c in (sample_id,4)) and phases[3], 'Unexpected sample/delay/go-cue sequence'
        if not early_lick:
            assert len(phases[3])==1, 'Unexpected replay in no-early-lick group'
        response=phases[5][0]['t_us']
        phase_times={name:(phases[c][0]['t_us']-response)/1e6
                     for name,c in [('sample_start',sample_id),('delay_start',3),('go_cue_start',4)]}
        assert phase_times['sample_start']<phase_times['delay_start']<phase_times['go_cue_start']<0
        licks=[{'relative_seconds':(r['t_us']-response)/1e6,'spout':int(r['channel']),
                'seq':r['seq']} for r in rows if r['type']=='LICK' and r['channel'] in (1,2)]
        eligible.append(dict(trial_id=trial['trial_id'],head_fixing=trial['head_fixing'],
            response_start_us=response,response_start_seq=phases[5][0]['seq'],trial_end_seconds=(trial['end_us']-response)/1e6,
            delay_entry_seconds=[(r['t_us']-response)/1e6 for r in phases[3]],
            early_pause_seconds=[(r['t_us']-response)/1e6 for r in rows if r['type']=='STATE' and r['channel']==9],
            **phase_times,licks=licks))
    assert len(eligible)>=N, f'Only {len(eligible)} eligible trials'
    selected=sorted(random.Random(SEED).sample(eligible,N),key=lambda t:t['response_start_us'])
    assert len({t['trial_id'] for t in selected})==N
    for trial in selected:
        after=[r for r in trial['licks'] if (r['relative_seconds'],r['seq'])>(0,trial['response_start_seq'])]
        assert after, 'No lick after response onset in an answered trial'
        trial['first_response_lick_seq']=after[0]['seq']
    counts=Counter(t['head_fixing'] for t in selected)
    selection=dict(session_id=79,rig_id=1,task=3,sample=sample_id,outcome=outcome,early_lick=early_lick,seed=SEED,eligible_count=len(eligible),
        missing_response_exclusions=missing_response,selected_count=N,head_fixing_counts=dict(counts),
        selection_method='Uniform random sample without replacement; no balancing by head fixing',
        time_basis='Device t_us; seconds relative to STATE RESPONSE entry; licks within sample-to-ITI trial; delay span includes all replays and early pauses',
        input_sha256={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest()
                      for name in ('session_79_trials.json','session_79_snapshot.json')},trials=selected)
    selection_name='session_79_raster_selection' if stem=='session_79_lick_raster' else stem+'_selection'
    (ROOT/(selection_name+'.json')).write_text(json.dumps(selection,indent=2),encoding='utf-8')

    axes_height=N*ROW_PITCH_PT/72
    figure_height=axes_height+1.9
    fig=plt.figure(figsize=(10,figure_height))
    ax=fig.add_axes([.10,1.0/figure_height,.87,axes_height/figure_height])
    fig.patch.set_facecolor('white')
    colors={1:'#134E6F',2:'#FF6150'}
    for y,trial in enumerate(selected,1):
        for name in ('sample_start','delay_start'):
            ax.vlines(trial[name],y-.43,y+.43,color='#ED8D80',linewidth=.85,zorder=1)
        ax.vlines(trial['go_cue_start'],y-.43,y+.43,color='#939393',linewidth=.75,linestyle='dotted',zorder=1)
        for spout,color in colors.items():
            xs=[r['relative_seconds'] for r in trial['licks'] if r['spout']==spout]
            ax.scatter(xs,[y]*len(xs),s=9,color=color,edgecolors='none',zorder=3)
        first=next(r for r in trial['licks'] if r['seq']==trial['first_response_lick_seq'])
        ax.scatter([first['relative_seconds']],[y],s=FIRST_LICK_DIAMETER_PT**2,
                   color=colors[first['spout']],edgecolors='none',zorder=4)
    ax.axvline(0,color='black',linewidth=1,zorder=2)
    lo=min(t['sample_start'] for t in selected)
    hi=max(t['trial_end_seconds'] for t in selected)
    ax.set_xlim(lo-.10,hi+.10)
    ax.set_ylim(N+.5,.5)
    ticks=list(range(1,N+1,5))+[N]
    ax.set_yticks(ticks,[str(selected[y-1]['trial_id']) for y in ticks])
    ax.set_ylabel('Session trial number')
    ax.set_xlabel('Time from response onset (s)')
    ax.spines[['top','right']].set_visible(False)
    ax.spines[['left','bottom']].set_color('#A0A0A0')
    ax.tick_params(labelsize=10)
    sample=statistics.median(t['sample_start'] for t in selected)
    delay=statistics.median(t['delay_start'] for t in selected)
    cue=statistics.median(t['go_cue_start'] for t in selected)
    for label,x in [('Sample',(sample+delay)/2),('Delay',(delay+cue)/2),('After go cue',hi/2)]:
        ax.text(x,1.02,label,transform=ax.get_xaxis_transform(),ha='center',va='bottom',fontsize=11)
    fig.suptitle(f'Session 79 · Sample {sample_id} · {outcome} · Early lick = {early_lick}\n'
                 f'{N} random trials · Head fixing: {counts[1]} yes / {counts[0]} no · Seed {SEED}',
                 fontsize=12,y=.98)
    handles=[Line2D([],[],marker='o',linestyle='none',color=colors[s],markersize=5,label=f'Spout {s}') for s in (1,2)]
    handles += [Line2D([],[],color='#ED8D80',linewidth=1,label='Sample / delay start'),
                Line2D([],[],color='#939393',linestyle=':',label='Go-cue start'),
                Line2D([],[],color='black',linewidth=1,label='Response onset (0 s)')]
    handles += [Line2D([],[],marker='o',linestyle='none',color='#555555',
                       markersize=FIRST_LICK_DIAMETER_PT,label='First lick after response onset')]
    fig.legend(handles=handles,loc='lower center',bbox_to_anchor=(.5,.015),ncol=3,frameon=False,fontsize=9)
    fig.canvas.draw()
    measured_pitch=abs(ax.transData.transform((0,2))[1]-ax.transData.transform((0,1))[1])*72/fig.dpi
    assert abs(measured_pitch/FIRST_LICK_DIAMETER_PT-1.2)<1e-9
    fig.savefig(ROOT/(stem+'.png'),dpi=200)
    fig.savefig(ROOT/(stem+'.svg'))
    plt.close(fig)
    print(json.dumps({k:v for k,v in selection.items() if k not in ('trials','input_sha256')},indent=2))


if __name__=='__main__':
    for sample_id,outcome,stem in (
        (1,'HIT','session_79_lick_raster'),
        (2,'HIT','session_79_sample2_hit_raster'),
        (1,'INCORRECT','session_79_sample1_incorrect_raster'),
        (2,'INCORRECT','session_79_sample2_incorrect_raster'),
    ):
        main(sample_id,outcome,stem)
    for sample_id in (1,2):
        for outcome in ('HIT','INCORRECT'):
            main(sample_id,outcome,f'session_79_sample{sample_id}_{outcome.lower()}_early1_raster',early_lick=1)
