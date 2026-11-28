"""Independently audit fixed-input Quality timing, image scores and repeats."""
import argparse
from datetime import datetime, timedelta
import json
import math
from pathlib import Path
import re
import statistics

from check_measurement import digest, read_csv
from metrics import ldr_flip


def verify(root):
    root = root.resolve()
    manifest = json.loads((root/'manifest.json').read_text())
    assert manifest['complete'] and manifest['state'] == 'complete'
    assert len(manifest['jobs']) == 27 and len(manifest['calibration']) == 3
    assert len(manifest['repeat_controls']) == 3
    bank = json.loads((Path(manifest['reference_bank'])/'measurement.json').read_text())
    policy = manifest['signature']['policy']
    expected_order = []
    shots_path = next(Path(p) for p in manifest['signature']['inputs'] if Path(p).name == 'shots.json')
    spec = json.loads(shots_path.read_text())
    for shot in spec['shots']:
        for index, seed in enumerate(spec['seeds']):
            names = [c['name'] for c in policy['methods']]
            for name in names[index:] + names[:index]:
                expected_order.append(f"{shot['name']}--{name}--{seed}")
    assert list(manifest['jobs']) == expected_order
    checks = {'passed': False, 'artifact_hashes': 0, 'timestamp_rows': 0,
              'state_rows': 0, 'direct_flip_scores': 0, 'max_score_difference': 0,
              'max_heartbeat_gap_seconds': 0, 'sources': 0}
    summaries = {}
    all_records = [(n, j, j['row']) for n, j in manifest['jobs'].items()]
    for name, record in manifest['calibration'].items():
        all_records.append((name, record, {'frames': 81, 'seed': 17, 'config': 'rr-quality'}))
    for name, record in manifest['repeat_controls'].items():
        all_records.append((name, record, {'frames': 81, 'seed': 17, 'config': name[7:]}))
    try:
        for p, sha in manifest['signature']['inputs'].items():
            assert digest(Path(p)) == sha
            checks['sources'] += 1
        for name, record, row in all_records:
            if (root/'pause.request').exists():
                raise InterruptedError('pause.request')
            for p, sha in record['files'].items():
                assert digest(Path(p)) == sha, p
                checks['artifact_hashes'] += 1
            config = next(c for c in policy['methods'] if c['name'] == row['config'])
            command = json.loads((root/'data'/f'{name}.command.json').read_text())
            assert command.count('--frame-ms') == 1
            assert abs(float(command[command.index('--frame-ms')+1])-1000/60) < 1e-9
            for flag in ['--dump-every', '--auto-exposure', '--no-temporal', '--no-spatial']:
                assert flag not in command
            if config['upscaler'] == 'DLSS':
                assert command[command.index('--dlss-preset')+1] == 'K'
                assert float(command[command.index('--dlss-exposure')+1]) == 1
            states = read_csv(root/'data'/f'{name}.states.csv')
            timings = read_csv(root/'data'/f'{name}.timings.csv')
            expected_frames = list(range(1, row['frames']+1))
            assert [int(r['frame']) for r in states] == expected_frames
            assert [int(r['frame']) for r in timings] == expected_frames
            for index, state in enumerate(states):
                assert int(state['seed']) == row['seed'] and int(state['spp']) == 1
                assert int(state['accumulated_frames']) == index+1
                assert int(state['upscaler_history_reset']) == int(index == 0)
                assert state['active_upscaler'] == config['upscaler']
                assert [int(state[k]) for k in ['render_width','render_height','output_width','output_height']] == [853,480,1280,720]
                assert abs(float(state['frame_time_ms'])-1000/60) < 1e-5
                assert float(state['dlss_exposure']) == (1 if config['upscaler'] == 'DLSS' else 0)
            for index, timing in enumerate(timings):
                gpu, trace = float(timing['gpu_ms']), float(timing['trace_ms'])
                assert math.isfinite(gpu) and math.isfinite(trace) and gpu > 0 and 0 <= trace <= gpu+.001
                assert int(timing['setup_frame']) == int(index == 0)
            checks['state_rows'] += len(states)
            checks['timestamp_rows'] += len(timings)
            hardware = read_csv(root/'data'/f'{name}.hardware.csv')
            assert len(hardware) >= 2 and all(int(r['ac_online']) == 1 for r in hardware)
            assert all(r['gpu'] == manifest['signature']['gpu'] and
                       r['driver'] == manifest['signature']['driver'] for r in hardware)
            activity = read_csv(root/'data'/f'{name}.activity.csv')
            assert not any(float(r['percent']) > 1 and r['process'] not in
                           ['starcore-render','dwm','System','Idle','heartbeat'] for r in activity)
            heartbeats = [datetime.fromisoformat(r['utc']) for r in activity if r['process'] == 'heartbeat']
            assert len(heartbeats) >= 2
            gap = max((b-a).total_seconds() for a,b in zip(heartbeats,heartbeats[1:]))
            assert gap <= 3
            checks['max_heartbeat_gap_seconds'] = max(checks['max_heartbeat_gap_seconds'],gap)
            log = (root/'data'/f'{name}.log').read_text()
            assert not re.search(r'/(error|critical)\]|evaluating failed',log)
            wall = re.search(r'\[(\d\d):(\d\d):(\d\d)\.(\d{3})\].*Wall: ([\d.]+) ms/frame after setup',log)
            assert wall
            anchor = datetime.fromisoformat(hardware[-1]['utc']).astimezone()
            hour,minute,second,millis = map(int,wall.groups()[:4])
            end = anchor.replace(hour=hour,minute=minute,second=second,microsecond=millis*1000)
            if end < anchor-timedelta(hours=12): end += timedelta(days=1)
            if end > anchor+timedelta(hours=12): end -= timedelta(days=1)
            start = end-timedelta(milliseconds=float(wall[5])*(row['frames']-1))
            assert heartbeats[0] <= start and (end-heartbeats[-1]).total_seconds() <= 3
            if 'shot' in row:
                for metric in ['gpu_ms','trace_ms']:
                    assert math.isclose(statistics.median(float(r[metric]) for r in timings[16:]),row[metric],abs_tol=1e-9)
                ref = bank['references'][row['shot']+'-1280x720']
                for field,key in [('flip','reference'),('secondary_flip','independent_reference')]:
                    value = ldr_flip(Path(ref[key]),Path(row['image']))
                    difference = abs(value-row[field])
                    assert difference <= 1e-9
                    checks['max_score_difference'] = max(checks['max_score_difference'],difference)
                    checks['direct_flip_scores'] += 1
                summaries.setdefault((row['shot'],row['config']),[]).append(row)
            print(f'AUDITED {name}',flush=True)
        summary = read_csv(root/'summary.csv')
        assert len(summary) == 9
        for row in summary:
            data = summaries[row['shot'],row['config']]
            assert sorted(r['seed'] for r in data) == sorted(spec['seeds'])
            for metric in ['gpu_ms','trace_ms','flip','secondary_flip']:
                for suffix,func in [('median',statistics.median),('min',min),('max',max)]:
                    assert math.isclose(float(row[metric+'_'+suffix]),func(r[metric] for r in data),abs_tol=1e-9)
        values = [r['gpu_ms'] for r in manifest['calibration'].values()]
        assert (max(values)-min(values))/statistics.median(values) <= .2
        for name,record in manifest['repeat_controls'].items():
            config = name[7:]
            original = manifest['jobs'][f'cockpit-anvil-turn--{config}--17']['row']['image']
            assert record['exact_endpoint_match'] and digest(root/'data'/f'{name}.png') == digest(Path(original))
        checks.update(passed=True, motion_runs=27, calibration_runs=3, exact_repeat_controls=3,
                      summary_rows=9, thresholds_changed=False, verifier_sha256=digest(Path(__file__)))
    finally:
        (root/'verification.json').write_text(json.dumps(checks,indent=2))
    return checks


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path('renders/quality-fixed-exposure'))
    print(json.dumps(verify(parser.parse_args().root)))
