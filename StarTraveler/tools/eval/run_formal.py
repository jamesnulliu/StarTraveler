"""Alternating, resumable denoiser sweep using the validated reference bank."""
import argparse
import csv
import ctypes
import importlib.metadata
import json
import math
import os
import queue
import re
import statistics
import subprocess
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

from check_measurement import Check, camera_flags, digest, read_csv, write_csv
import matplotlib.pyplot as plt
from PIL import Image, ImageDraw, ImageFont
from metrics import ldr_flip, settling


class Paused(RuntimeError):
    pass


def atomic_replace(source, target):
    for attempt in range(50):
        try:
            source.replace(target)
            return
        except PermissionError:
            if attempt == 49:
                raise
            time.sleep(.1)


def atomic_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False), encoding="utf-8")
    atomic_replace(temporary, path)


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def ac_power():
    class PowerStatus(ctypes.Structure):
        _fields_ = [("ac", ctypes.c_ubyte), ("flags", ctypes.c_ubyte),
                    ("percent", ctypes.c_ubyte), ("reserved", ctypes.c_ubyte),
                    ("life", ctypes.c_uint32), ("full_life", ctypes.c_uint32)]
    status = PowerStatus()
    if not ctypes.windll.kernel32.GetSystemPowerStatus(ctypes.byref(status)):
        raise OSError("Cannot read Windows AC power status")
    return int(status.ac)


