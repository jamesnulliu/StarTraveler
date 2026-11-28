"""Evaluate scene lighting at fixed endpoints with interleaved GPU runs."""
import argparse
import csv
import json
import math
import re
import subprocess
from pathlib import Path
from statistics import mean, median

import flip_evaluator
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from PIL import Image, ImageStat


HERE = Path(__file__).resolve().parent
COLUMNS = ["shot", "config", "seed", "gpu_ms", "trace_ms", "flip", "mode",
           "gpu", "driver", "width", "height"]
REFERENCE_FRAMES = 2048
REFERENCE_SPP = 4


def run(command, log):
    command = list(map(str, command))
    with log.open("w", encoding="utf-8") as stream:
        print(json.dumps(command), file=stream, flush=True)
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"Command exited {result.returncode}, see {log}")
    return log.read_text(encoding="utf-8", errors="replace")


def ldr_flip(reference, test):
    _, score, _ = flip_evaluator.evaluate(str(reference), str(test), "LDR", inputsRGB=True,
                                          applyMagma=False, parameters={"ppd": 67.0})
    if not 0 <= score <= 1:
        raise ValueError(f"Invalid LDR-FLIP {score} for {test}")
    return float(score)


def timings(path, frames, *, warmup=1, still=0):
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != frames or not 1 <= warmup < frames - still:
        raise ValueError(f"Incomplete timestamps in {path}")
    gpu, trace = [], []
    for i, row in enumerate(rows, 1):
        total, rays = float(row["gpu_ms"]), float(row["trace_ms"])
        if (int(row["frame"]) != i or int(row["setup_frame"]) != int(i == 1)
                or not math.isfinite(total) or not 0 <= rays <= total or total <= 0):
            raise ValueError(f"Invalid timestamps in {path}, frame {i}")
        if warmup < i <= frames - still:
            gpu.append(total)
            trace.append(rays)
    return median(gpu), median(trace)


def render(exe, scene, png, flags, size, *, frames, seed, spp=1, measure=False, reference=False):
    command = [exe, scene, "-o", png, "-W", size[0], "-H", size[1], "-f", frames,
               "-s", spp, "--seed", seed, *flags]
    if measure:
        command += ["--gpu-timings", png.with_suffix(".timings.csv")]
    log = run(command, png.with_suffix(".log"))
    device = re.search(r"Vulkan device: (.+?) \| (.+?) \| ([^\r\n]+)", log)
    if not device:
        raise ValueError(f"No Vulkan device and driver recorded for {png}")
    if reference and f"Endpoint accumulation: {frames} frames" not in log:
        raise ValueError(f"Reference did not accumulate all {frames} stationary frames: {png}")
    if "--upscaler" in flags:
        requested = flags[flags.index("--upscaler") + 1]
        if requested != "Off" and f"Upscaled with {requested}:" not in log:
            raise ValueError(f"{png}: renderer did not use {requested}")
    with Image.open(png) as image:
        if image.format != "PNG" or image.size != tuple(size) or image.mode not in ("RGBA", "RGB"):
            raise ValueError(f"Unexpected render format or dimensions in {png}")
        image.verify()
    return device[1], f"{device[2]} {device[3]}"


def check_direct(exe, root):
    root.mkdir()
    scene = HERE / "direct-control.yaml"
    device = None
    for name, pipeline in [("path", ["--gpu"]), ("traced", ["--hybrid"]),
                           ("maps", ["--hybrid", "--shadow-maps"]),
                           ("traced-half", ["--hybrid", "--half"]),
                           ("maps-half", ["--hybrid", "--half", "--shadow-maps"])]:
        for view, target_z, depth in [("mirror", 0, 2), ("receiver", 8, 2), ("bounce", 0, 5)]:
            flags = [*pipeline, "-b", depth, "--camera", 0, 0, 3, "--look-at", 0, 0, target_z,
                     "--fov", 4]
            if depth == 2:
                flags += ["--direct-only"]
            png = root / f"{name}-{view}.png"
            current = render(exe, scene, png, flags, [64, 64], frames=64, seed=17, spp=4)
            if device and device != current:
                raise ValueError("GPU changed during direct-light controls")
            device = current
            with Image.open(png) as image:
                center = image.convert("RGB").crop((28, 28, 36, 36))
                level = mean(ImageStat.Stat(center).mean)
            if (view == "mirror" and level > 2) or (view != "mirror" and level < 20):
                raise ValueError(f"Direct-light control failed: {png}, center mean {level:.2f}/255")
    return device


