"""Resumable 27-run fixed-exposure Quality error/timing comparison."""
import argparse
import importlib.metadata
import json
import math
from pathlib import Path
import shutil
import statistics

import matplotlib.pyplot as plt

from check_measurement import digest, read_csv, write_csv
from metrics import ldr_flip
from run_formal import Formal, Paused, atomic_json, utc_now


def method_order(configs, round_index):
    shift = round_index % len(configs)
    return configs[shift:] + configs[:shift]


class Quality(Formal):
    def __init__(self, args):
        self.args = args
        self.root = args.output.resolve()
        self.out = self.root / 'data'
        self.out.mkdir(parents=True, exist_ok=True)
        self.pause_path = self.root / 'pause.request'
        self.report_path = self.root / 'manifest.json'
        self.spec = json.loads(args.config.read_text())
        protocol_path = args.config.parent / self.spec['protocol']
        protocol = json.loads(protocol_path.read_text())
        self.common = protocol['common_flags'].copy()
        self.configs = [c for c in protocol['configurations'] if c['group'] == 'quality-end-to-end']
        self.total_runs = 27
        assert len(self.configs) == 3 and len(self.spec['shots']) == 3 and len(self.spec['seeds']) == 3
        self.bank = json.loads((args.references / 'measurement.json').read_text())
        assert self.bank['preflight_complete']
        compatibility = json.loads(args.compatibility.read_text())
        preflight = args.compatibility.parent / 'manifest.json'
        assert compatibility['passed'] and json.loads(preflight.read_text())['fixed_prefix_identical']
        assert compatibility['renderer_sha256'] == digest(args.exe)
        assert compatibility['reference_renderer_sha256'] == self.bank['renderer_sha256']
        assert digest(args.config) == self.bank['config_sha256']
        assert digest(protocol_path) == self.bank['input_hashes']['protocol_sha256']
        assert digest(Path(__file__).with_name('metrics.py')) == self.bank['input_hashes']['metrics_sha256']
        assets = args.exe.resolve().parent / 'assets'
        models = {str(p.relative_to(assets)): digest(p) for p in sorted(assets.rglob('*.glb'))}
        assert models == self.bank['input_hashes']['models'], 'Reference model assets changed'
        self.hardware = self.telemetry()
        inputs = {str(p.resolve()): digest(p) for p in [args.exe, args.config, protocol_path,
                  Path(__file__), Path(__file__).with_name('run_formal.py'),
                  Path(__file__).with_name('check_measurement.py'),
                  Path(__file__).with_name('metrics.py'),
                  Path(__file__).with_name('gpu_activity.ps1'), args.compatibility, preflight]}
        for p in args.exe.resolve().parent.glob('*.dll'):
            inputs[str(p)] = digest(p)
        for p in assets.rglob('*.glb'):
            inputs[str(p)] = digest(p)
        for config in self.configs:
            config['flags'] += ['--frame-ms', str(1000/60)]
            if config['upscaler'] == 'DLSS':
                config['flags'] += ['--dlss-preset', 'K', '--dlss-exposure', '1']
        for shot in self.spec['shots']:
            scene = self.bank['scenes'][shot['name']]
            for role in ['initial', 'end']:
                p = Path(scene['directory']) / f'{role}.yaml'
                assert digest(p) == scene[f'{role}_sha256']
                inputs[str(p)] = digest(p)
            ref = self.reference(shot['name'])
            for key, sha in zip(['reference', 'independent_reference'], ref['sha256']):
                p = Path(ref[key])
                assert digest(p) == sha
                inputs[str(p)] = sha
        versions = {p: importlib.metadata.version(p) for p in ['flip-evaluator', 'numpy', 'Pillow', 'matplotlib']}
        policy = {'methods': self.configs, 'seeds': self.spec['seeds'], 'frame_ms': 1000/60,
                  'sr_exposure': 1, 'tone_exposure': 1, 'warmup_frames': 16, 'ppd': 67,
                  'calibration_runs': 3, 'calibration_max_relative_range': .2,
                  'external_app_engine_peak_max': 1, 'idle_median_max': 5, 'idle_peak_max': 10,
                  'rotate_methods_per_seed': True, 'exact_repeat_controls': 3}
        signature = {'inputs': inputs, 'policy': policy, 'versions': versions,
                     'gpu': self.hardware['gpu'], 'driver': self.hardware['driver']}
        self.fingerprints = {'renderer_sha256': digest(args.exe), 'config_sha256': digest(args.config)}
        if self.report_path.exists():
            self.report = json.loads(self.report_path.read_text())
            assert self.report['signature'] == signature, 'Inputs changed; choose a fresh output'
        else:
            self.report = {'created_utc': utc_now(), 'signature': signature,
                           'reference_bank': str(args.references.resolve()), 'jobs': {}, 'repeat_controls': {}}
        self.report.update(complete=False, state='preparing', controller_pid=None)
        if args.resume and self.pause_path.exists():
            self.pause_path.unlink()
        snapshot = self.root / 'source-snapshot'
        snapshot.mkdir(exist_ok=True)
        for name, sha in inputs.items():
            p = Path(name)
            if p.suffix in {'.py', '.ps1', '.exe', '.dll'}:
                target = snapshot / p.name
                if target.exists():
                    assert digest(target) == sha
                else:
                    shutil.copy2(p, target)
        self.persist()

    def validate_states(self, states, config, seed):
        super().validate_states(states, config, seed)
        assert all(abs(float(r['frame_time_ms'])-1000/60) < 1e-5 for r in states)
        assert all(float(r['dlss_exposure']) == (1 if config['upscaler'] == 'DLSS' else 0) for r in states)

    def collect(self):
        for shot in self.spec['shots']:
            prepared = self.bank['scenes'][shot['name']]
            for round_index, seed in enumerate(self.spec['seeds']):
                for config in method_order(self.configs, round_index):
                    name = f"{shot['name']}--{config['name']}--{seed}"
                    if self.cached(name):
                        continue
                    self.check_pause()
                    for p, sha in self.report['signature']['inputs'].items():
                        assert digest(Path(p)) == sha, f'Input changed: {p}'
                    idle = self.idle_gate()
                    png, states = self.render(name, Path(prepared['directory']) / 'initial.yaml',
                        prepared['poses']['initial'], 1280, 720, shot['frames'], 1, seed,
                        config['flags'] + self.motion_flags(shot), timing=True)
                    self.validate_states(states, config, seed)
                    timings = read_csv(self.out / f'{name}.timings.csv')
                    assert [int(r['frame']) for r in timings] == list(range(1, shot['frames']+1))
                    for i, r in enumerate(timings):
                        gpu, trace = float(r['gpu_ms']), float(r['trace_ms'])
                        assert math.isfinite(gpu) and math.isfinite(trace) and gpu > 0 and 0 <= trace <= gpu+.001
                        assert int(r['setup_frame']) == int(i == 0)
                    ref = self.reference(shot['name'])
                    row = {'shot': shot['name'], 'config': config['name'], 'seed': seed,
                           'gpu_ms': statistics.median(float(r['gpu_ms']) for r in timings[16:]),
                           'trace_ms': statistics.median(float(r['trace_ms']) for r in timings[16:]),
                           'flip': ldr_flip(Path(ref['reference']), png),
                           'secondary_flip': ldr_flip(Path(ref['independent_reference']), png),
                           'frames': shot['frames'], 'timing_frames': len(timings)-16,
                           'image': str(png), 'active_upscaler': states[-1]['active_upscaler'],
                           'gpu': self.hardware['gpu'], 'driver': self.hardware['driver']}
                    self.report['jobs'][name] = {'status': 'passed', 'row': row,
                        'idle_before': idle, 'finished_utc': utc_now(),
                        'files': {str(p): digest(p) for p in self.out.glob(name+'.*')}}
                    self.report.update(state='collected', completed_motion_runs=len(self.report['jobs']))
                    self.persist()
                    write_csv(self.root / 'runs.csv', [j['row'] for j in self.report['jobs'].values()])
                    print(f"COMPLETED {len(self.report['jobs'])}/27 {name} GPU={row['gpu_ms']:.3f}", flush=True)

    def repeat_controls(self):
        shot = self.spec['shots'][0]
        prepared = self.bank['scenes'][shot['name']]
        for config in self.configs:
            name = 'repeat-' + config['name']
            saved = self.report['repeat_controls'].get(name)
            if saved:
                assert all(digest(Path(p)) == sha for p, sha in saved['files'].items())
                continue
            self.idle_gate()
            png, states = self.render(name, Path(prepared['directory'])/'initial.yaml',
                prepared['poses']['initial'], 1280, 720, shot['frames'], 1, 17,
                config['flags']+self.motion_flags(shot), timing=True)
            self.validate_states(states, config, 17)
            first = self.report['jobs'][f"{shot['name']}--{config['name']}--17"]['row']['image']
            assert digest(png) == digest(Path(first)), f'Non-repeatable endpoint: {name}'
            self.report['repeat_controls'][name] = {'exact_endpoint_match': True,
                'files': {str(p): digest(p) for p in self.out.glob(name+'.*')}}
            self.persist()

    def analyze(self):
        rows = [j['row'] for j in self.report['jobs'].values()]
        assert len(rows) == 27
        summary = []
        for shot in self.spec['shots']:
            for config in self.configs:
                data = [r for r in rows if r['shot'] == shot['name'] and r['config'] == config['name']]
                assert sorted(r['seed'] for r in data) == sorted(self.spec['seeds'])
                item = {'shot': shot['name'], 'config': config['name'], 'seeds': len(data)}
                for metric in ['gpu_ms', 'trace_ms', 'flip', 'secondary_flip']:
                    values = [r[metric] for r in data]
                    item.update({metric+'_median': statistics.median(values),
                                 metric+'_min': min(values), metric+'_max': max(values)})
                summary.append(item)
        write_csv(self.root/'summary.csv', summary)
        fig, axes = plt.subplots(1, 3, figsize=(15, 4.8), constrained_layout=True)
        for ax, shot in zip(axes, self.spec['shots']):
            for row in [r for r in summary if r['shot'] == shot['name']]:
                ax.errorbar(row['gpu_ms_median'], row['flip_median'],
                    xerr=[[row['gpu_ms_median']-row['gpu_ms_min']], [row['gpu_ms_max']-row['gpu_ms_median']]],
                    yerr=[[row['flip_median']-row['flip_min']], [row['flip_max']-row['flip_median']]],
                    marker='o', capsize=3, label=row['config'])
            ax.set(title=shot['name'], xlabel='Median GPU ms/frame', ylabel='Endpoint LDR-FLIP, primary')
            ax.grid(alpha=.2)
        axes[-1].legend(fontsize=7)
        fig.suptitle('Fixed temporal input and exposure | 1280x720 | 3 seeds; ranges are not CI')
        fig.savefig(self.root/'quality-error-time.png', dpi=160)
        plt.close(fig)
        self.report['summary_rows'] = len(summary)

    def execute(self):
        try:
            self.calibrate()
            self.collect()
            self.repeat_controls()
            self.analyze()
            self.report.update(complete=True, state='complete')
            self.persist()
        except Paused as error:
            self.report.update(complete=False, state='paused', pause_reason=str(error))
            self.persist()
            print(f'PAUSED {error}', flush=True)
            return 75
        except BaseException as error:
            self.report.update(complete=False, state='failed', failure=str(error))
            self.persist()
            raise
        return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--references', type=Path, default=Path('renders/measurement'))
    parser.add_argument('--compatibility', type=Path, default=Path('renders/fixed-exposure-validation/reference-compatibility.json'))
    parser.add_argument('--config', type=Path, default=Path(__file__).with_name('shots.json'))
    parser.add_argument('--output', type=Path, default=Path('renders/quality-fixed-exposure'))
    parser.add_argument('--resume', action='store_true')
    raise SystemExit(Quality(parser.parse_args()).execute())
