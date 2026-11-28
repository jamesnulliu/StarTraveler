"""Capture fixture logs/modules without changing thresholds.

Image/linear exports require an archived diagnostic build with capture hooks;
the production GPU test executable intentionally has no such hooks.
"""
import argparse
import json
import os
import threading
from pathlib import Path

from check_measurement import digest
from run_gpu_regression import Paused, Regression, junit_result, save


def module_paths(pid):
    import ctypes as c
    from ctypes import wintypes as w
    kernel = c.WinDLL("kernel32", use_last_error=True)
    psapi = c.WinDLL("psapi", use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.CloseHandle.argtypes = [w.HANDLE]
    psapi.EnumProcessModulesEx.argtypes = [w.HANDLE, c.POINTER(w.HMODULE), w.DWORD,
                                         c.POINTER(w.DWORD), w.DWORD]
    psapi.GetModuleFileNameExW.argtypes = [w.HANDLE, w.HMODULE, w.LPWSTR, w.DWORD]
    handle = kernel.OpenProcess(0x410, False, pid)
    if not handle:
        return []
    try:
        modules = (w.HMODULE * 1024)()
        needed = w.DWORD()
        if not psapi.EnumProcessModulesEx(handle, modules, c.sizeof(modules), c.byref(needed), 3):
            return []
        paths = []
        for module in modules[:min(1024, needed.value // c.sizeof(w.HMODULE))]:
            path = c.create_unicode_buffer(32768)
            if psapi.GetModuleFileNameExW(handle, module, path, len(path)) and "nvngx" in path.value.lower():
                paths.append(path.value)
        return sorted(paths)
    finally:
        kernel.CloseHandle(handle)


def main(args):
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    os.environ["STARCORE_GPU_TEST_DUMP_DIR"] = str(root)
    for assignment in args.environment:
        name, value = assignment.split("=", 1)
        os.environ[name] = value
    test = next(t for t in json.loads(args.inventory.read_text(encoding="utf-8-sig"))["tests"]
                if "fewer pixels rendered" in t["name"])
    test["command"] = list(test["command"])
    if args.test_exe:
        test["command"][0] = str(args.test_exe.resolve())
    runner = Regression.__new__(Regression)
    runner.root = root
    runner.path = root / "manifest.json"
    runner.pause = args.pause_file.resolve()
    runner.report = {"active_test": test["name"], "complete": False,
                     "test_exe_sha256": digest(Path(test["command"][0])),
                     "environment": args.environment}
    command = test["command"] + ["--rng-seed", "726919427", "--reporter", "junit",
                                 "--out", str(root / "junit.xml")]
    save(root / "command.json", command)
    cwd = next(p["value"] for p in test["properties"] if p["name"] == "WORKING_DIRECTORY")
    stop = threading.Event()
    loaded = set()

    def observe():
        try:
            while not stop.is_set():
                pid = runner.report.get("active_pid")
                if pid and os.name == "nt":
                    loaded.update(module_paths(pid))
                stop.wait(.25)
        except Exception as error:
            runner.report["module_capture_error"] = str(error)

    observer = threading.Thread(target=observe, daemon=True)
    observer.start()
    try:
        code, elapsed = runner.run_process(command, cwd, root / "stdout.log")
        runner.report.update(junit_result(root / "junit.xml", code), seconds=elapsed,
                             complete=True, state="complete")
        stop.set()
        observer.join(timeout=2)
        runner.report["loaded_ngx_modules"] = {
            path: digest(Path(path)) for path in sorted(loaded) if Path(path).exists()}
        runner.persist()
        print(json.dumps(runner.report), flush=True)
        return 0
    except Paused as error:
        runner.report.update(state="paused", pause_reason=str(error))
        runner.persist()
        return 75
    finally:
        stop.set()
        observer.join(timeout=2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--test-exe", type=Path)
    parser.add_argument("--pause-file", type=Path, default=Path("renders/dlss-diagnosis/pause.request"))
    parser.add_argument("--environment", action="append", default=[])
    raise SystemExit(main(parser.parse_args()))
