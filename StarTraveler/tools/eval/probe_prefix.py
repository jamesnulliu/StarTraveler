"""Compare short/long command prefixes; stop owned renders after closed PNGs exist."""
import argparse
import json
import os
import subprocess
import time
from pathlib import Path

from PIL import Image
from check_measurement import digest
from run_gpu_regression import save, Paused


def main(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    path = out / "manifest.json"
    base = json.loads(args.template.read_text())
    report = {"complete": False, "exe_sha256": digest(args.exe), "results": {}}
    report['diagnostic_environment'] = {key: os.environ.get(key) for key in
        ('STARCORE_TRACE_INPUTS', 'STARCORE_TRACE_LINEAR', 'STARCORE_DLSS_PROBE_EXPOSURE')}
    if path.exists():
        assert args.resume, "Choose a fresh output directory or --resume"
        previous = json.loads(path.read_text())
        assert previous['exe_sha256'] == report['exe_sha256']
        assert previous['diagnostic_environment'] == report['diagnostic_environment']
        report = previous
    save(path, report)
    try:
        for zero in ((False, True) if args.zero_check else (False,)):
            for label, total, still in (("short", args.short_frames, 0),
                                         ("long", args.long_frames,
                                          args.long_frames - args.short_frames)):
                name = f"{'zero' if zero else 'default'}-{label}"
                if args.pause_file.exists():
                    raise Paused(args.pause_file.read_text(encoding="utf-8-sig"))
                command = (json.loads(args.long_template.read_text())
                           if label == "long" and args.long_template else list(base))
                command[0] = str(args.exe.resolve())
                command[command.index("-f") + 1] = str(total)
                for flag, suffix in (("-o", ".png"), ("--frame-stats", ".states.csv")):
                    if flag in command:
                        command[command.index(flag) + 1] = str(out / (name + suffix))
                if "--still" in command:
                    command[command.index("--still") + 1] = str(still)
                elif still:
                    command += ["--still", str(still)]
                env = dict(os.environ)
                env.pop("STARCORE_ZERO_HISTORY_DIAGNOSTIC", None)
                if zero:
                    env["STARCORE_ZERO_HISTORY_DIAGNOSTIC"] = "1"
                if name in report['results']:
                    assert json.loads((out / (name + '.command.json')).read_text()) == command
                    assert all(digest(out / f'{name}_{int(i):04d}.png') == sha
                               for i, sha in report['results'][name].items())
                    continue
                stale = list(out.glob(name + '*'))
                if stale:
                    archive = out / 'interrupted' / f'{name}-{time.time_ns()}'
                    archive.mkdir(parents=True)
                    for artifact in stale:
                        assert artifact.is_file() and artifact.resolve().parent == out
                        artifact.rename(archive / artifact.name)
                save(out / (name + ".command.json"), command)
                with (out / (name + ".log")).open("w", encoding="utf-8") as log:
                    process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
                    try:
                        report.update(active=name, active_pid=process.pid)
                        save(path, report)
                        print("START " + name, flush=True)
                        began = time.monotonic()
                        while process.poll() is None:
                            if args.pause_file.exists():
                                raise Paused(args.pause_file.read_text(encoding="utf-8-sig"))
                            target = out / f"{name}_{args.prefix:04d}.png"
                            if target.exists():
                                try:
                                    with Image.open(target) as image:
                                        image.load()
                                    break
                                except OSError:
                                    pass
                            if time.monotonic() - began > args.timeout:
                                raise RuntimeError("Prefix probe exceeded timeout")
                            time.sleep(.1)
                    finally:
                        if process.poll() is None:
                            process.terminate()
                        process.wait(timeout=10)
                        report["active_pid"] = None
                report["results"][name] = {str(i): digest(out / f"{name}_{i:04d}.png")
                                           for i in range(2, args.prefix + 1)}
                save(path, report)
                print("PREFIX SAVED " + name, flush=True)
        labels = ("default", "zero") if args.zero_check else ("default",)
        report["differences"] = {label: [i for i in range(2, args.prefix + 1)
                if report["results"][label + "-short"][str(i)] !=
                   report["results"][label + "-long"][str(i)]] for label in labels}
        report.update(complete=True, state="complete")
        print(json.dumps(report["differences"]), flush=True)
        return 0
    except Paused as error:
        report.update(state="paused", reason=str(error))
        return 75
    finally:
        save(path, report)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--template", type=Path,
                        default=Path("renders/fixed-step-validation/motion-dumped.command.json"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--long-template", type=Path)
    parser.add_argument("--prefix", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=1200)
    parser.add_argument("--short-frames", type=int, default=81)
    parser.add_argument("--long-frames", type=int, default=593)
    parser.add_argument("--zero-check", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--pause-file", type=Path, default=Path("renders/prefix-diagnosis/pause.request"))
    raise SystemExit(main(parser.parse_args()))
