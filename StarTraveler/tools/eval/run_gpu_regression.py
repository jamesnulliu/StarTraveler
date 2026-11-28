"""Run every discovered GPU case serially, retaining results and quota-pause checkpoints."""
import argparse
import json
import re
import subprocess
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path

from check_measurement import digest


class Paused(RuntimeError):
    pass


def save(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2), encoding="utf-8")
    for attempt in range(50):
        try:
            temporary.replace(path)
            break
        except PermissionError:
            if attempt == 49:
                raise
            time.sleep(.1)


def now():
    return datetime.now(timezone.utc).isoformat()


def junit_result(path, code):
    try:
        tree = ET.parse(path)
        cases = list(tree.iter("testcase"))
        failed = list(tree.iter("failure")) + list(tree.iter("error"))
        skipped = list(tree.iter("skipped"))
        status = "failed" if code not in (0, 4) or failed or not cases else (
            "skipped" if code == 4 or skipped else "passed")
        return {"status": status, "exit_code": code, "junit_cases": len(cases),
                "failure_elements": len(failed), "skip_elements": len(skipped)}
    except (OSError, ET.ParseError) as error:
        return {"status": "failed", "exit_code": code, "report_error": str(error)}


class Regression:
    def __init__(self, args):
        self.root = args.output.resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.path = self.root / "manifest.json"
        self.pause = self.root / "pause.request"
        inventory = json.loads(args.inventory.read_text(encoding="utf-8-sig"))
        self.tests = inventory["tests"]
        if args.name_filter:
            self.tests = [t for t in self.tests if re.search(args.name_filter, t["name"])]
        assert self.tests and all(t["name"].startswith("GPU") for t in self.tests)
        self.total = len(self.tests)
        # Resolve the previously timed-out case first, then retain CTest discovery order.
        self.tests.sort(key=lambda t: "a glossy surface a pane reflects" not in t["name"])
        exe = Path(self.tests[0]["command"][0])
        smi = subprocess.run(["nvidia-smi", "--query-gpu=name,driver_version", "--format=csv,noheader"],
                             capture_output=True, text=True, check=True, timeout=5).stdout.strip()
        self.signature = {"inventory_sha256": digest(args.inventory), "test_exe_sha256": digest(exe),
                          "selected_tests": [t["name"] for t in self.tests],
                          "gpu_driver": smi, "cpu_report_sha256": digest(args.cpu_report),
                          "dlls": {p.name: digest(p) for p in sorted(exe.parent.glob("*.dll"))}}
        self.report = json.loads(self.path.read_text()) if self.path.exists() else {
            "created_utc": now(), "signature": self.signature, "results": {}}
        if self.report["signature"] != self.signature:
            raise ValueError("Test binary, inventory, runtime, hardware or CPU report changed")
        if not (self.root / "cpu.junit.xml").exists():
            (self.root / "cpu.junit.xml").write_bytes(args.cpu_report.read_bytes())
        cpu = junit_result(self.root / "cpu.junit.xml", 0)
        assert cpu["status"] == "passed" and cpu["junit_cases"] == 109
        self.report.update(cpu=cpu, state="preparing", complete=False, updated_utc=now(),
                           runner_sha256=digest(Path(__file__)))
        self.report.pop("pause_reason", None)
        self.persist()

    def persist(self):
        self.report["updated_utc"] = now()
        save(self.path, self.report)

    def check_pause(self):
        if self.pause.exists():
            raise Paused(self.pause.read_text(encoding="utf-8-sig").strip())

    def run_process(self, command, cwd, log):
        self.check_pause()
        began = time.monotonic()
        with log.open("w", encoding="utf-8") as stream:
            process = subprocess.Popen(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT)
            last_progress = began
            try:
                self.report.update(active_pid=process.pid, state="running")
                self.persist()
                while process.poll() is None:
                    self.check_pause()
                    if time.monotonic() - last_progress >= 30:
                        print(f"RUNNING {self.report['active_test']} {time.monotonic() - began:.0f}s",
                              flush=True)
                        last_progress = time.monotonic()
                    time.sleep(.2)
                self.check_pause()
                return process.returncode, time.monotonic() - began
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
                self.report["active_pid"] = None
                self.persist()

    def execute(self):
        try:
            for index, test in enumerate(self.tests, 1):
                self.check_pause()
                name = test["name"]
                previous = self.report["results"].get(name)
                if previous:
                    for path, sha in previous["files"].items():
                        if digest(Path(path)) != sha:
                            raise ValueError(f"Completed test artifact changed: {path}")
                    continue
                case = self.root / f"case-{index:02d}"
                case.mkdir(exist_ok=True)
                xml = case / "junit.xml"
                log = case / "stdout.log"
                command = test["command"] + ["--reporter", "junit", "--out", str(xml)]
                save(case / "command.json", command)
                self.report.update(active_test=name, active_case=index)
                print(f"START {index}/{self.total} {name}", flush=True)
                cwd = next(p["value"] for p in test["properties"] if p["name"] == "WORKING_DIRECTORY")
                code, elapsed = self.run_process(command, cwd, log)
                result = {**junit_result(xml, code), "seconds": elapsed, "finished_utc": now(),
                          "files": {str(p): digest(p) for p in case.iterdir() if p.is_file()}}
                self.report["results"][name] = result
                self.persist()
                print(f"RESULT {index}/{self.total} {result['status']} {elapsed:.2f}s {name}", flush=True)
            statuses = [r["status"] for r in self.report["results"].values()]
            self.report.update(complete=True, state="complete", active_test=None,
                               counts={s: statuses.count(s) for s in ("passed", "failed", "skipped")},
                               all_passed=all(s == "passed" for s in statuses))
            self.persist()
            print(json.dumps(self.report["counts"]), flush=True)
            return 0 if self.report["all_passed"] else 1
        except Paused as error:
            self.report.update(state="paused", pause_reason=str(error), complete=False)
            self.persist()
            print(f"PAUSED {error}", flush=True)
            return 75
        except BaseException as error:
            self.report.update(state="failed", failure=str(error), complete=False)
            self.persist()
            raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--cpu-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--name-filter", help="Optional regex over discovered GPU case names")
    raise SystemExit(Regression(parser.parse_args()).execute())