def prepare(args, spec, shot, directory):
    move, turn = shot["move"], shot["turn"]
    frames, still = shot["frames"], shot.get("still", 0)
    command = [args.prepare_exe, "--scene", args.assets / shot.get("scene", spec["source_scene"]),
               "--model", args.assets / spec["ship_model"], "--output", directory,
               "--position", *shot["position"], "--yaw", shot["yaw"], "--pitch", shot["pitch"],
               "--move", *move, "--turn", turn, "--frames", frames - still]
    if "camera" in shot:
        view = shot["camera"]
        command += ["--free-camera", *view["position"], "--fov", view["fov"]]
        if "look_at" in view:
            command += ["--look-at", *view["look_at"]]
        else:
            command += ["--view-yaw", view["yaw"], "--view-pitch", view["pitch"]]
    run(command, directory / "prepare.log")
    poses = json.loads((directory / "poses.json").read_text())
    if poses["motion_updates"] != frames - still - 1:
        raise ValueError(f"Wrong endpoint for {shot['name']}")
    motion = []
    if any(move):
        motion += ["--move", *move, "--move-entity", "Ship", *move]
    if turn:
        motion += ["--turn-entity", "Ship", turn]
    return poses, motion


def camera(pose):
    return ["--camera", *pose["position"], "--camera-angles", pose["yaw"], pose["pitch"],
            "--fov", pose["fov"]]


def check_endpoint(args, shot, directory, poses, motion, size, flags, seed, device):
    """A moving raw render resets at its final update and must match the saved pose."""
    width, height = size
    check_size = (min(384, width), max(1, round(height * min(384, width) / width)))
    moving = directory / "endpoint-motion.png"
    saved = directory / "endpoint-saved.png"
    frames = shot["frames"] - shot.get("still", 0) if motion else 1
    for scene, pose, png, count, update in (
        ("initial.yaml", "initial", moving, frames, motion),
        ("end.yaml", "end", saved, 1, []),
    ):
        current = render(args.exe, directory / scene, png, ["--gpu", *camera(poses[pose]),
                         *flags, *update], check_size, frames=count, seed=seed)
        if current != device:
            raise ValueError("GPU or driver changed during the endpoint check")
    if moving.read_bytes() != saved.read_bytes():
        raise ValueError(f"Moving and saved renders differ at the endpoint of {shot['name']}")


def evaluate(args, spec):
    device = check_direct(args.exe, args.output / "controls")
    width, height = spec["resolution"]
    rows = []
    for shot in spec["shots"]:
        directory = args.output / shot["name"]
        directory.mkdir()
        poses, motion = prepare(args, spec, shot, directory)
        size = (width, height)
        still = shot.get("still", 0)
        check_endpoint(args, shot, directory, poses, motion, size, spec["common_flags"],
                       spec["seeds"][0], device)
        for mode, options in spec["modes"].items():
            out = directory / mode
            out.mkdir()
            flags = [*spec["common_flags"], *options]
            base = dict(shot=shot["name"], mode=mode, width=width, height=height,
                        gpu=device[0], driver=device[1])
            ref = out / "reference.png"
            for index, seed in enumerate(spec["reference_seeds"]):
                png = out / "reference-noise.png" if index else ref
                current = render(args.exe, directory / "end.yaml", png,
                                 ["--gpu", *camera(poses["end"]), *flags], size,
                                 frames=REFERENCE_FRAMES, spp=REFERENCE_SPP, seed=seed,
                                 measure=True, reference=True)
                if current != device:
                    raise ValueError("GPU or driver changed during references")
                gpu, trace = timings(png.with_suffix(".timings.csv"), REFERENCE_FRAMES,
                                     warmup=spec["warmup_frames"])
                score = ldr_flip(ref, png)
                if not index and score != 0:
                    raise ValueError(f"Reference self-comparison failed in {out}")
                rows.append(dict(base, config=png.stem, seed=seed,
                                 gpu_ms=gpu, trace_ms=trace, flip=score))

            configs = spec["configurations"]
            for i, seed in enumerate(spec["seeds"]):
                start = i % len(configs)
                for config in configs[start:] + configs[:start]:
                    name = config["name"]
                    png = out / f"{name}-{seed}.png"
                    print(f"{shot['name']} / {mode} / {name} / seed {seed}", flush=True)
                    current = render(args.exe, directory / "initial.yaml", png,
                                     [*camera(poses["initial"]), *motion, "--still", still,
                                      *flags, *config["flags"]], size, frames=shot["frames"],
                                     seed=seed, measure=True)
                    if current != device:
                        raise ValueError("GPU or driver changed during measurements")
                    gpu, trace = timings(png.with_suffix(".timings.csv"), shot["frames"],
                                         warmup=spec["warmup_frames"], still=still)
                    rows.append(dict(base, config=name, seed=seed, gpu_ms=gpu,
                                     trace_ms=trace, flip=ldr_flip(ref, png)))
    return rows


