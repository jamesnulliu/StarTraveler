"""Strict, resumable reference and real-scene temporal-prefix preflight."""
import argparse
import json
import shutil
import time
from pathlib import Path

import numpy as np
from PIL import Image

from check_measurement import digest, read_csv
from run_gpu_regression import Regression, Paused, save


def main(args):
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    previous = args.previous.resolve()
    exe = args.exe.resolve()
    runner = Regression.__new__(Regression)
    runner.root, runner.path = root, root / 'manifest.json'
    runner.pause = root / 'pause.request'
    signature = {'renderer': digest(exe), 'script': digest(Path(__file__)),
                 'fixed_dlss_exposure': 1.0,
                 'process_helper': digest(Path(__file__).with_name('run_gpu_regression.py')),
                 'previous_manifest': digest(previous / 'manifest.json')}
    if runner.path.exists():
        runner.report = json.loads(runner.path.read_text())
        assert runner.report['signature'] == signature, 'Inputs changed; use a fresh directory'
    else:
        runner.report = {'complete': False, 'results': {}, 'signature': signature}
    runner.report.update(complete=False, fixed_prefix_identical=False)

    def capture(name, template, flags=()):
        runner.check_pause()
        command = json.loads(template.read_text())
        command[0] = str(exe)
        if '--upscaler' in command and command[command.index('--upscaler') + 1] == 'DLSS':
            command += ['--dlss-exposure', '1']
        for flag, suffix in (('-o', '.png'), ('--frame-stats', '.states.csv'),
                             ('--gpu-timings', '.timings.csv')):
            if flag in command:
                command[command.index(flag) + 1] = str(root / (name + suffix))
        command += list(flags)
        cached = runner.report['results'].get(name)
        if cached:
            assert cached['command'] == command
            for path, sha in cached['files'].items():
                assert digest(Path(path)) == sha
            return root / (name + '.png')
        stale = list(root.glob(name + '*'))
        if stale:
            archive = root / 'interrupted' / f'{name}-{time.time_ns()}'
            archive.mkdir(parents=True)
            for path in stale:
                assert path.resolve().parent == root
                shutil.move(str(path), archive / path.name)
        save(root / (name + '.command.json'), command)
        runner.report['active_test'] = name
        print('START ' + name, flush=True)
        code, _ = runner.run_process(command, Path.cwd(), root / (name + '.log'))
        assert code == 0, name
        runner.report['results'][name] = {'command': command,
            'files': {str(p): digest(p) for p in sorted(root.glob(name + '*')) if p.is_file()}}
        runner.persist()
        return root / (name + '.png')

    try:
        # Directly reject invalid first-frame motion before the longer captures.
        motion_template = json.loads((previous / 'motion-dumped.command.json').read_text())
        motion_template[motion_template.index('-f') + 1] = '1'
        motion_template[motion_template.index('--upscaler') + 1] = 'Off'
        path = root / 'template-initial-motion.json'
        save(path, motion_template)
        png = capture('initial-motion', path, ['--view', 'Motion'])
        rgb = np.array(Image.open(png))[:, :, :3]
        runner.report['initial_nonzero_motion_pixels'] = int(np.any(rgb, axis=2).sum())
        assert not np.any(rgb), 'Stationary first frame has invalid motion'
        old = json.loads((previous / 'manifest.json').read_text())
        compatibility = json.loads((previous / 'reference-compatibility.json').read_text())
        controls = {}
        for shot, hashes in old['results'].items():
            native = previous / f'reference-control-{shot}-old.png'
            assert digest(native) == hashes['old_sha256']
            png = capture(f'reference-{shot}',
                          previous / f'reference-control-{shot}-fixed.command.json')
            assert digest(png) == digest(native), 'Native reference path changed'
            controls[shot] = {'old_sha256': digest(native), 'fixed_sha256': digest(png)}
        timed = capture('motion-timed', previous / 'motion.command.json')
        capture('motion-dumped', previous / 'motion-dumped.command.json')
        capture('still', previous / 'still.command.json')
        states = [read_csv(root / (name + '.states.csv')) for name in
                  ('motion-timed', 'motion-dumped', 'still')]
        assert [len(s) for s in states] == [81, 81, 593]
        assert states[0] == states[1] == states[2][:81], 'Motion-prefix states differ'
        assert all(abs(float(row['frame_time_ms']) - 1000 / 60) <= 1e-5
                   for rows in states for row in rows), 'Time step changed'
        assert all(float(row['dlss_exposure']) == 1.0 for rows in states for row in rows)
        assert digest(timed) == digest(root / 'still_0081.png'), 'Timed endpoint differs'
        differences = [i for i in range(2, 82)
                       if digest(root / f'motion-dumped_{i:04d}.png') !=
                          digest(root / f'still_{i:04d}.png')]
        runner.report['different_moving_frames'] = differences
        assert not differences, 'Dumped motion prefix differs'
        save(root / 'reference-compatibility.json', {
            'passed': True, 'renderer_sha256': digest(exe), 'controls': controls,
            'reference_renderer_sha256': compatibility['reference_renderer_sha256'],
            'scope': 'Three 32-spp native endpoint controls; manual exposure and effects off.'})
        runner.report.update(complete=True, state='complete', fixed_prefix_identical=True,
                             renderer_sha256=digest(exe))
        print('REFERENCE, INITIAL MOTION AND FULL PREFIX PASSED', flush=True)
        return 0
    except Paused as error:
        runner.report.update(state='paused', reason=str(error))
        return 75
    except BaseException as error:
        runner.report.update(state='failed', failure=str(error))
        raise
    finally:
        runner.persist()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--previous', type=Path, default=Path('renders/fixed-step-validation'))
    parser.add_argument('--output', type=Path, default=Path('renders/fixed-exposure-validation'))
    raise SystemExit(main(parser.parse_args()))
