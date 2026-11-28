"""Reference/noise, GPU timestamp, and finite-capture lag preflight (not final tables)."""
import argparse
import csv
import hashlib
import importlib.metadata
import json
import math
import os
import statistics
import subprocess
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", str(Path(__file__).resolve().parents[3] / ".cache" / "matplotlib"))
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from PIL import Image

from metrics import ldr_flip, settling


def save(path, data):
    path.write_text(json.dumps(data, indent=2, allow_nan=False), encoding="utf-8")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def camera_flags(camera):
    return ["--camera", *map(str, camera["position"]), "--camera-angles",
            str(camera["yaw"]), str(camera["pitch"]), "--fov", str(camera["fov"])]


def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def write_csv(path, rows):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


class Check:
    def __init__(self, args):
        self.args = args
        self.out = args.output.resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.spec = json.loads(args.config.read_text(encoding="utf-8-sig"))
        protocol_path = args.config.parent / self.spec["protocol"]
        self.protocol = json.loads(protocol_path.read_text())
        self.common = self.protocol["common_flags"].copy()
        self.report_path = self.out / "measurement.json"
        self.report = json.loads(self.report_path.read_text()) if self.report_path.exists() else {}
        if self.report.get("references") and self.report.get("renderer_sha256") != digest(args.exe):
            raise ValueError("Renderer changed: use a separate output directory")
        config_hash = digest(args.config)
        if self.report.get("references") and self.report.get("config_sha256") != config_hash:
            raise ValueError("Shot definitions changed: use a separate output directory")
        self.input_hashes = {
            "protocol_sha256": digest(protocol_path),
            "preparer_sha256": digest(args.prepare_exe),
            "metrics_sha256": digest(Path(__file__).with_name("metrics.py")),
            "source_scene_sha256": digest(args.assets / self.spec["source_scene"]),
            "models": {str(p.relative_to(args.assets)): digest(p) for p in sorted(args.assets.rglob("*.glb"))}}
        if self.report.get("references") and self.report.get("input_hashes") != self.input_hashes:
            raise ValueError("Reference inputs changed or are unverified: use a separate output directory")
        self.report.update(renderer=str(args.exe.resolve()), renderer_sha256=digest(args.exe),
                           config_sha256=config_hash,
                           input_hashes=self.input_hashes,
                           ppd=67.0, metric="LDR-FLIP", inputs="sRGB PNG, no resizing",
                           versions={p: importlib.metadata.version(p) for p in
                                     ("flip-evaluator", "numpy", "Pillow", "matplotlib")})
        self.report["complete"] = False
        self.persist()

    def persist(self):
        save(self.report_path, self.report)

    def run(self, name, command):
        print(f"START {name}", flush=True)
        log_path = self.out / f"{name}.log"
        command = list(map(str, command))
        save(self.out / f"{name}.command.json", command)
        with log_path.open("w", encoding="utf-8") as stream:
            process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, encoding="utf-8")
            for line in process.stdout:
                stream.write(line)
                stream.flush()
                print(line.rstrip(), flush=True)
            code = process.wait()
        log = log_path.read_text(encoding="utf-8")
        if code or "/error]" in log or "/critical]" in log or "evaluating failed" in log:
            raise RuntimeError(f"{name} failed, see {log_path}")

    def common_flags(self, spp):
        flags = self.common.copy()
        flags[flags.index("-s") + 1] = str(spp)
        return flags

    def prepare(self):
        self.scenes = {}
        for shot in self.spec["shots"]:
            output = self.out / "scenes" / shot["name"]
            command = [self.args.prepare_exe, "--scene", self.args.assets / self.spec["source_scene"],
                       "--model", self.args.assets / self.spec["ship_model"], "--output", output,
                       "--position", *map(str, shot["position"]), "--yaw", str(shot["yaw"]),
                       "--pitch", str(shot["pitch"]), "--move", *map(str, shot["move"]),
                       "--turn", str(shot["turn"]), "--frames", str(shot["frames"])]
            if "camera" in shot:
                view = shot["camera"]
                command += ["--free-camera", *map(str, view["position"]), "--view-yaw", str(view["yaw"]),
                            "--view-pitch", str(view["pitch"]), "--fov", str(view["fov"])]
            self.run(f"prepare-{shot['name']}", command)
            self.scenes[shot["name"]] = {"directory": str(output),
                "poses": json.loads((output / "poses.json").read_text()),
                "initial_sha256": digest(output / "initial.yaml"),
                "end_sha256": digest(output / "end.yaml")}
        self.report["scenes"] = self.scenes
        self.persist()

    def render(self, name, scene, camera, width, height, frames, spp, seed, flags,
               timing=False, dump=False):
        png = self.out / f"{name}.png"
        stats = self.out / f"{name}.states.csv"
        command = [self.args.exe, scene, "-W", str(width), "-H", str(height), "-f", str(frames),
                   "--seed", str(seed), *self.common_flags(spp), *camera_flags(camera),
                   "-o", png, "--frame-stats", stats, *flags]
        if timing:
            command += ["--gpu-timings", self.out / f"{name}.timings.csv"]
        if dump:
            command += ["--dump-every", "1"]
        self.run(name, command)
        rows = read_csv(stats)
        if len(rows) != frames or [int(r["frame"]) for r in rows] != list(range(1, frames + 1)):
            raise RuntimeError(f"{name}: incomplete frame states")
        expected = "DLSS-RR" if "DLSS-RR" in flags else "DLSS" if "DLSS" in flags else "Off"
        scale = 1 / 1.5 if expected != "Off" else 1
        rw, rh = math.floor(width * scale + .5), math.floor(height * scale + .5)
        for row in rows:
            if (row["active_upscaler"] != expected or int(row["render_width"]) != rw or
                int(row["render_height"]) != rh or int(row["output_width"]) != width or
                int(row["output_height"]) != height or int(row["spp"]) != spp):
                raise RuntimeError(f"{name}: wrong upscaler/resolution/spp")
        with Image.open(png) as image:
            if image.size != (width, height):
                raise RuntimeError(f"{name}: PNG dimensions differ")
        return png, rows

    def timing_check(self):
        # Seven frames exercise slot reuse/drains; a 64x path workload is a known control.
        scene = self.args.assets / "scenes" / "legacy_demo.yaml"
        camera = {"position": [0, 2, 10], "yaw": -90, "pitch": -11.309933, "fov": 45}
        results = []
        for spp in (1, 64):
            name = f"timing-spp{spp}"
            _, states = self.render(name, scene, camera, 384, 216, 7, spp, 17, ["--gpu"], timing=True)
            samples = read_csv(self.out / f"{name}.timings.csv")
            if [int(r["frame"]) for r in samples] != list(range(1, 8)):
                raise RuntimeError("Missing or duplicate GPU frame indices")
            for sample in samples:
                gpu, trace = float(sample["gpu_ms"]), float(sample["trace_ms"])
                if not (math.isfinite(gpu) and math.isfinite(trace) and 0 <= trace <= gpu + .001 and gpu > 0):
                    raise RuntimeError("Invalid GPU timestamp duration")
            results.append({"spp": spp, "frames": len(samples),
                "gpu_ms": statistics.median(float(r["gpu_ms"]) for r in samples[1:]),
                "trace_ms": statistics.median(float(r["trace_ms"]) for r in samples[1:]),
                "setup_excluded": True, "dump_every": 0})
        ratio = results[1]["trace_ms"] / results[0]["trace_ms"]
        if ratio <= 3:
            raise RuntimeError("GPU timing control failed: 64 spp not substantially more expensive")
        self.report["timing_check"] = {"runs": results, "trace_workload_ratio": ratio,
            "meaning": "qualitative workload sanity check, not alternating benchmark medians"}
        self.persist()

    def reference_pair(self, shot, width, height):
        name = shot["name"]
        scene = self.scenes[name]
        key = f"{name}-{width}x{height}"
        images = []
        for seed in (1009, 2027):
            png, states = self.render(f"ref-{key}-{seed}", Path(scene["directory"]) / "end.yaml",
                scene["poses"]["end"], width, height, 2048, 4, seed, ["--gpu"], timing=True)
            if [int(r["accumulated_frames"]) for r in states] != list(range(1, 2049)):
                raise RuntimeError("Reference history reset: not 8192 accumulated spp")
            images.append(png)
        same = ldr_flip(images[0], images[0])
        if same != 0:
            raise RuntimeError("FLIP same-image control failed")
        floor = ldr_flip(images[0], images[1])
        result = {"shot": name, "width": width, "height": height, "spp": 8192,
                  "frames": 2048, "spp_per_frame": 4, "seeds": [1009, 2027],
                  "reference": str(images[0]), "independent_reference": str(images[1]),
                  "scene_sha256": scene["end_sha256"],
                  "sha256": [digest(p) for p in images], "same_image_flip": same,
                  "reference_pair_flip": floor, "ppd": 67.0}
        self.report.setdefault("references", {})[key] = result
        self.persist()
        write_csv(self.out / "reference-noise.csv", [
            {k: value[k] for k in ("shot", "width", "height", "spp", "reference_pair_flip", "same_image_flip", "ppd")}
            for value in self.report["references"].values()])
        print(f"REFERENCE NOISE {key}: {floor:.6f}", flush=True)
        return result

    def references(self):
        self.prepare()
        for shot in self.spec["shots"]:
            self.reference_pair(shot, self.args.width, self.args.height)

    def controls(self):
        camera = {"position": [0, 2, 10], "yaw": -90, "pitch": -11.309933, "fov": 45}
        scene = self.args.assets / "scenes" / "legacy_demo.yaml"
        capture_on = self.out / "timing-spp1.png"
        if not capture_on.exists():
            capture_on, _ = self.render("timing-spp1", scene, camera, 384, 216, 7, 1, 17,
                                        ["--gpu"], timing=True)
        capture_off, _ = self.render("timing-capture-off", scene, camera, 384, 216, 7, 1, 17,
                                     ["--gpu"])
        if digest(capture_on) != digest(capture_off):
            raise RuntimeError("Timing capture changed rendered pixels")
        shot = self.spec["shots"][0]
        prepared = self.report["scenes"][shot["name"]]
        total = shot["frames"] + self.args.still_frames
        flags = ["--gpu", "--no-accumulation", "--move", *map(str, shot["move"]),
                 "--move-entity", "Ship", *map(str, shot["move"]),
                 "--turn-entity", "Ship", str(shot["turn"]), "--still", str(self.args.still_frames)]
        moving, states = self.render("still-pose-moving", Path(prepared["directory"]) / "initial.yaml",
            prepared["poses"]["initial"], 384, 216, total, 1, 17, flags)
        if ([int(r["seed"]) for r in states] != list(range(17, 17 + total)) or
            any(int(r["accumulated_frames"]) != 1 for r in states)):
            raise RuntimeError("Independent still-sample accounting failed")
        end, _ = self.render("still-pose-explicit", Path(prepared["directory"]) / "end.yaml",
            prepared["poses"]["end"], 384, 216, 1, 1, 17 + total - 1, ["--gpu"])
        if digest(moving) != digest(end):
            raise RuntimeError("Appending still frames changed the motion endpoint")
        self.report["controls"] = {"timing_capture_image_identical": True,
            "still_motion_end_identical": True, "still_frames": self.args.still_frames,
            "motion_updates": shot["frames"] - 1, "total_frames": total,
            "last_raw_seed": 17 + total - 1, "still_endpoint_sha256": digest(end)}
        self.persist()

    def lag(self):
        self.scenes = self.report["scenes"]
        shot = self.spec["shots"][0]
        scene = self.scenes[shot["name"]]
        # A separate native reference at the small capture size; never resize the 720p one.
        key = f"{shot['name']}-384x216"
        reference = self.report.get("references", {}).get(key)
        if reference is None:
            reference = self.reference_pair(shot, 384, 216)
        if digest(Path(scene["directory"]) / "end.yaml") != reference["scene_sha256"]:
            raise ValueError("Endpoint scene changed since reference creation")
        if digest(Path(reference["reference"])) != reference["sha256"][0]:
            raise ValueError("Reference image changed")
        results, curves = [], []
        for config_name in ("asvgf-dlss-full", "rr-quality"):
            config = next(c for c in self.protocol["configurations"] if c["name"] == config_name)
            name = f"lag-{shot['name']}-{config_name}"
            total = shot["frames"] + self.args.still_frames
            motion_flags = ["--move", *map(str, shot["move"]), "--move-entity", "Ship", *map(str, shot["move"])]
            if shot["turn"]:
                motion_flags += ["--turn-entity", "Ship", str(shot["turn"])]
            flags = config["flags"] + motion_flags + ["--still", str(self.args.still_frames)]
            _, states = self.render(name, Path(scene["directory"]) / "initial.yaml", scene["poses"]["initial"],
                384, 216, total, 1, 17, flags, dump=True)
            if [int(r["accumulated_frames"]) for r in states] != list(range(1, total + 1)):
                raise RuntimeError("Unexpected denoiser reset during motion/still capture")
            curve = []
            for still_index in range(1, self.args.still_frames + 1):
                frame = shot["frames"] + still_index
                image = self.out / f"{name}_{frame:04d}.png"
                error = ldr_flip(Path(reference["reference"]), image)
                curve.append(error)
                curves.append({"shot": shot["name"], "config": config_name,
                               "still_frame": still_index, "frame": frame, "flip": error})
            result = {"shot": shot["name"], "config": config_name,
                      "motion_updates": shot["frames"] - 1, "still_frames": self.args.still_frames,
                      "first_still_frame": shot["frames"] + 1, "width": 384, "height": 216,
                      "reference_noise_flip": reference["reference_pair_flip"], **settling(curve)}
            results.append(result)
            self.report["lag_check"] = results
            self.persist()
            write_csv(self.out / "lag-curve.csv", curves)
            print(f"LAG {config_name}: {result}", flush=True)
        fig, ax = plt.subplots(figsize=(9, 4.8), constrained_layout=True)
        for result in results:
            data = [r for r in curves if r["config"] == result["config"]]
            line, = ax.plot([r["still_frame"] for r in data], [r["flip"] for r in data],
                            label=f"{result['config']} ({result['status']})")
            ax.axhline(result["threshold"], color=line.get_color(), linestyle=":", alpha=.7)
        ax.axvspan(self.args.still_frames - 63, self.args.still_frames, alpha=.08, color="gray")
        ax.axhline(reference["reference_pair_flip"], color="black", linestyle="--", label="Reference-pair noise")
        ax.set(xlabel="Frames since motion stopped", ylabel="Mean LDR-FLIP (67 PPD)",
               title="Small lag validation; dotted thresholds are finite-tail estimates")
        ax.legend(fontsize=8)
        fig.savefig(self.out / "lag-check.png", dpi=160)
        plt.close(fig)

    def execute(self):
        stage = self.args.stage
        if stage in ("all", "timing"):
            self.timing_check()
        if stage in ("all", "references"):
            self.references()
        if stage in ("all", "controls"):
            self.controls()
        if stage in ("all", "lag"):
            self.lag()
        self.report["complete"] = True
        self.report["completed_stage"] = stage
        self.report["preflight_complete"] = bool(
            self.report.get("timing_check") and self.report.get("controls") and
            len(self.report.get("references", {})) >= 3 and len(self.report.get("lag_check", [])) == 2)
        self.persist()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--prepare-exe", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name("shots.json"))
    parser.add_argument("--stage", choices=("all", "timing", "references", "controls", "lag"), default="all")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--still-frames", type=int, default=128)
    arguments = parser.parse_args()
    if arguments.still_frames < 64:
        parser.error("At least 64 still frames are needed for the predeclared tail window")
    Check(arguments).execute()