def write_report(root, spec, rows, timing_note=None):
    first = rows[0]
    hardware = f"{first['gpu']}, {first['driver']}, {first['width']}×{first['height']}"
    intro = ["# Direct-light comparison", "", hardware, "",
             f"GPU and trace times are medians over frames after the first {spec['warmup_frames']} "
             "warmup frames and before any still suffix, then over seeds. "
             "FLIP is the median endpoint LDR-FLIP (67 PPD). Each noise estimate compares "
             "two independent 8192 spp endpoint images.", ""]
    if timing_note:
        intro.extend([f"**Timing conditions.** {timing_note}", ""])
    report = []
    comparisons = {}
    for shot in spec["shots"]:
        for mode in spec["modes"]:
            relative = Path(shot["name"]) / mode
            out = root / relative
            group = [r for r in rows if r["shot"] == shot["name"] and r["mode"] == mode]
            noise = next(r["flip"] for r in group if r["config"] == "reference-noise")
            report.extend([f"## {shot['name']} · {mode}", "", hardware, "",
                           "| Configuration | GPU ms | Trace ms | FLIP |",
                           "|---|---:|---:|---:|"])
            fig, ax = plt.subplots(figsize=(7.5, 4), layout="constrained")
            pairs = {}
            crops = [("Reference", out / "reference.png")]
            for config in spec["configurations"]:
                name = config["name"]
                runs = [r for r in group if r["config"] == name]
                gpu, trace, score = [median(r[key] for r in runs)
                                     for key in ("gpu_ms", "trace_ms", "flip")]
                report.append(f"| {name} | {gpu:.3f} | {trace:.3f} | {score:.5f} |")
                ax.scatter(gpu, score, label=name)
                if "pair" in config:
                    pairs.setdefault(config["pair"], {})[config["shadows"]] = (gpu, score)
                crops.append((name, out / f"{name}-{spec['seeds'][0]}.png"))
            ax.axhline(noise, ls="--", color="0.45", label="Reference-pair FLIP")
            ax.set(xlabel="GPU time per frame (ms)", ylabel="Mean LDR-FLIP",
                   title=f"{shot['name']} · {mode}")
            ax.legend(loc="center left", bbox_to_anchor=(1, .5))
            fig.savefig(out / "error-time.png", dpi=150)
            plt.close(fig)

            box = [round(v * n) for v, n in zip(shot["crop"], spec["resolution"] * 2)]
            columns = min(3, len(crops))
            fig, axes = plt.subplots(math.ceil(len(crops) / columns), columns, squeeze=False,
                                     figsize=(columns * 3, math.ceil(len(crops) / columns) * 2.4),
                                     layout="constrained")
            for axis in axes.flat:
                axis.axis("off")
            for axis, (label, png) in zip(axes.flat, crops):
                with Image.open(png) as image:
                    axis.imshow(image.crop(box), interpolation="nearest")
                axis.set_title(label)
            fig.savefig(out / "crops.png", dpi=150)
            plt.close(fig)
            report += ["", f"Reference-pair FLIP was {noise:.5f}."]
            for label, pair in pairs.items():
                traced_ms, traced_flip = pair["traced"]
                maps_ms, maps_flip = pair["maps"]
                delta = traced_flip - maps_flip
                comparisons.setdefault((mode, label), []).append(
                    (traced_ms - maps_ms, delta, noise))
                note = (" The difference is within the reference noise floor."
                        if abs(delta) <= noise else "")
                report.append(f"At {label} resolution, traced shadows cost "
                              f"{abs(traced_ms - maps_ms):.3f} ms "
                              f"{'more' if traced_ms >= maps_ms else 'less'} per frame than shadow "
                              f"maps and {'raised' if delta >= 0 else 'lowered'} FLIP by "
                              f"{abs(delta):.5f}.{note}")
            report += ["", f"![Error against GPU time]({relative.as_posix()}/error-time.png)",
                       "", f"![Matched crops, seed {spec['seeds'][0]}]({relative.as_posix()}/crops.png)", ""]
    summary = []
    if comparisons:
        summary = ["## Traced shadows versus shadow maps", ""]
        for (mode, label), values in comparisons.items():
            time_change = median(ms for ms, _, _ in values)
            flip_change = median(flip for _, flip, _ in values)
            decreases = sum(flip < -noise for _, flip, noise in values)
            increases = sum(flip > noise for _, flip, noise in values)
            summary += [f"For {mode} images at {label} trace resolution, the median paired "
                        f"change with traced shadows was {time_change:+.3f} ms and "
                        f"{flip_change:+.5f} FLIP across {len(values)} shots. Beyond the "
                        f"reference noise estimate, FLIP decreased in {decreases} and "
                        f"increased in {increases} of those shots.", ""]
    (root / "report.md").write_text("\n".join(intro + summary + report), encoding="utf-8")


