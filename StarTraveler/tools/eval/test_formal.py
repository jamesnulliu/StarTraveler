import json
import subprocess
import sys
import threading
import time

import pytest

from check_measurement import digest
from run_formal import Formal, Paused


def test_pause_terminates_inflight_child_and_preserves_completed_jobs(tmp_path, monkeypatch):
    runner = Formal.__new__(Formal)
    runner.root = runner.out = tmp_path
    runner.pause_path = tmp_path / "pause.request"
    runner.report_path = tmp_path / "manifest.json"
    runner.report = {"jobs": {"already_done": {"status": "passed"}}}
    runner.hardware = {"gpu": "test-gpu"}
    runner.monitor_activity = False
    runner.monitor_hardware = False
    runner.require_ac = False
    original = subprocess.Popen
    processes = []

    def spy(*args, **kwargs):
        process = original(*args, **kwargs)
        processes.append(process)
        return process

    monkeypatch.setattr(subprocess, "Popen", spy)
    trigger = threading.Timer(.5, lambda: runner.pause_path.write_text("quota"))
    trigger.start()
    began = time.monotonic()
    try:
        with pytest.raises(Paused, match="quota"):
            runner.run("interrupt-test", [sys.executable, "-u", "-c",
                                         "import time; print('test-gpu'); time.sleep(60)"])
    finally:
        trigger.cancel()
    assert time.monotonic() - began < 5
    assert len(processes) == 1 and processes[0].poll() is not None
    saved = json.loads(runner.report_path.read_text())
    assert saved["active_pid"] is None
    assert saved["jobs"]["already_done"]["status"] == "passed"


def test_resume_rejects_changed_completed_artifact(tmp_path):
    artifact = tmp_path / "image.png"
    artifact.write_bytes(b"original")
    runner = Formal.__new__(Formal)
    runner.report = {"jobs": {"done": {"status": "passed", "files": {str(artifact): digest(artifact)}}}}
    assert runner.cached("done")["status"] == "passed"
    artifact.write_bytes(b"changed")
    with pytest.raises(ValueError, match="modified"):
        runner.cached("done")


def test_background_app_activity_rejects_timing_but_os_and_renderer_are_allowed(tmp_path):
    activity = tmp_path / "activity.csv"
    activity.write_text('utc,pid,process,engine,percent\nnow,123,renderer,3d,99\n'
                        'now,2,dwm,3d,4\n')
    Formal.check_activity(activity, 123)
    with activity.open("a") as stream:
        stream.write('now,4,ChatGPT,3d,5\n')
    with pytest.raises(Paused, match="background_gpu: ChatGPT"):
        Formal.check_activity(activity, 123)


@pytest.mark.parametrize("power", [0, 255])
def test_disconnected_or_unknown_power_pauses(tmp_path, monkeypatch, power):
    runner = Formal.__new__(Formal)
    runner.pause_path = tmp_path / "pause.request"
    monkeypatch.setattr("run_formal.ac_power", lambda: power)
    with pytest.raises(Paused, match="ac_power_required"):
        runner.check_pause()


def test_counter_startup_retries_before_renderer_launch(tmp_path, monkeypatch):
    runner = Formal.__new__(Formal)
    runner.pause_path = tmp_path / "pause.request"
    runner.require_ac = False
    activity = tmp_path / "activity.csv"
    original = subprocess.Popen
    attempts = []

    def fake_counter(*args, **kwargs):
        code = "raise SystemExit(1)" if not attempts else (
            "from pathlib import Path; import time; "
            f"Path({str(activity)!r}).write_text('utc,pid,process,engine,percent\\nnow,0,heartbeat,,0\\n'); "
            "time.sleep(60)")
        process = original([sys.executable, "-u", "-c", code], **kwargs)
        attempts.append(process)
        return process

    monkeypatch.setattr(subprocess, "Popen", fake_counter)
    try:
        with (tmp_path / "monitor.log").open("w") as stream:
            monitor = runner.start_activity_monitor(activity, stream)
        assert len(attempts) == 2
        assert attempts[0].poll() == 1 and monitor.poll() is None
    finally:
        for process in attempts:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
