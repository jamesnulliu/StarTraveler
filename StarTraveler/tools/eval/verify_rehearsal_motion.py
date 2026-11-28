"""Check real-window per-phase motion and logged stationary holds after video QA."""
import argparse
import json
from pathlib import Path
import re
import subprocess

import imageio_ffmpeg
import numpy as np
from PIL import Image

from check_measurement import digest


def main(root):
    root = root.resolve()
    manifest = json.loads((root / 'manifest.json').read_text())
    base = json.loads((root / 'verification.json').read_text())
    assert base['passed'] and manifest['configuration_version'] == 'fixed-exposure-v1'
    assert base['video_sha256'] == digest(root / 'flight.mp4')
    states = []
    for match in re.finditer(r'Rehearsal sample: t=([\d.]+) phase=(\d) '
                            r'camera=\(([^)]+)\) yaw=([-\d.]+)',
                            (root / 'game.log').read_text()):
        t, phase, camera, yaw = match.groups()
        states.append((float(t), int(phase), camera, yaw))
    report = {'passed': False, 'phases': [], 'video_sha256': base['video_sha256'],
              'verifier_sha256': digest(Path(__file__)),
              'interpretation': 'Frozen-video check and logged camera holds, not FLIP or latency'}
    try:
        for i, chapter in enumerate(base['chapters']):
            duration = manifest['seconds_per_phase']
            # Leave one sample of margin around the movement-to-hold boundary.
            hold = [s for s in states if s[1] == i and
                    s[0] - i * duration >= duration * .8 + .5]
            assert len(hold) >= 2 and len({s[2:] for s in hold}) == 1, (i, hold)
            assert len({s[2:] for s in states if s[1] == i}) > 1
            pictures = []
            for tag, fraction in [('early', .2), ('late', .7)]:
                stamp = chapter['start_s'] + fraction * (chapter['end_s'] - chapter['start_s'])
                output = root / f'motion-{i+1}-{tag}.png'
                result = subprocess.run([
                    imageio_ffmpeg.get_ffmpeg_exe(), '-hide_banner', '-loglevel', 'error',
                    '-ss', str(stamp), '-i', str(root / 'flight.mp4'), '-frames:v', '1',
                    '-y', str(output)], capture_output=True, text=True, timeout=30)
                assert result.returncode == 0, result.stderr
                with Image.open(output) as im:
                    pictures.append(np.asarray(im.convert('RGB'), dtype=np.float32)
                                    [140:650, 100:1180] / 255)
            change = float(np.mean(np.abs(pictures[0] - pictures[1])))
            assert change > .001, f'Phase {i+1} appears frozen'
            report['phases'].append({'phase': i+1, 'motion_mean_absolute_change': change,
                                     'stationary_camera_log_samples': len(hold),
                                     'stationary_camera_and_yaw': hold[-1][2:]})
        report['passed'] = True
    finally:
        (root / 'motion-verification.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    main(parser.parse_args().root)