class Formal(Check):
    def __init__(self, args):
        self.args = args
        self.root = args.output.resolve()
        self.out = self.root / "data"
        self.out.mkdir(parents=True, exist_ok=True)
        self.pause_path = self.root / "pause.request"
        self.report_path = self.root / "manifest.json"
        self.spec = json.loads(args.config.read_text(encoding="utf-8-sig"))
        protocol_path = args.config.parent / self.spec["protocol"]
        self.protocol = json.loads(protocol_path.read_text())
        self.common = self.protocol["common_flags"].copy()
        self.configs = [c for c in self.protocol["configurations"] if c["group"] != "diagnostic-only"]
        self.total_runs = len(self.spec["shots"]) * len(self.spec["seeds"]) * len(self.configs)
        self.bank = json.loads((args.references / "measurement.json").read_text())
        if not self.bank.get("preflight_complete"):
            raise ValueError("Reference bank has not passed measurement validation")
        self.hardware = self.telemetry()
        self.fingerprints = {
            "renderer_sha256": digest(args.exe), "config_sha256": digest(args.config),
            "protocol_sha256": digest(protocol_path),
            "metrics_sha256": digest(Path(__file__).with_name("metrics.py")),
            "preparer_sha256": digest(args.prepare_exe),
            "source_scene_sha256": digest(args.assets / self.spec["source_scene"]),
            "models": {str(p.relative_to(args.assets)): digest(p) for p in sorted(args.assets.rglob("*.glb"))}}
        if self.bank["renderer_sha256"] != self.fingerprints["renderer_sha256"]:
            raise ValueError("Reference bank renderer differs")
        if self.bank["config_sha256"] != self.fingerprints["config_sha256"]:
            raise ValueError("Reference bank shots differ")
        for key, value in self.bank["input_hashes"].items():
            if value != self.fingerprints[key]:
                raise ValueError(f"Reference input differs: {key}")
        policy = {"width": 1280, "height": 720, "seeds": self.spec["seeds"],
                  "require_ac_power": True, "hardware_sampling_seconds": .5,
                  "calibration_runs": 3, "calibration_max_relative_range": .2,
                  "activity_monitor_sha256": digest(Path(__file__).with_name("gpu_activity.ps1")),
                  "external_app_engine_peak_max": 1.0,
                  "os_processes": ["dwm", "System", "Idle"],
                  "warmup_frames": 16, "ppd": 67, "dump_every": 0,
                  "idle_median_max": 5, "idle_peak_max": 10, "idle_samples": 3,
                  "lag_size": [384, 216], "lag_horizons": [512, 1024, 2048],
                  "lag_configs": ["asvgf-dlss-full", "rr-quality"],
                  "lag_seed": 17, "tail": 64, "drift_limit": .03, "tolerance": .10, "hold": 16}
        signature = {"inputs": self.fingerprints, "policy": policy,
                     "gpu": self.hardware["gpu"], "driver": self.hardware["driver"]}
        if self.report_path.exists():
            self.report = json.loads(self.report_path.read_text())
            if self.report["signature"] != signature:
                raise ValueError("Inputs/hardware/policy changed; use another output directory")
        else:
            self.report = {"version": 1, "created_utc": utc_now(), "signature": signature,
                           "reference_bank": str(args.references.resolve()), "jobs": {}, "lag": {}}
        if args.clear_pause and self.pause_path.exists():
            self.pause_path.unlink()
        self.report.update(complete=False, state="preparing", controller_pid=os.getpid(),
                           hardware=self.hardware, collector_sha256=digest(Path(__file__)),
                           versions={p: importlib.metadata.version(p) for p in
                                     ("flip-evaluator", "numpy", "Pillow", "matplotlib")})
        self.report.pop("pause_reason", None)
        for shot in self.spec["shots"]:
            scene = self.bank["scenes"][shot["name"]]
            for role in ("initial", "end"):
                if digest(Path(scene["directory"]) / f"{role}.yaml") != scene[f"{role}_sha256"]:
                    raise ValueError("Prepared scene changed")
            ref = self.reference(shot["name"])
            if digest(Path(ref["reference"])) != ref["sha256"][0]:
                raise ValueError("Reference PNG changed")
        self.persist()

    def telemetry(self):
        result = subprocess.run(["nvidia-smi", "--query-gpu=name,driver_version,memory.total,"
            "utilization.gpu,temperature.gpu,power.draw,pstate,clocks.gr,clocks.mem",
            "--format=csv,noheader,nounits"],
            capture_output=True, text=True, check=True, timeout=5)
        devices = list(csv.reader(result.stdout.strip().splitlines()))
        if len(devices) != 1:
            raise ValueError("This collection requires one unambiguously selected NVIDIA GPU")
        gpu, driver, memory, util, temperature, power, pstate, core, mem = [v.strip() for v in devices[0]]
        return {"gpu": gpu, "driver": driver, "vram_mib": int(memory), "utilization": int(util),
                "temperature_c": int(temperature), "power_w": float(power), "utc": utc_now(),
                "pstate": pstate, "graphics_mhz": int(core), "memory_mhz": int(mem),
                "ac_online": ac_power()}

    def reference(self, shot, size="1280x720"):
        return self.bank["references"][f"{shot}-{size}"]

    def persist(self):
        self.report["updated_utc"] = utc_now()
        atomic_json(self.report_path, self.report)

    def check_pause(self):
        if self.pause_path.exists():
            raise Paused(self.pause_path.read_text(encoding="utf-8-sig").strip())
        if getattr(self, "require_ac", True) and ac_power() != 1:
            raise Paused("ac_power_required: disconnected or unknown power status")

    def idle_gate(self):
        began = time.monotonic()
        while True:
            self.check_pause()
            samples = []
            for _ in range(3):
                sample = self.telemetry()
                if (sample["gpu"], sample["driver"]) != (self.hardware["gpu"], self.hardware["driver"]):
                    raise RuntimeError("GPU or driver changed during the sweep")
                samples.append(sample)
                self.check_pause()
                time.sleep(1)
            utilization = [s["utilization"] for s in samples]
            if statistics.median(utilization) <= 5 and max(utilization) <= 10:
                return samples
            self.report.update(state="waiting_for_idle_gpu", idle_samples=samples)
            self.persist()
            print(f"WAIT idle GPU: {utilization}", flush=True)
            if time.monotonic() - began > 120:
                raise Paused("gpu_busy: no clean idle baseline within 120 seconds")

    def run(self, name, command):
        self.check_pause()
        print(f"START {name}", flush=True)
        command = list(map(str, command))
        atomic_json(self.out / f"{name}.command.json", command)
        log_path = self.out / f"{name}.log"
        monitor = None
        monitor_log = None
        activity = self.out / f"{name}.activity.csv"
        if getattr(self, "monitor_activity", True):
            monitor_log = (self.out / f"{name}.activity-monitor.log").open("w", encoding="utf-8")
            try:
                monitor = self.start_activity_monitor(activity, monitor_log)
            except BaseException:
                monitor_log.close()
                raise
        lines = queue.Queue()
        try:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, encoding="utf-8")
        except BaseException:
            if monitor is not None:
                monitor.terminate()
                monitor.wait(timeout=5)
            if monitor_log is not None:
                monitor_log.close()
            raise
        self.report.update(active_job=name, active_pid=process.pid, state="rendering")
        hardware_stop = threading.Event()
        hardware_errors = []

        def record_hardware():
            try:
                with (self.out / f"{name}.hardware.csv").open("w", newline="", encoding="utf-8") as stream:
                    writer = None
                    while not hardware_stop.is_set():
                        sample = self.telemetry()
                        if sample["ac_online"] != 1:
                            raise Paused("ac_power_required: hardware sample was not on AC")
                        if writer is None:
                            writer = csv.DictWriter(stream, fieldnames=list(sample))
                            writer.writeheader()
                        writer.writerow(sample)
                        stream.flush()
                        hardware_stop.wait(.5)
            except BaseException as error:
                hardware_errors.append(str(error))

        hardware_thread = None
        if getattr(self, "monitor_hardware", True):
            hardware_thread = threading.Thread(target=record_hardware, daemon=True)

        def read_output():
            for line in process.stdout:
                lines.put(line)
            lines.put(None)

        thread = threading.Thread(target=read_output, daemon=True)
        ended = False
        try:
            self.persist()
            if hardware_thread is not None:
                hardware_thread.start()
            thread.start()
            with log_path.open("w", encoding="utf-8") as stream:
                while not ended or process.poll() is None:
                    self.check_pause()
                    if hardware_errors:
                        raise Paused(f"hardware_monitor_failed: {hardware_errors[0]}")
                    if monitor is not None:
                        if monitor.poll() is not None:
                            raise Paused(f"GPU activity monitor exited ({monitor.returncode}); "
                                         f"see {name}.activity-monitor.log")
                        self.check_activity(activity, process.pid)
                    try:
                        line = lines.get(timeout=.2)
                    except queue.Empty:
                        continue
                    if line is None:
                        ended = True
                    else:
                        stream.write(line)
                        stream.flush()
                        print(line.rstrip(), flush=True)
            code = process.wait()
        except BaseException:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            raise
        finally:
            hardware_stop.set()
            if hardware_thread is not None and hardware_thread.ident is not None:
                hardware_thread.join(timeout=6)
            if monitor is not None:
                monitor.terminate()
                monitor.wait(timeout=5)
            if monitor_log is not None:
                monitor_log.close()
            self.report["active_pid"] = None
            self.persist()
        if hardware_errors or (hardware_thread is not None and hardware_thread.is_alive()):
            raise Paused(f"hardware_monitor_failed: {hardware_errors}")
        if hardware_thread is not None and len(read_csv(self.out / f"{name}.hardware.csv")) < 2:
            raise Paused("hardware_monitor_failed: insufficient samples")
        if monitor is not None:
            if not activity.exists():
                raise Paused("GPU activity counter produced no samples")
            self.check_activity(activity, process.pid)
        log = log_path.read_text()
        if code or re.search(r"/(error|critical)\]", log) or "evaluating failed" in log:
            raise RuntimeError(f"Renderer failed: {name}")
        if self.hardware["gpu"] not in log:
            raise RuntimeError("Renderer GPU does not match captured hardware")

    def start_activity_monitor(self, activity, stream):
        # PDH can reject the very first sample while GPU engine instances change.
        # Retry before launching the renderer; never bridge a gap during rendering.
        for attempt in range(3):
            self.check_pause()
            activity.unlink(missing_ok=True)
            stream.write(f"Counter startup attempt {attempt + 1}\n")
            stream.flush()
            monitor = subprocess.Popen([
                r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe",
                "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                str(Path(__file__).with_name("gpu_activity.ps1")), "-OutputPath", str(activity)],
                stdout=subprocess.DEVNULL, stderr=stream)
            try:
                deadline = time.monotonic() + 5
                while monitor.poll() is None and time.monotonic() < deadline:
                    self.check_pause()
                    if activity.exists():
                        try:
                            if any(r.get("process") == "heartbeat" for r in read_csv(activity)):
                                return monitor
                        except OSError:
                            pass
                    time.sleep(.1)
            except BaseException:
                if monitor.poll() is None:
                    monitor.terminate()
                    monitor.wait(timeout=5)
                raise
            if monitor.poll() is None:
                monitor.terminate()
            monitor.wait(timeout=5)
        raise Paused("GPU activity monitor startup failed three times; see activity-monitor.log")

    @staticmethod
    def check_activity(path, render_pid):
        if not path.exists():
            return
        # A writer may currently be appending a row; inspect only complete CSV lines.
        import io
        content = path.read_text(encoding="utf-8-sig")
        content = content[:content.rfind("\n") + 1]
        for row in csv.DictReader(io.StringIO(content)):
            if not row.get("percent"):
                continue
            if (int(row["pid"]) != render_pid and row["process"] not in
                    ("dwm", "System", "Idle", "heartbeat") and float(row["percent"]) > 1):
                raise Paused(f"background_gpu: {row['process']} pid={row['pid']} "
                             f"engine={float(row['percent']):.2f}%")

    def motion_flags(self, shot):
        flags = ["--move", *map(str, shot["move"]), "--move-entity", "Ship", *map(str, shot["move"])]
        if shot["turn"]:
            flags += ["--turn-entity", "Ship", str(shot["turn"])]
        return flags

    def validate_states(self, states, config, seed):
        for index, state in enumerate(states):
            expected_seed = seed + index if config["name"] == "raw-path" else seed
            age = index + 1 if config["accumulates"] else 1
            reset = int(config["upscaler"] != "Off" and index == 0)
            if (int(state["seed"]) != expected_seed or int(state["accumulated_frames"]) != age or
                int(state["upscaler_history_reset"]) != reset):
                raise RuntimeError("Unexpected sample/history state")

    def cached(self, name):
        job = self.report["jobs"].get(name)
        if not job or job.get("status") != "passed":
            return None
        for path, sha in job["files"].items():
            if not Path(path).exists() or digest(Path(path)) != sha:
                raise ValueError(f"Completed artifact was modified: {path}")
        return job

    def calibrate(self):
        shot = self.spec["shots"][0]
        prepared = self.bank["scenes"][shot["name"]]
        config = next(c for c in self.configs if c["name"] == "rr-quality")
        calibrations = self.report.setdefault("calibration", {})
        for repeat in range(3):
            self.check_pause()
            name = f"power-calibration-{repeat}"
            if name in calibrations:
                for path, sha in calibrations[name]["files"].items():
                    if digest(Path(path)) != sha:
                        raise ValueError("Calibration artifact changed")
                continue
            idle = self.idle_gate()
            _, states = self.render(name, Path(prepared["directory"]) / "initial.yaml",
                        prepared["poses"]["initial"], 1280, 720, shot["frames"], 1, 17,
                        config["flags"] + self.motion_flags(shot), timing=True)
            self.validate_states(states, config, 17)
            samples = read_csv(self.out / f"{name}.timings.csv")
            if [int(r["frame"]) for r in samples] != list(range(1, shot["frames"] + 1)):
                raise RuntimeError("Incomplete calibration timestamps")
            median = statistics.median(float(r["gpu_ms"]) for r in samples[16:])
            if not math.isfinite(median) or median <= 0:
                raise RuntimeError("Invalid calibration timing")
            files = list(self.out.glob(f"{name}.*"))
            calibrations[name] = {"gpu_ms": median, "idle_before": idle,
                                  "files": {str(p): digest(p) for p in files}}
            self.persist()
            print(f"CALIBRATION {repeat + 1}/3 GPU={median:.3f}", flush=True)
        values = [c["gpu_ms"] for c in calibrations.values()]
        spread = (max(values) - min(values)) / statistics.median(values)
        self.report["calibration_relative_range"] = spread
        self.persist()
        if spread > .2:
            raise Paused(f"unstable_ac_calibration: relative range {spread:.3f} exceeds .20")
        print(f"CALIBRATED GPU={values} relative_range={spread:.4f}", flush=True)

    def collect(self):
        completed = sum(j.get("status") == "passed" for j in self.report["jobs"].values())
        for shot in self.spec["shots"]:
            prepared = self.bank["scenes"][shot["name"]]
            scene = Path(prepared["directory"]) / "initial.yaml"
            for round_index, seed in enumerate(self.spec["seeds"]):
                shift = round_index * 3
                configs = self.configs[shift:] + self.configs[:shift]
                for config in configs:
                    self.check_pause()
                    name = f"{shot['name']}--{config['name']}--{seed}"
                    if self.cached(name):
                        continue
                    # Check inputs at each launch; never silently mix renderer/protocol revisions.
                    if digest(self.args.exe) != self.fingerprints["renderer_sha256"]:
                        raise ValueError("Renderer changed during collection")
                    if digest(self.args.config) != self.fingerprints["config_sha256"]:
                        raise ValueError("Shot configuration changed during collection")
                    idle = self.idle_gate()
                    flags = config["flags"] + self.motion_flags(shot)
                    png, states = self.render(name, scene, prepared["poses"]["initial"],
                        1280, 720, shot["frames"], 1, seed, flags, timing=True)
                    self.validate_states(states, config, seed)
                    timing_path = self.out / f"{name}.timings.csv"
                    samples = read_csv(timing_path)
                    if [int(r["frame"]) for r in samples] != list(range(1, shot["frames"] + 1)):
                        raise RuntimeError("Timestamp indices do not match the submitted frames")
                    for i, row in enumerate(samples):
                        gpu, trace = float(row["gpu_ms"]), float(row["trace_ms"])
                        if not (math.isfinite(gpu) and math.isfinite(trace) and gpu > 0 and 0 <= trace <= gpu + .001):
                            raise RuntimeError("Invalid GPU interval")
                        if int(row["setup_frame"]) != int(i == 0):
                            raise RuntimeError("Unexpected setup marker")
                    stable = samples[16:]
                    ref = self.reference(shot["name"])
                    flip = ldr_flip(Path(ref["reference"]), png)
                    row = {"shot": shot["name"], "config": config["name"], "seed": seed,
                        "gpu_ms": statistics.median(float(r["gpu_ms"]) for r in stable),
                        "trace_ms": statistics.median(float(r["trace_ms"]) for r in stable), "flip": flip,
                        "group": config["group"], "gpu": self.hardware["gpu"], "driver": self.hardware["driver"],
                        "width": 1280, "height": 720, "render_width": int(states[-1]["render_width"]),
                        "render_height": int(states[-1]["render_height"]), "active_upscaler": states[-1]["active_upscaler"],
                        "warmup_frames": 16, "timing_frames": len(stable), "frames": shot["frames"],
                        "reference_noise": ref["reference_pair_flip"], "ppd": 67, "image": str(png)}
                    files = [png, self.out / f"{name}.states.csv", timing_path,
                             self.out / f"{name}.activity.csv",
                             self.out / f"{name}.hardware.csv",
                             self.out / f"{name}.command.json", self.out / f"{name}.log"]
                    monitor_log = self.out / f"{name}.activity-monitor.log"
                    if monitor_log.exists():
                        files.append(monitor_log)
                    self.report["jobs"][name] = {"status": "passed", "row": row,
                        "collector_sha256": digest(Path(__file__)),
                        "idle_before": idle, "telemetry_after": self.telemetry(),
                        "files": {str(p): digest(p) for p in files}, "finished_utc": utc_now()}
                    completed += 1
                    self.report.update(state="collected", completed_motion_runs=completed)
                    self.persist()
                    write_csv(self.root / "runs.csv", [j["row"] for j in self.report["jobs"].values()
                                                       if j.get("status") == "passed"])
                    print(f"COMPLETED {completed}/{self.total_runs} {name} FLIP={flip:.6f} GPU={row['gpu_ms']:.3f}", flush=True)

    def analyze(self):
        rows = [j["row"] for j in self.report["jobs"].values() if j.get("status") == "passed"]
        if len(rows) != self.total_runs:
            raise ValueError(f"Expected {self.total_runs} completed motion runs, got {len(rows)}")
        summary = []
        for shot in self.spec["shots"]:
            for config in self.configs:
                data = [r for r in rows if r["shot"] == shot["name"] and r["config"] == config["name"]]
                if sorted(r["seed"] for r in data) != sorted(self.spec["seeds"]):
                    raise ValueError("Seed set differs from the declared protocol")
                item = {"shot": shot["name"], "group": config["group"], "config": config["name"],
                        "gpu": self.hardware["gpu"], "driver": self.hardware["driver"],
                        "output": "1280x720", "internal": f"{data[0]['render_width']}x{data[0]['render_height']}",
                        "runs": len(data), "reference_noise": data[0]["reference_noise"]}
                for metric in ("gpu_ms", "trace_ms", "flip"):
                    values = [r[metric] for r in data]
                    item.update({f"{metric}_median": statistics.median(values),
                                 f"{metric}_min": min(values), f"{metric}_max": max(values)})
                summary.append(item)
        write_csv(self.root / "summary.csv", summary)
        for group in ("native-ablation", "quality-end-to-end"):
            fig, axes = plt.subplots(1, 3, figsize=(15, 4.6), constrained_layout=True)
            for ax, shot in zip(axes, self.spec["shots"]):
                for row in [r for r in summary if r["shot"] == shot["name"] and r["group"] == group]:
                    ax.errorbar(row["gpu_ms_median"], row["flip_median"],
                        xerr=[[row["gpu_ms_median"] - row["gpu_ms_min"]], [row["gpu_ms_max"] - row["gpu_ms_median"]]],
                        yerr=[[row["flip_median"] - row["flip_min"]], [row["flip_max"] - row["flip_median"]]],
                        marker="o", capsize=3, label=row["config"])
                ax.axhline(self.reference(shot["name"])["reference_pair_flip"], color="gray", linestyle=":")
                ax.set(title=shot["name"], xlabel="Median GPU ms/frame", ylabel="Mean LDR-FLIP (67 PPD)")
                ax.grid(alpha=.2)
            axes[-1].legend(fontsize=7)
            fig.suptitle(f"{group} | {self.hardware['gpu']} | driver {self.hardware['driver']} | 1280x720 | 3 seeds")
            fig.savefig(self.root / f"{group}.png", dpi=160)
            plt.close(fig)
        # Consistent endpoint crops at fixed image coordinates; choose the median-FLIP seed per method.
        for shot in self.spec["shots"]:
            names = ["raw-path", "asvgf-full", "asvgf-dlss-full", "rr-quality"]
            paths, labels = [], []
            for name in names:
                candidates = sorted([r for r in rows if r["shot"] == shot["name"] and r["config"] == name],
                                    key=lambda r: r["flip"])
                paths.append(Path(candidates[1]["image"]))
                labels.append(f"{name} (seed {candidates[1]['seed']})")
            paths.append(Path(self.reference(shot["name"])["reference"]))
            labels.append("Reference 8192 spp")
            box = {"cockpit-anvil-turn": (350, 280, 950, 580),
                   "mirror-glass-pass": (60, 160, 660, 460),
                   "fast-star-turn": (320, 260, 920, 560)}[shot["name"]]
            sheet = Image.new("RGB", (600 * len(paths), 330), "white")
            draw = ImageDraw.Draw(sheet)
            for i, (path, label) in enumerate(zip(paths, labels)):
                with Image.open(path) as image:
                    sheet.paste(image.convert("RGB").crop(box), (i * 600, 30))
                draw.text((i * 600 + 6, 6), label, fill="black", font=ImageFont.load_default(size=20))
            sheet.save(self.root / f"crops-{shot['name']}.png")
        self.report["summary_rows"] = len(summary)
        self.persist()

    def lag_extension(self):
        shot = self.spec["shots"][0]
        prepared = self.bank["scenes"][shot["name"]]
        reference = self.reference(shot["name"], "384x216")
        if digest(Path(reference["reference"])) != reference["sha256"][0]:
            raise ValueError("Small reference changed")
        for horizon in (512, 1024, 2048):
            results = []
            curve_rows = []
            for config_name in ("asvgf-dlss-full", "rr-quality"):
                self.check_pause()
                name = f"lag-{config_name}-{horizon}"
                cached = self.report["lag"].get(name)
                if cached:
                    if digest(Path(cached["curve"])) != cached["curve_sha256"]:
                        raise ValueError("Saved lag curve changed")
                    results.append(cached["result"])
                    curve_rows.extend(read_csv(Path(cached["curve"])))
                    continue
                self.idle_gate()
                config = next(c for c in self.configs if c["name"] == config_name)
                flags = config["flags"] + self.motion_flags(shot) + ["--still", str(horizon)]
                total = shot["frames"] + horizon
                _, states = self.render(name, Path(prepared["directory"]) / "initial.yaml",
                    prepared["poses"]["initial"], 384, 216, total, 1, 17, flags, dump=True)
                self.validate_states(states, config, 17)
                errors, data = [], []
                for age in range(1, horizon + 1):
                    self.check_pause()
                    frame = shot["frames"] + age
                    error = ldr_flip(Path(reference["reference"]), self.out / f"{name}_{frame:04d}.png")
                    errors.append(error)
                    data.append({"config": config_name, "horizon": horizon,
                                 "still_frame": age, "frame": frame, "flip": error})
                result = {"config": config_name, "horizon": horizon, "width": 384, "height": 216,
                          "gpu": self.hardware["gpu"], "driver": self.hardware["driver"], **settling(errors)}
                curve_path = self.root / f"{name}-curve.csv"
                write_csv(curve_path, data)
                self.report["lag"][name] = {"result": result, "curve": str(curve_path), "curve_sha256": digest(curve_path)}
                self.persist()
                results.append(result)
                curve_rows.extend(data)
                print(f"LAG {config_name} {horizon}: {result['status']} age={result['settle_frames']}", flush=True)
            write_csv(self.root / f"lag-{horizon}-summary.csv", results)
            fig, ax = plt.subplots(figsize=(9, 4.8), constrained_layout=True)
            for result in results:
                data = [r for r in curve_rows if r["config"] == result["config"]]
                line, = ax.plot([int(r["still_frame"]) for r in data], [float(r["flip"]) for r in data],
                                label=f"{result['config']}: {result['status']}")
                ax.axhline(result["threshold"], color=line.get_color(), linestyle=":")
            ax.axvspan(horizon - 63, horizon, color="gray", alpha=.1)
            ax.set(xlabel="Still frames", ylabel="Mean LDR-FLIP", title=f"Shared horizon {horizon}, 384x216, seed 17")
            ax.legend(fontsize=8)
            fig.savefig(self.root / f"lag-{horizon}.png", dpi=160)
            plt.close(fig)
            self.report["lag_selected_horizon"] = horizon
            self.report["lag_selected_results"] = results
            self.persist()
            if all(r["status"] == "settled_within_capture" for r in results):
                break

    def execute(self):
        try:
            self.check_pause()
            if self.args.stage in ("all", "collect", "calibrate"):
                self.calibrate()
            if self.args.stage in ("all", "collect"):
                self.collect()
            if self.args.stage in ("all", "analyze"):
                self.analyze()
            if self.args.stage in ("all", "lag"):
                self.lag_extension()
            complete = (len(self.report["jobs"]) == self.total_runs and
                        self.report.get("summary_rows") == len(self.spec["shots"]) * len(self.configs) and
                        bool(self.report.get("lag_selected_results")))
            self.report.update(complete=complete, state="complete" if complete else "stage_complete")
            self.persist()
        except Paused as error:
            self.report.update(complete=False, state="paused", pause_reason=str(error))
            self.persist()
            print(f"PAUSED {error}", flush=True)
            return 75
        except BaseException as error:
            self.report.update(complete=False, state="failed", failure=str(error))
            self.persist()
            raise
        return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--prepare-exe", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--references", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name("shots.json"))
    parser.add_argument("--stage", choices=("all", "collect", "analyze", "lag", "calibrate"), default="all")
    parser.add_argument("--clear-pause", action="store_true")
    raise SystemExit(Formal(parser.parse_args()).execute())
