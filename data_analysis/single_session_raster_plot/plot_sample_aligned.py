"""Run python plot_sample_aligned.py; requires matplotlib and psycopg2-binary.
Prepares local session-79 inputs automatically; refresh: python prepare_data.py --refresh.
Generates eight separate 10-trial PNGs
and a selection manifest. Approved styling is documented in references/figure-making.md.
"""
import json
import math
import random
from bisect import bisect_left, bisect_right
from pathlib import Path
from collections import Counter
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon, Rectangle
from matplotlib.lines import Line2D
from prepare_data import load_data

ROOT=Path(__file__).resolve().parent
SEED=79
COLORS={1:'#134E6F',2:'#FF6150'}
CLASS_COLORS={'HIT':'#D5EDF8','INCORRECT':'#F6D9D9','EARLY':'#B0A6BA'}


def main():
    trials,data=load_data()
    records=sorted(data['records'],key=lambda r:(r['t_us'],r['seq']))
    keys=[(r['t_us'],r['seq']) for r in records]
    manifest=[]
    for early in (0,1):
        for outcome in ('HIT','INCORRECT'):
            for sample in (1,2):
                eligible=[t for t in trials if t['trial_type']==sample and t['outcome']==outcome
                          and t['early_lick']==early and t['head_fixing'] in (0,1)
                          and t['start_us'] is not None and t['end_us'] is not None]
                assert len(eligible)>=10
                selected=sorted(random.Random(SEED).sample(eligible,10),key=lambda t:t['start_us'])
                prepared=[]
                for t in selected:
                    rows=records[bisect_left(keys,(t['start_us'],t['start_seq'])):
                                 bisect_right(keys,(t['end_us'],t['end_seq']))]
                    phases={c:[r for r in rows if r['type']=='STATE' and r['channel']==c] for c in (3,4,5)}
                    assert phases[3] and len(phases[4])==len(phases[5])==1
                    response=phases[5][0]
                    after=[r for r in rows if r['type']=='LICK' and r['channel'] in (1,2)
                           and (r['t_us'],r['seq'])>(response['t_us'],response['seq'])]
                    assert after
                    seconds=lambda r:(r['t_us']-t['start_us'])/1e6
                    prepared.append(dict(trial_id=t['trial_id'],head_fixing=t['head_fixing'],
                        delay=seconds(phases[3][0]),cue=seconds(phases[4][0]),response=seconds(response),
                        first_lick_seq=after[0]['seq'],licks=[dict(time=seconds(r),spout=r['channel'],seq=r['seq'])
                        for r in rows if r['type']=='LICK' and r['channel'] in (1,2)]))
                # Cue width represents real time. For these trials it is 100 ms.
                widths=[(t['response']-t['cue'])*9*72/6 for t in prepared]
                assert max(widths)-min(widths)<1e-7
                side=widths[0]/1.2; pitch=2*side; large=side
                height=10*pitch/72; total=height+1.55
                fig=plt.figure(figsize=(10.3,total))
                ax=fig.add_axes([.085,.83/total,9/10.3,height/total])
                class_color=CLASS_COLORS['EARLY' if early else outcome]
                ax.add_patch(Rectangle((-.025,0),.012,1,transform=ax.transAxes,
                                      facecolor=class_color,edgecolor='none',clip_on=False))
                ax.axvline(0,color='black',linewidth=1,clip_on=False)
                delays=[t['delay'] for t in prepared]
                if max(delays)-min(delays)<1e-7:
                    ax.axvline(delays[0],color='black',linewidth=1)
                for y,t in enumerate(prepared,1):
                    if max(delays)-min(delays)>=1e-7:
                        ax.vlines(t['delay'],y-.5,y+.5,color='black',linewidth=1)
                    for s,color in COLORS.items():
                        xs=[r['time'] for r in t['licks'] if r['spout']==s]
                        ax.scatter(xs,[y]*len(xs),s=4.9883063258**2,color=color,edgecolors='none',zorder=3)
                    first=next(r for r in t['licks'] if r['seq']==t['first_lick_seq'])
                    ax.scatter(first['time'],y,s=large**2,color=COLORS[first['spout']],edgecolors='none',zorder=4)
                    ax.add_patch(Polygon([(t['cue'],y-.25),(t['cue'],y+.25),(t['response'],y)],
                                         facecolor='black',edgecolor='none',zorder=5))
                ax.set_xlim(0,6); ax.set_xticks(range(7)); ax.set_ylim(10.5,.5)
                ax.set_yticks([]); ax.spines[['top','right','left']].set_visible(False)
                ax.set_xlabel('Time from sample onset (s)',fontsize=10)
                ax.tick_params(labelsize=9)
                head=Counter(t['head_fixing'] for t in prepared)
                fig.suptitle(f'Session 79 · Sample {sample} · {outcome} · Early lick = {early}\n'
                             f'10 random trials · Head fixing: {head[1]} yes / {head[0]} no',fontsize=11,y=.97)
                handles=[Line2D([],[],marker='o',linestyle='none',color=COLORS[s],markersize=4,label=f'Spout {s}') for s in (1,2)]
                handles += [Line2D([],[],marker='>',linestyle='none',color='black',markersize=5,label='Go cue → response'),
                            Line2D([],[],marker='o',linestyle='none',color='#555555',markersize=6,label='First response lick')]
                fig.legend(handles=handles,loc='lower center',bbox_to_anchor=(.5,.015),ncol=4,frameon=False,fontsize=8)
                fig.canvas.draw()
                for p in ax.patches:
                    if isinstance(p,Polygon):
                        a,b,c=ax.transData.transform(p.get_xy()[:3]); left=math.dist(a,b)
                        assert abs((c[0]-a[0])/left-1.2)<1e-7
                row=abs(ax.transData.transform((0,2))[1]-ax.transData.transform((0,1))[1])
                assert abs(row/left-2)<1e-7 and abs(large*fig.dpi/72/left-1)<1e-7
                filename=f'session_79_sample{sample}_{outcome.lower()}_early{early}_10trials.png'
                fig.savefig(ROOT/filename,dpi=180); plt.close(fig)
                item=dict(sample=sample,outcome=outcome,early_lick=early,seed=SEED,eligible_count=len(eligible),
                          head_fixing_counts=dict(head),class_color=class_color,filename=filename,
                          response_beyond_6s=sum(t['response']>6 for t in prepared),trials=prepared)
                manifest.append(item)
                print(json.dumps({k:v for k,v in item.items() if k!='trials'}))
    (ROOT/'session_79_10trial_selections.json').write_text(json.dumps(manifest,indent=2))


if __name__=='__main__':
    main()
