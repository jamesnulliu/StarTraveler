"""Audit a completed local dataset and assess its second-reference sensitivity."""
import argparse
import json
import math
import re
import statistics
from datetime import datetime, timedelta
from pathlib import Path

from check_measurement import digest, read_csv, write_csv
from metrics import ldr_flip, settling


def verify(root):
    manifest = json.loads((root / "manifest.json").read_text())
    assert manifest["complete"] and manifest["state"] == "complete"
    bank = json.loads((Path(manifest["reference_bank"]) / "measurement.json").read_text())
    jobs = manifest["jobs"]
    assert len(jobs) == 81
    checks = {"motion_runs": len(jobs), "artifact_hashes": 0, "timestamp_rows": 0,
              "hardware_samples": 0, "activity_heartbeats": 0, "max_heartbeat_gap_seconds": 0,
              "calibration_runs": len(manifest["calibration"])}
    sensitivity = []
    for calibration in manifest["calibration"].values():
        for path, sha in calibration["files"].items():
            assert digest(Path(path)) == sha, path
            checks["artifact_hashes"] += 1
    for name, job in jobs.items():
        if (root / "pause.request").exists():
            raise InterruptedError("pause.request")
        for path, sha in job["files"].items():
            assert digest(Path(path)) == sha, path
            checks["artifact_hashes"] += 1
        row = job["row"]
        timing = read_csv(root / "data" / f"{name}.timings.csv")
        assert [int(r["frame"]) for r in timing] == list(range(1, row["frames"] + 1))
        checks["timestamp_rows"] += len(timing)
        for metric in ("gpu_ms", "trace_ms"):
            assert math.isclose(statistics.median(float(r[metric]) for r in timing[16:]),
                                row[metric], abs_tol=1e-9)
        hardware = read_csv(root / "data" / f"{name}.hardware.csv")
        assert len(hardware) >= 2
        assert all(int(r["ac_online"]) == 1 and r["gpu"] == row["gpu"] and
                   r["driver"] == row["driver"] for r in hardware)
        checks["hardware_samples"] += len(hardware)
        activity = read_csv(root / "data" / f"{name}.activity.csv")
        assert not any(float(r["percent"]) > 1 and r["process"] not in
                       ("starcore-render", "dwm", "System", "Idle", "heartbeat") for r in activity)
        heartbeats = [datetime.fromisoformat(r["utc"]) for r in activity if r["process"] == "heartbeat"]
        assert len(heartbeats) >= 2
        gaps = [(b - a).total_seconds() for a, b in zip(heartbeats, heartbeats[1:])]
        assert max(gaps) <= 3, (name, max(gaps))
        assert abs((datetime.fromisoformat(hardware[-1]["utc"]) - heartbeats[-1]).total_seconds()) <= 3
        wall = re.search(r"\[(\d\d):(\d\d):(\d\d)\.(\d{3})\].*Wall: ([\d.]+) ms/frame after setup",
                         (root / "data" / f"{name}.log").read_text())
        assert wall is not None
        anchor = datetime.fromisoformat(hardware[-1]["utc"]).astimezone()
        hour, minute, second, millis = map(int, wall.groups()[:4])
        end = anchor.replace(hour=hour, minute=minute, second=second, microsecond=millis * 1000)
        if end < anchor - timedelta(hours=12):
            end += timedelta(days=1)
        if end > anchor + timedelta(hours=12):
            end -= timedelta(days=1)
        after_setup_start = end - timedelta(milliseconds=float(wall[5]) * (row["frames"] - 1))
        assert heartbeats[0] <= after_setup_start, (name, "counter started after timed workload")
        assert (end - heartbeats[-1]).total_seconds() <= 3, (name, "counter ended too early")
        checks["max_heartbeat_gap_seconds"] = max(checks["max_heartbeat_gap_seconds"], max(gaps))
        checks["activity_heartbeats"] += len(heartbeats)
        ref = bank["references"][f"{row['shot']}-1280x720"]
        secondary = Path(ref["independent_reference"])
        assert digest(secondary) == ref["sha256"][1]
        sensitivity.append({"shot": row["shot"], "config": row["config"], "seed": row["seed"],
                            "primary_flip": row["flip"], "secondary_flip": ldr_flip(secondary, Path(row["image"]))})
        print(f"AUDITED {len(sensitivity)}/81 {name}", flush=True)
    write_csv(root / "reference-sensitivity-runs.csv", sensitivity)
    summary = read_csv(root / "summary.csv")
    assert len(summary) == 27
    output = []
    for group in summary:
        selected = [j["row"] for j in jobs.values() if j["row"]["shot"] == group["shot"] and
                    j["row"]["config"] == group["config"]]
        assert sorted(r["seed"] for r in selected) == [17, 29, 43]
        for metric in ("gpu_ms", "trace_ms", "flip"):
            assert math.isclose(float(group[f"{metric}_median"]),
                                statistics.median(r[metric] for r in selected), abs_tol=1e-9)
        alt = [r["secondary_flip"] for r in sensitivity if r["shot"] == group["shot"] and
               r["config"] == group["config"]]
        output.append({"shot": group["shot"], "config": group["config"],
                       "primary_flip": float(group["flip_median"]),
                       "secondary_flip": statistics.median(alt),
                       "secondary_min": min(alt), "secondary_max": max(alt),
                       "reference_pair_flip": float(group["reference_noise"])})
    write_csv(root / "reference-sensitivity-summary.csv", output)
    lag_hashes = {}
    horizon = manifest["lag_selected_horizon"]
    for result in manifest["lag_selected_results"]:
        name = f"lag-{result['config']}-{horizon}"
        saved = manifest["lag"][name]
        curve = Path(saved["curve"])
        assert digest(curve) == saved["curve_sha256"]
        rows = read_csv(curve)
        assert [int(r["still_frame"]) for r in rows] == list(range(1, horizon + 1))
        assert [int(r["frame"]) for r in rows] == list(range(82, 82 + horizon))
        recomputed = settling([float(r["flip"]) for r in rows])
        for key, value in recomputed.items():
            assert result[key] == value, (name, key)
        for path in (root / "data").glob(f"{name}.*"):
            lag_hashes[str(path)] = digest(path)
        for frame in range(82, 82 + horizon):
            path = root / "data" / f"{name}_{frame:04d}.png"
            lag_hashes[str(path)] = digest(path)
    checks.update(summary_rows=27, still_scores=2 * horizon, lag_artifact_sha256=lag_hashes,
                  second_reference_scores=len(sensitivity), passed=True)
    (root / "verification.json").write_text(json.dumps(checks, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in checks.items() if k != "lag_artifact_sha256"}), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    verify(parser.parse_args().root.resolve())
