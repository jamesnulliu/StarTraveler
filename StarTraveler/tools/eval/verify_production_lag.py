"""Audit every saved settling capture/curve and spot-check FLIP after collection."""
import argparse
import importlib.metadata
import json
import math
import statistics
from pathlib import Path

from check_measurement import digest, read_csv
from metrics import ldr_flip, settling
from run_gpu_regression import save


def verify(root, bank_path, formal):
    root = root.resolve()
    manifest = json.loads((root / "manifest.json").read_text())
    bank = json.loads(bank_path.read_text())
    assert manifest["complete"] and len(manifest["selected"]) == 3
    signature = manifest["signature"]
    inputs = signature["inputs"]
    assert inputs["reference_renderer"] == bank["renderer_sha256"]
    assert inputs["shots"] == bank["config_sha256"]
    for key, filename in (("runner", "run_production_lag.py"),
                          ("monitor", "run_formal.py"), ("metrics", "metrics.py")):
        assert digest(Path(__file__).with_name(filename)) == inputs[key]
    environment = json.loads((root / "environment.json").read_text())
    assert all(importlib.metadata.version(k) == v for k, v in environment.items())
    for path, sha in inputs.items():
        if Path(path).is_file():
            assert digest(Path(path)) == sha, path
    counts = {"physical_captures": 0, "artifact_hashes": 0, "png_hashes": 0,
              "frame_states": 0, "still_pngs": 0, "flip_scores": 0,
              "selected_outcomes": 0, "direct_score_checks": 0}
    max_difference = 0.0
    for name, render in manifest["renders"].items():
        assert len(render["images"]) == render["frames"] - 1
        for path, sha in render["files"].items():
            assert digest(Path(path)) == sha, path
            counts["artifact_hashes"] += 1
        for path, sha in render["images"].items():
            assert digest(Path(path)) == sha, path
            counts["png_hashes"] += 1
        states = read_csv(root / "data" / f"{name}.states.csv")
        assert len(states) == render["frames"]
        for index, state in enumerate(states, 1):
            assert int(state["frame"]) == index
            assert int(state["seed"]) == render["seed"] and int(state["spp"]) == 1
            assert int(state["accumulated_frames"]) == index
            assert int(state["upscaler_history_reset"]) == int(index == 1)
            assert (int(state["render_width"]), int(state["render_height"])) == (853, 480)
            assert (int(state["output_width"]), int(state["output_height"])) == (1280, 720)
            assert state["active_upscaler"] == ("DLSS-RR" if render["config"] == "rr-quality" else "DLSS")
            assert abs(float(state["frame_time_ms"]) - signature["policy"]["frame_time_ms"]) <= 1e-5
            assert float(state['dlss_exposure']) == (
                signature['policy']['sr_fixed_exposure'] if render['config'] != 'rr-quality' else 0.0)
        hardware = read_csv(root / "data" / f"{name}.hardware.csv")
        assert len(hardware) >= 2 and all(int(r["ac_online"]) == 1 for r in hardware)
        assert all(r["gpu"] == signature["gpu"] and r["driver"] == signature["driver"] for r in hardware)
        curves = manifest["curves"][name]
        assert digest(Path(curves["path"])) == curves["sha256"]
        rows = read_csv(Path(curves["path"]))
        assert len(rows) == render["horizon"]
        assert [int(r["still_frame"]) for r in rows] == list(range(1, len(rows) + 1))
        moving = render["frames"] - render["horizon"]
        endpoint = root / "data" / f"{name}_{moving:04d}.png"
        control_name = f"{render['shot']}--{render['config']}--{render['seed']}--motion-control"
        control = manifest["controls"][control_name]
        assert digest(endpoint) == digest(Path(control["png"]))
        for path, sha in control["files"].items():
            assert digest(Path(path)) == sha
        assert [int(r["frame"]) for r in rows] == list(range(moving + 1, render["frames"] + 1))
        for row in rows:
            image = root / "data" / f"{name}_{int(row['frame']):04d}.png"
            assert row["png_sha256"] == render["images"][str(image)]
        ref = bank["references"][render["shot"] + "-1280x720"]
        for label, reference in (("primary", ref["reference"]), ("secondary", ref["independent_reference"])):
            computed = settling([float(r[f"flip_{label}"]) for r in rows])
            assert computed == curves["results"][label]
            age = computed["settle_frames"] or len(rows) // 2
            row = rows[age - 1]
            png = root / "data" / f"{name}_{int(row['frame']):04d}.png"
            difference = abs(ldr_flip(Path(reference), png) - float(row[f"flip_{label}"]))
            assert difference <= 1e-7, "Direct FLIP spot check differs"
            max_difference = max(max_difference, difference)
            counts["direct_score_checks"] += 1
        counts["physical_captures"] += 1
        counts["frame_states"] += len(states)
        counts["still_pngs"] += len(rows)
        counts["flip_scores"] += 2 * len(rows)
    runs = read_csv(root / "runs.csv")
    summary = read_csv(root / "summary.csv")
    assert len(runs) == 36 and len(summary) == 12
    for row in runs:
        assert int(row["horizon"]) == manifest["selected"][row["shot"]]
        name = f"{row['shot']}--{row['config']}--{row['seed']}--still-{row['horizon']}"
        result = manifest["curves"][name]["results"][row["reference"]]
        assert row["status"] == result["status"]
        assert row["settle_frames"] == (str(result["settle_frames"]) if result["settle_frames"] is not None else "")
        counts["selected_outcomes"] += 1
    for row in summary:
        group = [r for r in runs if all(r[k] == row[k] for k in ("shot", "config", "reference"))]
        assert len(group) == 3
        successful = [r for r in group if r["status"] == "settled_within_capture"]
        assert int(row["settled_seeds"]) == len(successful)
        if len(successful) == 3:
            ages = [int(r["settle_frames"]) for r in group]
            assert float(row["settle_median"]) == statistics.median(ages)
            assert float(row["settle_min"]) == min(ages) and float(row["settle_max"]) == max(ages)
        else:
            assert row["settle_median"] == row["settle_min"] == row["settle_max"] == ""
        assert math.isclose(float(row["terminal_flip_median"]),
                            statistics.median(float(r["terminal_flip"]) for r in group), abs_tol=1e-12)
    result = {"passed": True, **counts, "selected_horizons": manifest["selected"],
              "direct_score_max_difference": max_difference,
              "manifest_sha256": digest(root / "manifest.json"),
              "verifier_sha256": digest(Path(__file__)),
              "summary_sha256": digest(root / "summary.csv"),
              "runs_sha256": digest(root / "runs.csv"),
              "plot_sha256": digest(root / "convergence.png")}
    save(root / "verification.json", result)
    print(json.dumps(result), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("renders/production-lag-fixed-exposure"))
    parser.add_argument("--bank", type=Path, default=Path("renders/measurement/measurement.json"))
    parser.add_argument("--formal", type=Path, default=Path("renders/formal-ac"))
    args = parser.parse_args()
    verify(args.root, args.bank, args.formal)
