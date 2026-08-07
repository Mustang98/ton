#!/usr/bin/env python3
"""Run the fixed 2x jetton baseline and attach perf during its spam phase."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
RUNNER = ROOT / "local/jetton-research/simulator/run.py"
STATE_MANIFEST = ROOT / "_local/jetton-research/generated-db-45m/state/manifest.json"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-clang-release")
    parser.add_argument("--base-port", type=int, default=5_000)
    parser.add_argument("--node-env", action="append", default=[])
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--frequency", type=int, default=499)
    parser.add_argument("--profile-seconds", type=float, default=55.0)
    parser.add_argument("--startup-timeout", type=float, default=360.0)
    return parser.parse_args()


def validator_pid(node_dir: Path, executable: Path) -> int | None:
    expected_cwd = node_dir.resolve()
    expected_exe = executable.resolve()
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if (entry / "cwd").resolve() != expected_cwd:
                continue
            if (entry / "exe").resolve() == expected_exe:
                return int(entry.name)
        except (FileNotFoundError, PermissionError):
            continue
    return None


def wait_for_spam(run_dir: Path, executable: Path, process: subprocess.Popen[bytes]) -> int:
    manifest_path = run_dir / "run.json"
    deadline = time.monotonic() + 1_800
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"benchmark exited before spam phase: {process.returncode}")
        if manifest_path.exists():
            try:
                manifest = json.loads(manifest_path.read_text())
            except (json.JSONDecodeError, OSError):
                manifest = {}
            if manifest.get("spam_command"):
                pid = validator_pid(run_dir / "network/node1", executable)
                if pid is not None:
                    return pid
        time.sleep(0.1)
    raise TimeoutError("timed out waiting for benchmark spam phase")


def main() -> int:
    args = parse_args()
    run_dir = args.run_dir.resolve()
    if run_dir.exists():
        raise FileExistsError(run_dir)
    run_dir.parent.mkdir(parents=True, exist_ok=True)
    harness_log = run_dir.with_name(run_dir.name + ".harness.log")
    build_dir = args.build_dir.resolve()
    validator = build_dir / "validator-engine/validator-engine"
    command = [
        sys.executable,
        str(RUNNER),
        "--state-manifest",
        str(STATE_MANIFEST),
        "--build-dir",
        str(build_dir),
        "--workdir",
        str(run_dir),
        "--pool-size",
        "10000",
        "--init-mode",
        "0",
        "--rate",
        "380",
        "--duration",
        str(args.duration),
        "--warmup",
        "10",
        "--drain",
        "20",
        "--startup-timeout",
        str(args.startup_timeout),
        "--block-limit-mul",
        "2",
        "--gas-limit-mul",
        "2",
        "--force-empty-first-slot",
        "--node-threads",
        "8",
        "--base-port",
        str(args.base_port),
    ]
    for item in args.node_env:
        command.extend(["--node-env", item])
    env = os.environ.copy()
    env["PYTHONPATH"] = str(ROOT / "test/tontester/src")
    with harness_log.open("wb") as log:
        benchmark = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            pid = wait_for_spam(run_dir, validator, benchmark)
            perf_command = [
                "perf",
                "record",
                "--freq",
                str(args.frequency),
                "--call-graph",
                "dwarf,8192",
                "--pid",
                str(pid),
                "--output",
                str(run_dir / "perf.data"),
                "--",
                "sleep",
                str(args.profile_seconds),
            ]
            perf = subprocess.run(perf_command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            benchmark_rc = benchmark.wait()
        except BaseException:
            benchmark.terminate()
            benchmark.wait()
            raise
    if perf.returncode != 0:
        raise RuntimeError(f"perf record failed with status {perf.returncode}")
    if benchmark_rc != 0:
        raise RuntimeError(f"benchmark failed with status {benchmark_rc}")
    with (run_dir / "perf-report.txt").open("wb") as output:
        report = subprocess.run(
            [
                "perf",
                "report",
                "--stdio",
                "--input",
                str(run_dir / "perf.data"),
                "--percent-limit",
                "0.1",
            ],
            cwd=ROOT,
            stdout=output,
            stderr=subprocess.STDOUT,
        )
    return report.returncode


if __name__ == "__main__":
    raise SystemExit(main())
