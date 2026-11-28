"""Decode a window rehearsal, sample its seven phases and reject a frozen capture."""
import argparse
import json
import re
import subprocess
from pathlib import Path

import imageio_ffmpeg
import numpy as np
from PIL import Image, ImageDraw
from check_measurement import digest, write_csv
from run_gpu_regression import save


LABELS = ["Cockpit Full / DLSS K", "Cockpit Half / DLSS K", "Cockpit Full / RR",
          "Mirror-glass Full / DLSS K", "Mirror-glass Full / RR",
          "Star turn Full / DLSS K", "Star turn Full / RR"]


def validate_fixed_configuration(log):
    samples = [line for line in log.splitlines() if "Rehearsal sample:" in line]
    assert samples, "Missing fixed-configuration evidence"
    phases = set()
    for line in samples:
        values = dict(re.findall(r"(\w+)=([^ ]+)", line))
        phase = int(values["phase"])
        phases.add(phase)
        assert float(values["dlss_exposure"]) == 1
        assert float(values["tone_exposure"]) == 1 and values["auto_exposure"] == "false"
        assert abs(float(values["temporal_ms"]) - 1000/60) < .00001
        assert values["tone"] == "ACES" and values["spp"] == "1"
        assert values["seed"] == "29" and values["effects_off"].strip() == "true"
        assert values["trace"] == ("Half" if phase == 1 else "Full")
        assert values["active"] == ("DLSS-RR" if phase in (2, 4, 6) else "DLSS")
        assert values["output"] == "1280x698" and values["internal"] == "853x465"
    assert phases == set(range(7))
    return {"passed": True, "sample_rows": len(samples), "phases": sorted(phases),
            "rr_sr_exposure_field": "inactive in RR; RR retains unity input"}


def main(root):
    root = root.resolve()
    manifest = json.loads((root / "manifest.json").read_text())
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    video = root / "flight.mp4"
    report = {"passed": False, "manual_visual_review_required": True,
              "video_sha256": digest(video), "verifier_sha256": digest(Path(__file__)),
              "manifest_sha256": digest(root / "manifest.json")}
    try:
        assert manifest["complete"] and manifest["game_exit"] == 0
        if manifest.get("configuration_version") == "fixed-exposure-v1":
            report["configuration_checks"] = validate_fixed_configuration(
                (root / "game.log").read_text(encoding="utf-8"))
        for path, sha in manifest["files"].items():
            assert digest(Path(path)) == sha, path
        decode = subprocess.run([ffmpeg, "-hide_banner", "-i", str(video), "-f", "null", "-"],
                                capture_output=True, text=True, timeout=180)
        (root / "decode.log").write_text(decode.stderr, encoding="utf-8")
        assert decode.returncode == 0, "Video does not decode completely"
        match = re.search(r"Duration: (\d+):(\d+):([\d.]+)", decode.stderr)
        assert match, "Video duration missing"
        h, m, seconds = map(float, match.groups())
        duration = h * 3600 + m * 60 + seconds
        frames = int(re.findall(r"frame=\s*(\d+)", decode.stderr)[-1])
        assert duration >= 7 * manifest["seconds_per_phase"] - 1.0
        assert re.search(r"1280x720", decode.stderr)
        starts = [max(0.0, manifest["phase_seen_wall_seconds"][str(i)] -
                      manifest["recording_start_wall_seconds"]) for i in range(7)]
        assert all(a < b for a, b in zip(starts, starts[1:]))
        ends = starts[1:] + [duration]
        sheet = Image.new("RGB", (960, 4 * 295), "#151515")
        draw = ImageDraw.Draw(sheet)
        pictures = []
        chapters = []
        for i, (start, end) in enumerate(zip(starts, ends)):
            assert end - start >= manifest["seconds_per_phase"] * .5
            timestamp = start + (end - start) * .5
            path = root / f"phase-{i + 1}.png"
            result = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error",
                                     "-ss", str(timestamp), "-i", str(video),
                                     "-frames:v", "1", "-y", str(path)],
                                    capture_output=True, text=True, timeout=30)
            assert result.returncode == 0 and path.exists(), result.stderr
            with Image.open(path) as im:
                assert im.size == (1280, 720)
                pixels = np.asarray(im.convert("RGB"), dtype=np.float32) / 255
                pictures.append(pixels[140:650, 100:1180])  # Exclude changing HUD text.
                x, y = (i % 2) * 480, (i // 2) * 295
                sheet.paste(im.convert("RGB").resize((480, 270)), (x, y + 25))
                label = LABELS[i]
                if not manifest.get("configuration_version") and i == 5:
                    label = "Star turn Half / DLSS K"
                draw.text((x + 4, y + 3), f"{i + 1}. {label} / {timestamp:.1f}s", fill="white")
            chapters.append({"phase": i + 1, "label": label, "start_s": start,
                             "end_s": end, "sample_s": timestamp,
                             "sample_sha256": digest(path)})
        sheet.save(root / "contact-sheet.jpg")
        write_csv(root / "chapters.csv", chapters)
        # This explicitly catches the known GDI failure: every phase was the first cockpit frame.
        changes = {"cockpit_to_mirror": float(np.mean(np.abs(pictures[0] - pictures[3]))),
                   "cockpit_to_star": float(np.mean(np.abs(pictures[0] - pictures[5])))}
        report.update(duration_seconds=duration, decoded_frames=frames,
                      scene_mean_absolute_changes=changes, chapters=chapters,
                      contact_sheet_sha256=digest(root / "contact-sheet.jpg"))
        assert all(value > .01 for value in changes.values()), "Frozen or wrong-scene video"
        report["passed"] = True
        print(json.dumps({k: v for k, v in report.items() if k != "chapters"}), flush=True)
        return 0
    except BaseException as error:
        report["failure"] = str(error)
        raise
    finally:
        save(root / "verification.json", report)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    raise SystemExit(main(parser.parse_args().root))