def validate(spec):
    if spec.get("version") != 1:
        raise ValueError("Expected evaluation configuration version 1")
    width, height = spec["resolution"]
    if any(type(n) is not int or n < 1 for n in (width, height)):
        raise ValueError("Resolution must contain two positive integers")
    warmup = spec["warmup_frames"]
    if type(warmup) is not int or not 1 <= warmup < REFERENCE_FRAMES:
        raise ValueError("Invalid warmup_frames")
    if len(spec["seeds"]) != 3 or len(spec["reference_seeds"]) != 2:
        raise ValueError("Supply three run seeds and two reference seeds")
    seeds = [*spec["seeds"], *spec["reference_seeds"]]
    if len(set(seeds)) != 5 or any(type(s) is not int or not 0 <= s < 2**32 for s in seeds):
        raise ValueError("Run and reference seeds must be distinct unsigned integers")
    for names in ([s["name"] for s in spec["shots"]], list(spec["modes"]),
                  [c["name"] for c in spec["configurations"]]):
        if (not names or len(set(names)) != len(names)
                or any(not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", n) for n in names)):
            raise ValueError("Use distinct lowercase filenames for shot, mode and configuration names")
    if any(c["name"] in ("reference", "reference-noise") for c in spec["configurations"]):
        raise ValueError("Reference names are reserved")
    for shot in spec["shots"]:
        frames, still = shot["frames"], shot.get("still", 0)
        if (type(frames) is not int or type(still) is not int
                or not 0 <= still < frames - warmup):
            raise ValueError(f"Invalid motion, warmup or still frame count for {shot['name']}")
        x0, y0, x1, y1 = shot["crop"]
        if not (0 <= x0 < x1 <= 1 and 0 <= y0 < y1 <= 1):
            raise ValueError(f"Invalid normalized crop for {shot['name']}")
        if round(x0 * width) == round(x1 * width) or round(y0 * height) == round(y1 * height):
            raise ValueError(f"Crop has no pixels at the requested resolution for {shot['name']}")
    pairs = {}
    for config in spec["configurations"]:
        if "pair" in config:
            pairs.setdefault(config["pair"], []).append(config["shadows"])
    if any(sorted(methods) != ["maps", "traced"] for methods in pairs.values()):
        raise ValueError("Each shadow comparison requires one traced and one maps configuration")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path, help="starcore-render executable")
    parser.add_argument("--prepare-exe", type=Path, help="startraveler-prepare-shot (beside renderer by default)")
    parser.add_argument("--assets", required=True, type=Path, help="root of scene and model files")
    parser.add_argument("--config", type=Path, default=HERE / "direct-light.json")
    parser.add_argument("--output", required=True, type=Path, help="new or empty results directory")
    parser.add_argument("--timing-note", help="GPU load or collection conditions for the report")
    args = parser.parse_args()
    args.exe = args.exe.resolve(strict=True)
    args.prepare_exe = (args.prepare_exe or args.exe.with_name(
        "startraveler-prepare-shot" + args.exe.suffix)).resolve(strict=True)
    args.assets = args.assets.resolve(strict=True)
    spec = json.loads(args.config.read_text(encoding="utf-8"))
    validate(spec)
    args.output.mkdir(parents=True, exist_ok=True)
    if any(args.output.iterdir()):
        parser.error("--output must be empty")
    (args.output / "config.json").write_text(json.dumps(spec, indent=2), encoding="utf-8")
    rows = evaluate(args, spec)
    with (args.output / "results.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
    write_report(args.output, spec, rows, args.timing_note)
    print(args.output / "report.md")


if __name__ == "__main__":
    main()
