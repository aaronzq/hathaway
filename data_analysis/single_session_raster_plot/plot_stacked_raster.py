"""Run: python plot_stacked_raster.py (requires matplotlib and psycopg2-binary).
Prepares local session-79 inputs automatically; refresh: python prepare_data.py --refresh.
Seed set by SEED below; sample-aligned 0–6 seconds.
Outputs PNG, editable SVG, and selected-trial manifest beside this script.
"""
import json
import math
import random
from bisect import bisect_left, bisect_right
from collections import Counter
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon, Rectangle
from matplotlib.lines import Line2D
from plot_sample_aligned import COLORS, CLASS_COLORS
from prepare_data import load_data

ROOT=Path(__file__).resolve().parent
SEED=122000


def main():
    trials,data=load_data()
    records=sorted(data['records'],key=lambda r:(r['t_us'],r['seq']))
    keys=[(r['t_us'],r['seq']) for r in records]
    groups=[]
    for label,count,early,outcomes in [('HIT',15,0,('HIT',)),('INCORRECT',5,0,('INCORRECT',)),
                                     ('EARLY LICK',5,1,('HIT','INCORRECT'))]:
        eligible=[t for t in trials if t['early_lick']==early and t['outcome'] in outcomes
                  and t['trial_type'] in (1,2) and t['head_fixing'] in (0,1)
                  and t['start_us'] is not None and t['end_us'] is not None]
        selected=sorted(random.Random(SEED).sample(eligible,count),key=lambda t:t['start_us'])
        # Both samples and head-fixing states are eligible, not guaranteed
        # to appear in a small random selection.
        prepared=[]
        for t in selected:
            rows=records[bisect_left(keys,(t['start_us'],t['start_seq'])):
                         bisect_right(keys,(t['end_us'],t['end_seq']))]
            states={c:[r for r in rows if r['type']=='STATE' and r['channel']==c] for c in (3,4,5)}
            assert states[3] and len(states[4])==len(states[5])==1
            response=states[5][0]
            after=[r for r in rows if r['type']=='LICK' and r['channel'] in (1,2)
                   and (r['t_us'],r['seq'])>(response['t_us'],response['seq'])]
            assert after
            seconds=lambda r:(r['t_us']-t['start_us'])/1e6
            prepared.append(dict(trial_id=t['trial_id'],sample=t['trial_type'],outcome=t['outcome'],
                head_fixing=t['head_fixing'],delay=seconds(states[3][0]),cue=seconds(states[4][0]),
                response=seconds(response),first_lick_seq=after[0]['seq'],
                licks=[dict(time=seconds(r),spout=r['channel'],seq=r['seq']) for r in rows
                       if r['type']=='LICK' and r['channel'] in (1,2)]))
        groups.append(dict(label=label,count=count,early_lick=early,eligible_count=len(eligible),
            sample_counts=dict(Counter(t['sample'] for t in prepared)),
            outcome_counts=dict(Counter(t['outcome'] for t in prepared)),
            head_fixing_counts=dict(Counter(t['head_fixing'] for t in prepared)),
            response_beyond_6s=sum(t['response']>6 for t in prepared),trials=prepared))
    assert len({t['trial_id'] for g in groups for t in g['trials']})==25
    widths=[(t['response']-t['cue'])*9*72/6 for g in groups for t in g['trials']]
    assert max(widths)-min(widths)<1e-7
    side=widths[0]/1.2; pitch=2*side
    gap=.22; bottom=.85; top=.65
    total=sum(g['count']*pitch/72 for g in groups)+2*gap+bottom+top
    matplotlib.rcParams['svg.fonttype']='none'
    fig=plt.figure(figsize=(10.3,total))
    cursor=total-top
    for i,g in enumerate(groups):
        height=g['count']*pitch/72; cursor-=height
        ax=fig.add_axes([.09,cursor/total,9/10.3,height/total])
        color=CLASS_COLORS['EARLY' if g['early_lick'] else g['label']]
        ax.add_patch(Rectangle((-.025,0),.012,1,transform=ax.transAxes,facecolor=color,
                              edgecolor='none',clip_on=False))
        ax.text(-.055,.5,f"{g['label']} · n={g['count']}",transform=ax.transAxes,
                rotation=90,ha='center',va='center',fontsize=10)
        ax.axvline(0,color='black',linewidth=1,clip_on=False)
        delays=[t['delay'] for t in g['trials']]
        assert max(delays)-min(delays)<1e-7
        ax.axvline(delays[0],color='black',linewidth=1)
        for y,t in enumerate(g['trials'],1):
            for s,col in COLORS.items():
                xs=[r['time'] for r in t['licks'] if r['spout']==s]
                ax.scatter(xs,[y]*len(xs),s=4.9883063258**2,color=col,edgecolors='none',zorder=3)
            first=next(r for r in t['licks'] if r['seq']==t['first_lick_seq'])
            ax.scatter(first['time'],y,s=side**2,color=COLORS[first['spout']],edgecolors='none',zorder=4)
            ax.add_patch(Polygon([(t['cue'],y-.25),(t['cue'],y+.25),(t['response'],y)],
                                 facecolor='black',edgecolor='none',zorder=5))
        ax.set_xlim(0,6); ax.set_ylim(g['count']+.5,.5); ax.set_yticks([])
        ax.spines[['top','left','right']].set_visible(False)
        if i<2:
            ax.spines['bottom'].set_visible(False); ax.set_xticks([])
        else:
            ax.set_xticks(range(7)); ax.set_xlabel('Time from sample onset (s)')
        fig.canvas.draw()
        for p in ax.patches:
            if isinstance(p,Polygon):
                a,b,c=ax.transData.transform(p.get_xy()[:3]); length=math.dist(a,b)
                assert abs((c[0]-a[0])/length-1.2)<1e-7
        row=abs(ax.transData.transform((0,2))[1]-ax.transData.transform((0,1))[1])
        assert abs(row/length-2)<1e-7 and abs(side*fig.dpi/72/length-1)<1e-7
        cursor-=gap
    fig.suptitle(f'Session 79 · Sample-aligned lick raster\nBoth samples and head-fixing states eligible · Seed {SEED}',fontsize=12,y=1-.12/total)
    handles=[Line2D([],[],marker='o',linestyle='none',color=COLORS[s],markersize=4,label=f'Spout {s}') for s in (1,2)]
    handles += [Line2D([],[],marker='>',linestyle='none',color='black',markersize=5,label='Go cue → response'),
                Line2D([],[],marker='o',linestyle='none',color='#555555',markersize=6,label='First response lick')]
    fig.legend(handles=handles,loc='lower center',bbox_to_anchor=(.5,.01),ncol=4,frameon=False,fontsize=8)
    stem=ROOT/'session_79_stacked_raster'
    fig.savefig(stem.with_suffix('.png'),dpi=180)
    fig.savefig(stem.with_suffix('.svg'))
    plt.close(fig)
    manifest=dict(session_id=79,rig_id=1,task=3,seed=SEED,window_seconds=[0,6],
        method='Random sampling without replacement in each group, then chronological ordering; both samples and head-fixing states eligible, with both outcomes eligible in the early-lick group; no balancing or guaranteed category coverage',groups=groups)
    (ROOT/'session_79_stacked_selection.json').write_text(json.dumps(manifest,indent=2))
    print(json.dumps([{k:v for k,v in g.items() if k!='trials'} for g in groups],indent=2))


if __name__=='__main__':
    main()
