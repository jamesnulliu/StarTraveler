import json
import subprocess
import sys
import threading

import pytest

from run_gpu_regression import Paused, Regression, junit_result


def test_junit_failure_is_not_hidden_by_zero_exit_status(tmp_path):
    report = tmp_path / "junit.xml"
    report.write_text('<testsuites><testsuite><testcase name="case">'
                      '<failure message="assertion"/></testcase></testsuite></testsuites>')
    assert junit_result(report, 0)["status"] == "failed"
    report.write_text('<testsuites><testsuite><testcase name="case">'
                      '<skipped/></testcase></testsuite></testsuites>')
    assert junit_result(report, 4)["status"] == "skipped"


def test_pause_stops_inflight_test_and_keeps_completed_results(tmp_path, monkeypatch):
    runner = Regression.__new__(Regression)
    runner.root = tmp_path
    runner.path = tmp_path / "manifest.json"
    runner.pause = tmp_path / "pause.request"
    runner.report = {"active_test": "simulated", "results": {"completed": {"status": "passed"}}}
    original = subprocess.Popen
    children = []

    def track(*args, **kwargs):
        child = original(*args, **kwargs)
        children.append(child)
        return child

    monkeypatch.setattr(subprocess, "Popen", track)
    trigger = threading.Timer(.5, lambda: runner.pause.write_text("quota"))
    trigger.start()
    try:
        with pytest.raises(Paused, match="quota"):
            runner.run_process([sys.executable, "-c", "import time; time.sleep(60)"],
                               str(tmp_path), tmp_path / "stdout.log")
    finally:
        trigger.cancel()
    assert len(children) == 1 and children[0].poll() is not None
    saved = json.loads(runner.path.read_text())
    assert saved["active_pid"] is None
    assert saved["results"]["completed"]["status"] == "passed"
