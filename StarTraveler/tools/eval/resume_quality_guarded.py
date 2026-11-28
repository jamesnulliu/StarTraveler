"""Retry only background-activity interruptions, retaining every rejected attempt."""
import json
from pathlib import Path
import subprocess
import sys
import time

from run_formal import atomic_json

ROOT = Path(__file__).resolve().parents[3]
RUN = ROOT/'renders/quality-fixed-exposure'
pause = RUN/'pause.request'
previous = -1
stalled = 0
for attempt in range(40):
    if pause.exists(): raise SystemExit(75)
    path = RUN/'manifest.json'
    report = json.loads(path.read_text())
    if report.get('complete'): raise SystemExit(0)
    count = len(report['jobs'])
    stalled = stalled+1 if count == previous else 0
    previous = count
    if stalled >= 10:
        print('STOP: repeated background activity without progress; evidence retained',flush=True)
        raise SystemExit(75)
    if report['state'] == 'paused':
        reason = report.get('pause_reason','')
        if not reason.startswith('background_gpu:'):
            raise SystemExit('Not an automatically retryable interruption: '+reason)
        assert report.get('active_pid') is None
        name = report['active_job']
        assert name not in report['jobs'] and name not in report.get('calibration',{})
        archive = RUN/'interrupted'/str(time.time_ns())
        archive.mkdir(parents=True)
        for source in (RUN/'data').glob(name+'.*'):
            target = archive/source.name
            assert source.resolve().is_relative_to(RUN) and target.resolve().is_relative_to(RUN)
            source.rename(target)
        report.setdefault('pause_history',[]).append({'reason':report.pop('pause_reason'),
            'updated_utc':report['updated_utc'],'archive':str(archive),'automatic_retry':True})
        atomic_json(path,report)
    for _ in range(100):
        if pause.exists(): raise SystemExit(75)
        time.sleep(.2)
    command=[sys.executable,str(Path(__file__).with_name('run_quality_revalidation.py')),
             '--exe',str(ROOT/'build/msvc-vs2022/bin/Release/starcore-render.exe')]
    print(f'RESUME attempt={attempt+1} completed={count}/27; thresholds unchanged',flush=True)
    process = subprocess.Popen(command,cwd=ROOT)
    while process.poll() is None:
        time.sleep(.2)  # The collector observes the same pause file and cleans up its children.
    current = json.loads(path.read_text())
    if current.get('complete'): raise SystemExit(0)
    if current['state'] != 'paused': raise SystemExit(process.returncode or 1)
raise SystemExit('Retry cap reached; completed results and rejected attempts retained')
