"""Exercise seven CLI presentation profiles and capture their real-window overlays."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time

import imageio_ffmpeg
from PIL import Image, ImageDraw

from check_measurement import digest
from run_gpu_regression import save


def main(args):
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    pause = root/'pause.request'
    path = root/'manifest.json'
    signature = {'exe': digest(args.exe), 'checker': digest(Path(__file__)),
                 'ffmpeg': digest(Path(imageio_ffmpeg.get_ffmpeg_exe()))}
    report = json.loads(path.read_text()) if path.exists() else {'signature': signature, 'profiles': {}}
    assert report['signature'] == signature, 'Inputs changed; choose another output directory'
    report.update(complete=False, state='running', manual_keyboard_verified=False,
                  performance_measurement=False)
    save(path, report)
    game = capture = None
    try:
        for preset in range(1, 8):
            if str(preset) in report['profiles']:
                saved = report['profiles'][str(preset)]
                assert digest(Path(saved['png'])) == saved['png_sha256']
                continue
            folder = root/f'profile-{preset}'
            folder.mkdir(exist_ok=True)
            command = [str(args.exe.resolve()), '--demo-preset', str(preset),
                       '--frames', '300', '--no-viewports']
            log_path = folder/'game.log'
            with log_path.open('w') as log:
                game = subprocess.Popen(command, cwd=folder, stdout=log, stderr=subprocess.STDOUT)
                began = time.monotonic()
                while True:
                    if pause.exists(): raise InterruptedError('pause.request')
                    if time.monotonic()-began > 240: raise TimeoutError('Profile startup timeout')
                    text = log_path.read_text(errors='replace')
                    if 'Demo state:' in text: break
                    assert game.poll() is None, text[-2000:]
                    time.sleep(.1)
                geometry = re.search(r'Demo window: x=(-?\d+) y=(-?\d+) width=(\d+) height=(\d+)',text)
                assert geometry
                x,y,width,height = map(int,geometry.groups())
                assert x >= 0 and y >= 0
                state = re.search(r'Demo state: preset=(\d+) matched=(\w+) pipeline=(.*?) active=(\S+) '
                                  r'output=(\d+)x(\d+) internal=(\d+)x(\d+) tone_exposure=(\S+) '
                                  r'auto_exposure=(\S+) spp=(\S+) overlay=(\S+)',text)
                assert state
                n,matched,pipeline,active,ow,oh,rw,rh,exposure,auto,spp,overlay = state.groups()
                assert int(n) == preset and matched == 'true' and overlay == 'true'
                assert active == ('DLSS-RR' if preset == 6 else 'Off')
                assert pipeline == ('Raster' if preset == 1 else 'Path tracing' if preset == 7 else 'Hybrid')
                assert float(exposure) == 1 and auto == 'false' and spp == '1'
                expected = [int(int(ow)/1.5+.5),int(int(oh)/1.5+.5)] if preset == 6 else [int(ow),int(oh)]
                assert [int(rw),int(rh)] == expected
                png = folder/'window.png'
                capture_command = [imageio_ffmpeg.get_ffmpeg_exe(), '-hide_banner', '-loglevel', 'error',
                    '-y', '-f', 'lavfi', '-i', f'ddagrab=output_idx=0:video_size={width}x{height}:'
                    f'offset_x={x}:offset_y={y}:framerate=30:draw_mouse=0',
                    '-vf', 'hwdownload,format=bgra', '-frames:v', '1', str(png)]
                with (folder/'capture.log').open('w') as stream:
                    capture = subprocess.Popen(capture_command, stdout=stream, stderr=subprocess.STDOUT,
                                               creationflags=subprocess.CREATE_NO_WINDOW)
                    began_capture = time.monotonic()
                    while capture.poll() is None:
                        if pause.exists(): raise InterruptedError('pause.request')
                        if time.monotonic()-began_capture > 20: raise TimeoutError('Window capture timeout')
                        time.sleep(.1)
                assert capture.returncode == 0, (folder/'capture.log').read_text()
                game.wait(timeout=20)
                assert game.returncode == 0
            with Image.open(png) as image:
                assert image.size == (width,height)
            report['profiles'][str(preset)] = {'command': command, 'pipeline': pipeline,
                'active_upscaler': active, 'output': [int(ow),int(oh)], 'internal': [int(rw),int(rh)],
                'png': str(png), 'png_sha256': digest(png), 'game_exit': game.returncode,
                'log_sha256': digest(log_path), 'capture_command': capture_command}
            save(path, report)
            game = capture = None
            print(f'PROFILE {preset}/7: {pipeline}, active={active}, real window captured',flush=True)
        sheet = Image.new('RGB',(1200,4*363),'#151515')
        draw = ImageDraw.Draw(sheet)
        for index,saved in enumerate(report['profiles'].values()):
            with Image.open(saved['png']) as image:
                image = image.convert('RGB'); image.thumbnail((600,338))
                x,y = index%2*600,index//2*363
                sheet.paste(image,(x,y+25))
                draw.text((x+5,y+5),f"{index+1}: {saved['pipeline']}, {saved['active_upscaler']}",fill='white')
        sheet.save(root/'contact-sheet.jpg')
        report.update(complete=True,state='captured_needs_visual_review')
    except BaseException as error:
        report.update(state='paused' if pause.exists() else 'failed',reason=str(error))
        raise
    finally:
        for process in [capture,game]:
            if process is not None and process.poll() is None:
                process.terminate(); process.wait(timeout=10)
        save(path, report)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=Path('renders/demo-presets'))
    main(parser.parse_args())
