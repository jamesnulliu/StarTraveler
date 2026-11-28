"""Publish verified Quality summaries and prepare the presentation's Q&A appendix."""
import csv
import hashlib
import json
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[3]
RUN = ROOT/'renders/quality-fixed-exposure'
OUT = ROOT/'docs/final-materials'


def main():
    manifest = json.loads((RUN/'manifest.json').read_text())
    audit = json.loads((RUN/'verification.json').read_text())
    assert manifest['complete'] and audit['passed']
    rows = list(csv.DictReader((RUN/'summary.csv').open()))
    lookup = {(r['shot'],r['config']): r for r in rows}
    shots = ['cockpit-anvil-turn','mirror-glass-pass','fast-star-turn']
    names = ['Anvil cockpit','Mirror / glass','Star turn']
    methods = ['asvgf-dlss-full','asvgf-dlss-half','rr-quality']
    labels = ['Full + DLSS K','Half + DLSS K','Hybrid + DLSS-RR']
    colors = ['#1869A6','#397A54','#C65A20']
    fig,axes = plt.subplots(2,3,figsize=(15,8),sharex='col',sharey='col')
    for ri,(metric,reference) in enumerate([('flip','Primary'),('secondary_flip','Secondary')]):
        for si,(shot,name) in enumerate(zip(shots,names)):
            ax = axes[ri,si]
            for method,label,color in zip(methods,labels,colors):
                r=lookup[shot,method]
                x=float(r['gpu_ms_median']); y=float(r[metric+'_median'])
                ax.errorbar(x,y,xerr=[[x-float(r['gpu_ms_min'])],[float(r['gpu_ms_max'])-x]],
                    yerr=[[y-float(r[metric+'_min'])],[float(r[metric+'_max'])-y]],
                    marker='o',capsize=4,label=label,color=color)
            ax.set_title(name+' / '+reference+' reference')
            ax.set_xlabel('Median GPU ms/frame')
            ax.set_ylabel('Moving endpoint LDR-FLIP')
            ax.grid(alpha=.2)
    handles,legend=axes[0,0].get_legend_handles_labels()
    fig.legend(handles,legend,loc='lower center',ncol=3,bbox_to_anchor=(.5,.025))
    fig.suptitle('Fixed-input Quality: moving endpoint error and path GPU cost',fontsize=18)
    fig.text(.5,.005,'3 seeds; observed min–max, not CI. Axes differ by shot; references stay separate.',ha='center')
    fig.tight_layout(rect=[0,.09,1,.94]); fig.canvas.draw()
    for ext in ['png','svg']: fig.savefig(OUT/f'quality-error-time.{ext}',dpi=180)
    plt.close(fig)
    (OUT/'quality-chart-data.csv').write_bytes((RUN/'summary.csv').read_bytes())
    table=['| Shot | Method | GPU ms, median (range) | Trace ms, median | Moving FLIP, primary | Moving FLIP, secondary | Seeds |',
           '|---|---|---:|---:|---:|---:|---:|']
    findings=[]
    for shot,name in zip(shots,names):
        for method,label in zip(methods,labels):
            r=lookup[shot,method]
            table.append(f"| {name} | {label} | {float(r['gpu_ms_median']):.3f} ({float(r['gpu_ms_min']):.3f}–{float(r['gpu_ms_max']):.3f}) | {float(r['trace_ms_median']):.3f} | {float(r['flip_median']):.5f} | {float(r['secondary_flip_median']):.5f} | 3 |")
        full=lookup[shot,methods[0]]; rr=lookup[shot,methods[2]]
        delta=float(rr['gpu_ms_median'])-float(full['gpu_ms_median'])
        difference=float(rr['flip_median'])-float(full['flip_median'])
        findings.append(f"- {name}: RR minus Full+K is {delta:+.3f} GPU ms and {difference:+.5f} primary endpoint FLIP (descriptive group medians).")
    report='''# Fixed-input Quality error and timing results

Completed 27 November 2026. This new study replaces the historical Quality
table for discussion of the final fixed-input configuration. It does not turn
the preserved historical wall-clock/auto-exposure table into a controlled one.

Output 1280x720, internal 853x480, 1 spp, five bounces, seeds 17/29/43, fixed
1000/60 ms temporal input, manual tone-map exposure 1, ACES/sRGB, no effects or
clamp. Full/Half A-SVGF explicitly use DLSS K exposure 1; RR retains unity input.
RTX 3070 Laptop, driver 610.91, Release Vulkan, AC power. The three methods
rotate launch positions across seed rounds. See the [protocol](quality-revalidation.md).

GPU and trace numbers are three-seed medians of per-run medians after excluding
the first 16 frames. Image error is the final MOVING frame against each separate
8192-spp reference, not average path error or post-stop terminal error. Three-seed
ranges are observed min–max, not confidence intervals. No significance claim is made.

'''+ '\n'.join(table)+'''\n
![Both-reference error/time comparison](final-materials/quality-error-time.png)

'''+ '\n'.join(findings)+'''\n
Full/Half also changes ray budget; the difference does not isolate filter cost.
These endpoint/cost results and the [post-stop settling results](production-lag-results.md)
answer different questions. Earlier entry into a method's own terminal band
does not imply lower terminal error, moving endpoint error, or GPU cost.
Reference noise remains part of all comparisons.

All 27 main runs, three RR calibration sequences and three exact endpoint repeat
controls pass. The independent audit recomputes all 54 reference scores, checks
commands, fixed inputs, state/history, timestamps, AC power, source hashes and
GPU activity/counter coverage, and recomputes all nine summary rows. Attempts
rejected for background activity remain archived; their results are not included.
All original numerical limits are unchanged. The deferred Performance-M fixture
remains unresolved, and this is not a new full GPU-suite pass.

- [Per-seed results](../renders/quality-fixed-exposure/runs.csv)
- [Summary](../renders/quality-fixed-exposure/summary.csv)
- [Independent verification](../renders/quality-fixed-exposure/verification.json)
- [Manifest, inputs and calibration](../renders/quality-fixed-exposure/manifest.json)
- [Vendor identity check](../renders/quality-fixed-exposure/vendor-runtime-check.json)
- [Fresh upstream check](../renders/quality-fixed-exposure/upstream-check.json)

The figures' source CSV is copied without precision loss beside them. The slide
appendix uses primary-reference medians rounded to eight decimals in editable
chart workbooks for portability, displayed to two GPU-ms/five FLIP decimals.
Both references and seed ranges remain in the report/CSV. Software and method
citations are in the [bibliography](references-denoiser.md).
'''
    (ROOT/'docs/quality-fixed-exposure-results.md').write_text(report,encoding='utf-8')
    content=json.loads((OUT/'slides.json').read_text(encoding='utf-8'))
    content[10]['body'][-1]='Separate fixed-input motion error / GPU-cost results in the appendix'
    content[10]['script']='All eighteen settling captures and thirty-six reference outcomes pass the unchanged finite-capture rule. That study observes one laptop and three paths. A separate twenty-seven-run Quality comparison now measures moving-path GPU cost and the final moving-frame error under fixed temporal and exposure inputs. Its results are in the appendix and report. The settling dump sequences themselves are not timing evidence. We retain the historical table as historical and preserve the known Performance-M color-test failure. The results do not establish significance or universal superiority.'
    content[11]['script']=content[11]['script'].replace('and the final live rehearsal under this configuration','and manual operation, projection and spoken timing checks on the presentation setup').replace('label the older flight backup as illustrative','use the accepted fixed-exposure flight backup as illustration')
    content += [
        {'title':'Appendix: moving-path GPU cost','qualityMetric':'gpu_ms_median',
         'caption':'27-run fixed-input study. Three-seed medians; first 16 frames excluded. Observed ranges are in the report.',
         'seconds':0,'script':'GPU timestamp medians over each moving path. This is a separate timed capture, not the stationary PNG dump. All three methods use the same Quality input/output sizes. See the report for seed ranges and scope.','cue':'补充答辩图，不增加主讲十二分钟；这是独立的运动计时实验。'},
        {'title':'Appendix: error at the end of motion','qualityMetric':'flip_median',
         'caption':'Primary reference, three-seed medians. Final moving frame; separate from stationary terminal FLIP. Lower is better.',
         'seconds':0,'script':'Final moving-frame LDR-FLIP for the same timed runs as the preceding appendix slide. The second reference and all per-seed outcomes remain in the report. Do not substitute these values for the post-stop terminal-error chart.','cue':'运动结束误差与静止后最终误差不是同一指标；双参考和范围见报告。'}]
    (OUT/'slides-r2.json').write_text(json.dumps(content,ensure_ascii=False,indent=2),encoding='utf-8')
    (OUT/'quality-figure-provenance.json').write_text(json.dumps({'source_sha256':hashlib.sha256((RUN/'summary.csv').read_bytes()).hexdigest(),'rows':9,'references_pooled':False,'ranges':'observed min/max, not CI'},indent=2))
    print('Prepared verified Quality report, figures and two untimed native-chart appendix slides.')


if __name__ == '__main__': main()
