"""Resumable three-shot/three-seed, two-reference 720p settling validation."""
import argparse
import importlib.metadata
import json
import math
import multiprocessing as mp
import statistics
import time
from pathlib import Path

from check_measurement import digest, read_csv, write_csv
import matplotlib.pyplot as plt
from metrics import ldr_flip, settling
from run_formal import Formal, Paused, atomic_json, atomic_replace, utc_now


METHODS = ("asvgf-dlss-full", "rr-quality")
HORIZONS = (512, 1024, 2048)


def score_frame(task):
    age, frame, png, first, second = task
    return {"still_frame": age, "frame": frame,
            "flip_primary": ldr_flip(Path(first), Path(png)),
            "flip_secondary": ldr_flip(Path(second), Path(png)),
            "png_sha256": digest(Path(png))}


class ProductionLag(Formal):
    def __init__(self, args):
        assert math.isfinite(args.frame_ms) and 0 < args.frame_ms <= 1000
        # Reuse the validated process/power monitor and state checks, with separate inputs/output.
        self.args = args
        self.root = args.output.resolve()
        self.out = self.root / "data"
        self.out.mkdir(parents=True, exist_ok=True)
        self.pause_path = self.root / "pause.request"
        self.report_path = self.root / "manifest.json"
        self.spec = json.loads(args.config.read_text(encoding="utf-8-sig"))
        protocol_path = args.config.parent / self.spec["protocol"]
        protocol = json.loads(protocol_path.read_text())
        self.common = protocol["common_flags"].copy()
        self.configs = [c for c in protocol["configurations"] if c["name"] in METHODS]
        self.bank = json.loads((args.references / "measurement.json").read_text())
        assert self.bank["preflight_complete"]
        compatibility = json.loads(args.compatibility.read_text())
        validation_path = args.compatibility.parent / "manifest.json"
        validation = json.loads(validation_path.read_text())
        assert compatibility["passed"] and validation.get("fixed_prefix_identical")
        assert compatibility["renderer_sha256"] == digest(args.exe)
        assert compatibility["reference_renderer_sha256"] == self.bank["renderer_sha256"]
        assert digest(args.config) == self.bank["config_sha256"]
        assert digest(protocol_path) == self.bank["input_hashes"]["protocol_sha256"]
        assert digest(Path(__file__).with_name("metrics.py")) == self.bank["input_hashes"]["metrics_sha256"]
        self.hardware = self.telemetry()
        self.require_ac = True
        self.monitor_hardware = True
        # Fixed-spp frame-based quality/settling capture, not a GPU-time comparison.
        self.monitor_activity = False
        policy = {"output": [1280, 720], "internal": [853, 480],
                  "methods": METHODS, "seeds": self.spec["seeds"], "horizons": HORIZONS,
                  "horizon_selection": "per shot, shared by 2 methods x 3 seeds x 2 references",
                  "reference_seeds": [1009, 2027], "ppd": 67, "tail": 64,
                  "drift_limit": .03, "tolerance": .10, "hold": 16,
                  "timing_comparison": False, "require_ac": True,
                  "sr_model": "SDK 310.9.1 explicit K", "frame_time_ms": args.frame_ms,
                  "sr_fixed_exposure": 1.0,
                  "preserve_all_frame_pngs": True}
        inputs = {"renderer": digest(args.exe), "shots": digest(args.config),
                  "protocol": digest(protocol_path), "runner": digest(Path(__file__)),
                  "monitor": digest(Path(__file__).with_name("run_formal.py")),
                  "metrics": digest(Path(__file__).with_name("metrics.py"))}
        for dll in sorted(args.exe.resolve().parent.glob("*.dll")):
            inputs[str(dll)] = digest(dll)
        inputs[str(args.compatibility.resolve())] = digest(args.compatibility)
        inputs[str(validation_path.resolve())] = digest(validation_path)
        inputs["reference_renderer"] = self.bank["renderer_sha256"]
        for config in self.configs:
            config["flags"] = config["flags"] + ["--frame-ms", str(args.frame_ms)]
            if config["upscaler"] == "DLSS":
                config["flags"] += ["--dlss-preset", "K", "--dlss-exposure", "1"]
        for shot in self.spec["shots"]:
            scene = self.bank["scenes"][shot["name"]]
            for role in ("initial", "end"):
                path = Path(scene["directory"]) / f"{role}.yaml"
                assert digest(path) == scene[f"{role}_sha256"]
                inputs[str(path)] = digest(path)
            ref = self.reference(shot["name"])
            for key, sha in zip(("reference", "independent_reference"), ref["sha256"]):
                assert digest(Path(ref[key])) == sha
                inputs[ref[key]] = sha
        versions = {name: importlib.metadata.version(name) for name in
                    ("flip-evaluator", "numpy", "Pillow", "matplotlib")}
        signature = json.loads(json.dumps({"inputs": inputs, "policy": policy, "versions": versions,
                                           "gpu": self.hardware["gpu"],
                                           "driver": self.hardware["driver"]}))
        if self.report_path.exists():
            self.report = json.loads(self.report_path.read_text())
            assert self.report["signature"] == signature, "Inputs changed; use fresh output"
        else:
            self.report = {"created_utc": utc_now(), "signature": signature,
                           "renders": {}, "controls": {}, "curves": {}, "selected": {}}
        self.report.update(complete=False, state="preparing", workers=args.workers)
        environment = self.root / "environment.json"
        if environment.exists():
            assert json.loads(environment.read_text()) == versions
        else:
            atomic_json(environment, versions)
        self.report.pop("pause_reason", None)
        self.persist()

    def validate_states(self, states, config, seed):
        super().validate_states(states, config, seed)
        assert all(abs(float(row["frame_time_ms"]) - self.args.frame_ms) <= 1e-5
                   for row in states), "Temporal step was not fixed"
        exposure = 1.0 if config['upscaler'] == 'DLSS' else 0.0
        assert all(float(row['dlss_exposure']) == exposure for row in states)

    def motion_control(self, shot, config, seed):
        name = f"{shot['name']}--{config['name']}--{seed}--motion-control"
        cached = self.report["controls"].get(name)
        if cached:
            for path, sha in cached["files"].items():
                assert digest(Path(path)) == sha
            return Path(cached["png"])
        stale = list(self.out.glob(name + ".*"))
        if stale:
            archive = self.root / "interrupted" / f"{name}-{int(time.time())}"
            archive.mkdir(parents=True)
            for path in stale:
                path.rename(archive / path.name)
        prepared = self.bank["scenes"][shot["name"]]
        png, states = self.render(name, Path(prepared["directory"]) / "initial.yaml",
            prepared["poses"]["initial"], 1280, 720, shot["frames"], 1, seed,
            config["flags"] + self.motion_flags(shot), timing=True)
        self.validate_states(states, config, seed)
        old = self.args.formal / "data" / f"{shot['name']}--{config['name']}--{seed}.png"
        same = digest(png) == digest(old)
        files = list(self.out.glob(name + ".*"))
        self.report["controls"][name] = {"png": str(png), "historical_identical": same,
            "historical_flip": 0.0 if same else ldr_flip(old, png),
            "files": {str(p): digest(p) for p in files}}
        self.persist()
        return png

    def capture(self, shot, config, seed, horizon):
        name = f"{shot['name']}--{config['name']}--{seed}--still-{horizon}"
        cached = self.report["renders"].get(name)
        if cached:
            for path, sha in cached["files"].items():
                assert digest(Path(path)) == sha, path
            return name
        # Rejected/interrupted output is retained separately before a retry.
        stale = list(self.out.glob(name + "*"))
        if stale:
            archive = self.root / "interrupted" / f"{name}-{int(time.time())}"
            archive.mkdir(parents=True)
            for path in stale:
                path.rename(archive / path.name)
        self.check_pause()
        control = self.motion_control(shot, config, seed)
        prepared = self.bank["scenes"][shot["name"]]
        total = shot["frames"] + horizon
        flags = config["flags"] + self.motion_flags(shot) + ["--still", str(horizon)]
        _, states = self.render(name, Path(prepared["directory"]) / "initial.yaml",
                                prepared["poses"]["initial"], 1280, 720, total, 1,
                                seed, flags, dump=True)
        self.validate_states(states, config, seed)
        # The first moving PNG is not dumped; every subsequent frame is retained.
        images = {str(path): digest(path) for path in sorted(self.out.glob(name + "_*.png"))}
        assert len(images) == total - 1
        endpoint = self.out / f"{name}_{shot['frames']:04d}.png"
        assert digest(endpoint) == digest(control), "Fixed-step motion endpoint changed with dumping"
        # Extensions must reproduce the shorter capture's complete pixel prefix.
        for previous_horizon in HORIZONS:
            previous = self.report["renders"].get(name.replace(f"still-{horizon}",
                                                               f"still-{previous_horizon}"))
            if previous_horizon >= horizon or not previous:
                continue
            old_prefix = f"{shot['name']}--{config['name']}--{seed}--still-{previous_horizon}"
            for path, sha in previous["images"].items():
                other = self.out / Path(path).name.replace(old_prefix, name)
                assert digest(other) == sha, "Longer capture changed an existing frame"
        artifacts = list(self.out.glob(name + ".*"))
        self.report["renders"][name] = {"shot": shot["name"], "config": config["name"],
            "seed": seed, "horizon": horizon, "frames": total, "motion_endpoint_identical": True,
            "images": images, "files": {str(p): digest(p) for p in artifacts}}
        self.persist()
        print(f"CAPTURED {name} ({total} frames)", flush=True)
        return name

    def score(self, name, shot, horizon):
        path = self.root / f"{name}-curve.csv"
        cached = self.report["curves"].get(name)
        if cached:
            assert digest(path) == cached["sha256"]
            return cached["results"]
        existing = read_csv(path) if path.exists() else []
        assert [int(r["still_frame"]) for r in existing] == list(range(1, len(existing) + 1))
        for row in existing:
            png = self.out / f"{name}_{int(row['frame']):04d}.png"
            assert digest(png) == row["png_sha256"]
        ref = self.reference(shot["name"])
        tasks = [(age, shot["frames"] + age,
                  str(self.out / f"{name}_{shot['frames'] + age:04d}.png"),
                  ref["reference"], ref["independent_reference"])
                 for age in range(len(existing) + 1, horizon + 1)]
        self.report.update(state="scoring", active_job=name,
                           scoring_completed=len(existing), scoring_total=horizon)
        self.persist()
        pool = mp.Pool(self.args.workers)
        try:
            iterator = pool.imap(score_frame, tasks, chunksize=1)
            for _ in tasks:
                while True:
                    self.check_pause()
                    try:
                        row = iterator.next(timeout=.2)
                        break
                    except mp.TimeoutError:
                        continue
                existing.append(row)
                if len(existing) % 16 == 0 or len(existing) == horizon:
                    temporary = path.with_suffix(".csv.tmp")
                    write_csv(temporary, existing)
                    atomic_replace(temporary, path)
                    self.report["scoring_completed"] = len(existing)
                    self.persist()
                if len(existing) % 64 == 0:
                    print(f"SCORED {name}: {len(existing)}/{horizon} (two references)", flush=True)
            pool.close()
            pool.join()
        except BaseException:
            pool.terminate()
            pool.join()
            raise
        results = {key: settling([float(row[f"flip_{key}"]) for row in existing])
                   for key in ("primary", "secondary")}
        self.report["curves"][name] = {"path": str(path), "sha256": digest(path),
                                       "results": results}
        self.persist()
        print(f"CONVERGENCE {name}: {json.dumps(results)}", flush=True)
        return results

    def summarize(self):
        rows = []
        summary = []
        fig, axes = plt.subplots(3, 2, figsize=(12, 10), constrained_layout=True)
        for shot_index, shot in enumerate(self.spec["shots"]):
            if shot["name"] not in self.report["selected"]:
                continue
            horizon = self.report["selected"][shot["name"]]
            for column, reference in enumerate(("primary", "secondary")):
                ax = axes[shot_index, column]
                for config in self.configs:
                    subset = []
                    for seed in self.spec["seeds"]:
                        name = f"{shot['name']}--{config['name']}--{seed}--still-{horizon}"
                        result = self.report["curves"][name]["results"][reference]
                        row = {"shot": shot["name"], "config": config["name"], "seed": seed,
                               "reference": reference, "horizon": horizon, **result}
                        rows.append(row)
                        subset.append(row)
                        curve = read_csv(self.root / f"{name}-curve.csv")
                        ax.plot([int(r["still_frame"]) for r in curve],
                                [float(r[f"flip_{reference}"]) for r in curve],
                                label=f"{config['name']} seed {seed}", alpha=.8, linewidth=.8)
                    settled = all(r["status"] == "settled_within_capture" for r in subset)
                    ages = [r["settle_frames"] for r in subset] if settled else []
                    summary.append({"shot": shot["name"], "config": config["name"],
                        "reference": reference, "horizon": horizon, "seeds": len(subset),
                        "settled_seeds": sum(r["status"] == "settled_within_capture" for r in subset),
                        "settle_median": statistics.median(ages) if ages else None,
                        "settle_min": min(ages) if ages else None, "settle_max": max(ages) if ages else None,
                        "terminal_flip_median": statistics.median(r["terminal_flip"] for r in subset),
                        "tail_drift_max": max(r["tail_relative_drift"] for r in subset)})
                ax.axvspan(horizon - 63, horizon, color="gray", alpha=.12)
                ax.set(title=f"{shot['name']} / {reference} / {horizon} still frames",
                       xlabel="Still frames (not elapsed time)", ylabel="Mean LDR-FLIP (67 PPD)")
                ax.legend(fontsize=6)
        if rows:
            write_csv(self.root / "runs.csv", rows)
            write_csv(self.root / "summary.csv", summary)
            fig.savefig(self.root / "convergence.png", dpi=160)
        plt.close(fig)

    def execute(self):
        try:
            for shot in self.spec["shots"]:
                if shot["name"] in self.report["selected"]:
                    continue
                for horizon in HORIZONS:
                    outcomes = []
                    for seed_index, seed in enumerate(self.spec["seeds"]):
                        for offset in range(len(self.configs)):
                            config = self.configs[(offset + seed_index) % len(self.configs)]
                            self.check_pause()
                            name = self.capture(shot, config, seed, horizon)
                            outcomes.extend(self.score(name, shot, horizon).values())
                    if all(r["status"] == "settled_within_capture" for r in outcomes) or horizon == HORIZONS[-1]:
                        self.report["selected"][shot["name"]] = horizon
                        self.persist()
                        self.summarize()
                        break
            self.report.update(complete=True, state="complete", active_job=None)
            self.persist()
            return 0
        except Paused as error:
            self.report.update(complete=False, state="paused", pause_reason=str(error))
            self.persist()
            print(f"PAUSED {error}", flush=True)
            return 75
        except BaseException as error:
            self.report.update(complete=False, state="failed", failure=str(error))
            self.persist()
            raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--references", type=Path, default=Path("renders/measurement"))
    parser.add_argument("--formal", type=Path, default=Path("renders/formal-ac"))
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name("shots.json"))
    parser.add_argument("--output", type=Path, default=Path("renders/production-lag-fixed-exposure"))
    parser.add_argument("--compatibility", type=Path,
                        default=Path("renders/fixed-exposure-validation/reference-compatibility.json"))
    parser.add_argument("--frame-ms", type=float, default=1000 / 60)
    parser.add_argument("--workers", type=int, choices=(1, 2, 4), default=4)
    raise SystemExit(ProductionLag(parser.parse_args()).execute())
