"""Record the real StarTraveler window during its opt-in automatic flight route."""
import argparse
import json
import re
import subprocess
import time
from pathlib import Path

import imageio_ffmpeg
from check_measurement import digest
from run_gpu_regression import Paused, save


def stop_recording(process):
    if process is not None and process.poll() is None:
        try:
            process.communicate(b"q\n", timeout=15)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=5)


def main(args):
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    manifest = root / "manifest.json"
    if manifest.exists():
        raise ValueError("Output already has a recording; choose a fresh folder")
    pause = args.pause_file.resolve()
    ffmpeg = Path(imageio_ffmpeg.get_ffmpeg_exe())
    command = [str(args.exe.resolve()), "--upscaler", "DLSS", "--quality", "Quality",
               "--dlss-preset", "K", "--dlss-exposure", "1", "--no-viewports",
               "--rehearsal-seconds", str(args.seconds)]
    capture = [str(ffmpeg), "-hide_banner", "-n", "-f", "lavfi", "-i", "WINDOW_GEOMETRY",
               "-an", "-vf", "hwdownload,format=bgra,"
               "scale=1280:720:force_original_aspect_ratio=decrease,"
               "pad=1280:720:(ow-iw)/2:(oh-ih)/2,setsar=1",
               "-c:v", "libx264", "-preset", "ultrafast", "-crf", "20",
               "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(root / "flight.mp4")]
    report = {"complete": False, "state": "starting", "kind": "automatic_real_window",
              "manual_keyboard_rehearsal": False, "performance_measurement": False,
              "configuration_version": "fixed-exposure-v1",
              "expected": {"dlss_exposure": 1, "tone_exposure": 1,
                           "auto_exposure": False, "temporal_ms": 1000 / 60,
                           "tone": "ACES", "spp": 1, "seed": 29, "effects_off": True},
              "seconds_per_phase": args.seconds, "game_command": command,
              "capture_command": capture, "exe_sha256": digest(args.exe),
              "ffmpeg_sha256": digest(ffmpeg), "runner_sha256": digest(Path(__file__)),
              "sources": {str(p): digest(p) for p in
                          (Path("StarTraveler/src/main.cpp"),
                           Path("StarTraveler/src/game_layer.cpp"),
                           Path("StarTraveler/src/game_layer.hpp"))},
              "assets": {str(p): digest(p) for p in
                         (args.exe.resolve().parent / "assets").rglob("*")
                         if p.is_file() and p.suffix in (".yaml", ".glb")}}
    save(manifest, report)
    game = recorder = None
    began = last_progress = time.monotonic()
    seen_phases = {}
    log_path = root / "game.log"
    try:
        with log_path.open("w", encoding="utf-8") as game_log, \
                (root / "capture.log").open("w", encoding="utf-8") as capture_log:
            game = subprocess.Popen(command, stdout=game_log, stderr=subprocess.STDOUT)
            report["game_pid"] = game.pid
            save(manifest, report)
            while True:
                if pause.exists():
                    raise Paused(pause.read_text(encoding="utf-8-sig").strip())
                log = log_path.read_text(encoding="utf-8", errors="replace")
                for phase in re.findall(r"Rehearsal phase (\d):", log):
                    if phase not in seen_phases:
                        seen_phases[phase] = time.monotonic() - began
                        report["phase_seen_wall_seconds"] = dict(seen_phases)
                        save(manifest, report)
                if recorder is None and "Rehearsal phase 0:" in log:
                    geometry = re.search(r"Rehearsal window: x=(-?\d+) y=(-?\d+) "
                                         r"width=(\d+) height=(\d+)", log)
                    assert geometry, "Missing game-window bounds"
                    x, y, width, height = map(int, geometry.groups())
                    assert x >= 0 and y >= 0, "Recording currently requires the primary monitor"
                    capture[capture.index("-i") + 1] = (
                        f"ddagrab=output_idx=0:video_size={width}x{height}:"
                        f"offset_x={x}:offset_y={y}:framerate=30:draw_mouse=0")
                    report["capture_command"] = capture
                    report["window_bounds"] = [x, y, width, height]
                    recorder = subprocess.Popen(capture, stdin=subprocess.PIPE,
                                                stdout=capture_log, stderr=subprocess.STDOUT)
                    report.update(state="recording", recorder_pid=recorder.pid,
                                  recording_start_wall_seconds=time.monotonic() - began)
                    save(manifest, report)
                    print("Recording actual StarTraveler window", flush=True)
                if recorder is not None and recorder.poll() is not None:
                    if "Rehearsal complete:" not in log:
                        raise RuntimeError("Window recorder stopped early; see capture.log")
                if "Rehearsal complete:" in log or game.poll() is not None:
                    stop_recording(recorder)
                    game.wait(timeout=30)
                    break
                if time.monotonic() - began > 900:
                    raise RuntimeError("Window rehearsal exceeded 15-minute wall-time limit")
                if time.monotonic() - last_progress > 30:
                    phases = re.findall(r"Rehearsal phase (\d):", log)
                    print(f"Window rehearsal running; phases started: {phases}", flush=True)
                    last_progress = time.monotonic()
                time.sleep(.1)
        log = log_path.read_text()
        assert game.returncode == 0 and "Rehearsal complete:" in log
        assert re.findall(r"Rehearsal phase (\d):", log) == list("0123456")
        assert seen_phases["1"] > report["recording_start_wall_seconds"], "Missed route start"
        assert not re.search(r"\[[^\]]*(?:error|critical)[^\]]*\]|evaluating failed", log, re.I)
        samples = re.findall(r"Rehearsal sample: t=([\d.]+) phase=(\d).*?active=(\S+) "
                             r"internal=(\d+)x(\d+) output=(\d+)x(\d+)", log)
        assert set(s[1] for s in samples) == set("0123456")
        for sample in samples:
            expected = "DLSS-RR" if sample[1] in ("2", "4", "6") else "DLSS"
            assert sample[2] == expected, "Unexpected upscaler fallback"
            assert all(int(n) > 0 for n in sample[3:])
        from verify_rehearsal_video import validate_fixed_configuration
        report["configuration_checks"] = validate_fixed_configuration(log)
        assert recorder is not None and (root / "flight.mp4").stat().st_size > 100000
        report.update(complete=True, state="recorded_needs_video_review",
                      game_exit=game.returncode, recorder_exit=recorder.returncode,
                      sampled_frames=len(samples),
                      wall_seconds=time.monotonic() - began,
                      files={str(p): digest(p) for p in root.iterdir()
                             if p.is_file() and p != manifest})
        save(manifest, report)
        print("Seven phases recorded; video decoding and visual review still required", flush=True)
        return 0
    except BaseException as error:
        if game is not None and game.poll() is None:
            game.terminate()
            game.wait(timeout=10)
        stop_recording(recorder)
        report.update(state="paused" if isinstance(error, Paused) else "failed",
                      complete=False, reason=str(error))
        save(manifest, report)
        if isinstance(error, Paused):
            return 75
        raise
    finally:
        report.update(game_pid=None, recorder_pid=None)
        save(manifest, report)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--pause-file", type=Path,
                        default=Path("renders/window-rehearsal/pause.request"))
    raise SystemExit(main(parser.parse_args()))
