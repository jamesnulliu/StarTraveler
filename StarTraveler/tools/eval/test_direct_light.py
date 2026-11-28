"""Exercise the measurement contract using deterministic renderer outputs in a temporary folder."""
import csv
import json
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from PIL import Image

import direct_light as dl


class DirectEvaluationTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def test_motion_references_and_reduction(self):
        spec = json.loads((dl.HERE / "direct-light.json").read_text())
        spec["resolution"] = [48, 32]
        spec["warmup_frames"] = 1
        spec["shots"] = [dict(spec["shots"][0], frames=6, still=2)]
        args = SimpleNamespace(output=self.root, assets=self.root, prepare_exe="prepare", exe="render")
        measurements = []

        def fixture(command, log):
            command = list(map(str, command))

            def value(option):
                return command[command.index(option) + 1]

            if command[0] == "prepare":
                self.assertEqual(value("--frames"), "4")
                start = dict(position=[0, 0, 0], yaw=-90, pitch=0, fov=70)
                poses = dict(initial=start, end=dict(start, yaw=-91.5), motion_updates=3)
                (log.parent / "poses.json").write_text(json.dumps(poses))
                return ""
            png = Path(value("-o"))
            size = (int(value("-W")), int(value("-H")))
            level = 0 if "--direct-only" in command and png.stem.endswith("-mirror") else 96
            if png.stem.startswith("endpoint"):
                moving = png.stem == "endpoint-motion"
                self.assertEqual(value("-f"), "4" if moving else "1")
                self.assertEqual(value("--camera-angles"), "-90" if moving else "-91.5")
                self.assertEqual("--turn-entity" in command, moving)
            if "--gpu-timings" in command:
                frames = int(value("-f"))
                if png.stem.startswith("reference"):
                    self.assertEqual((command[1], frames, value("-s"), value("--camera-angles")),
                                     (str(log.parent.parent / "end.yaml"), 2048, "4", "-91.5"))
                    self.assertNotIn("--turn-entity", command)
                    level += int(png.stem.endswith("noise"))
                else:
                    self.assertEqual((frames, value("--still"), value("--turn-entity")),
                                     (6, "2", "Ship"))
                    measurements.append((png.parent.name, png.stem))
                    level += 15
                path = Path(value("--gpu-timings"))
                with path.open("w") as stream:
                    print("frame,gpu_ms,trace_ms,setup_frame", file=stream)
                    for i in range(1, frames + 1):
                        total = 100 if i == 1 or (frames == 6 and i > 4) else 3
                        trace = int("--raster" not in command)
                        print(f"{i},{total},{trace},{int(i == 1)}", file=stream)
            Image.new("RGB", size, (level,) * 3).save(png)
            return "Vulkan device: fixture | test driver | 1\nEndpoint accumulation: 2048 frames"

        dl.validate(spec)
        with patch.object(dl, "run", side_effect=fixture):
            rows = dl.evaluate(args, spec)
        self.assertEqual(len(rows), 34)
        self.assertEqual({r["gpu_ms"] for r in rows}, {3})
        self.assertEqual({r["trace_ms"] for r in rows if r["config"] == "raster"}, {0})
        self.assertTrue(all(r["flip"] > 0 for r in rows if r["config"] != "reference"))
        self.assertEqual([name for mode, name in measurements if mode == "whole"][::5],
                         ["raster-17", "traced-half-29", "maps-half-43"])
        dl.write_report(self.root, spec, rows)
        self.assertEqual(len(list(self.root.rglob("crops.png"))), 2)
        self.assertEqual(len(list(self.root.rglob("error-time.png"))), 2)
        self.assertIn("Reference-pair FLIP was", (self.root / "report.md").read_text())

    def test_times_exclude_warmup_and_still_suffix(self):
        path = self.root / "times.csv"
        path.write_text("frame,gpu_ms,trace_ms,setup_frame\n"
                        "1,40,20,1\n2,30,20,0\n3,6,2,0\n4,10,4,0\n5,50,15,0\n")
        self.assertEqual(dl.timings(path, 5, warmup=2, still=1), (8, 3))

    def test_missing_and_invalid_timestamps(self):
        path = self.root / "times.csv"
        for bad in [[], [2, "nan", 1, 0], [2, 1, 2, 0], [3, 4, 2, 0], [2, 4, 1, 1]]:
            with path.open("w", newline="") as stream:
                writer = csv.writer(stream)
                writer.writerows([["frame", "gpu_ms", "trace_ms", "setup_frame"], [1, 4, 2, 1]])
                if bad:
                    writer.writerow(bad)
            with self.assertRaises(ValueError):
                dl.timings(path, 2)

    def test_leaking_indirect_light_fails_control(self):
        def bright(exe, scene, png, flags, size, **kwargs):
            Image.new("RGB", tuple(size), "white").save(png)
            return "device", "driver"

        with patch.object(dl, "render", side_effect=bright), self.assertRaisesRegex(ValueError, "mirror"):
            dl.check_direct("fixture", self.root / "controls")


if __name__ == "__main__":
    unittest.main()
