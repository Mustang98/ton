#!/usr/bin/env python3
"""Run a comparable one-shard jetton collation experiment."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
RUNNER = ROOT / "local/jetton-research/simulator/run.py"
STATE_MANIFEST = ROOT / "_local/jetton-research/generated-db-45m/state/manifest.json"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--pool-size", type=int, default=10_000)
    parser.add_argument("--rate", type=float, default=380.0)
    parser.add_argument("--block-limit-mul", type=int, default=2)
    parser.add_argument("--gas-limit-mul", type=int)
    parser.add_argument("--duration", type=float, default=120.0)
    parser.add_argument("--warmup", type=float, default=10.0)
    parser.add_argument("--drain", type=float, default=30.0)
    parser.add_argument("--startup-timeout", type=float, default=180.0)
    parser.add_argument("--base-port", type=int, default=5_000)
    parser.add_argument("--node-env", action="append", default=[])
    args = parser.parse_args()

    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    run_dir = ROOT / "local/jetton-research/experiments/runs" / f"{args.name}-{stamp}"
    command = [
        sys.executable,
        str(RUNNER),
        "--state-manifest",
        str(STATE_MANIFEST),
        "--build-dir",
        str(args.build_dir),
        "--workdir",
        str(run_dir),
        "--pool-size",
        str(args.pool_size),
        "--init-mode",
        "0",
        "--rate",
        str(args.rate),
        "--duration",
        str(args.duration),
        "--warmup",
        str(args.warmup),
        "--drain",
        str(args.drain),
        "--startup-timeout",
        str(args.startup_timeout),
        "--block-limit-mul",
        str(args.block_limit_mul),
        "--gas-limit-mul",
        str(args.gas_limit_mul if args.gas_limit_mul is not None else args.block_limit_mul),
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
    print(run_dir)
    return subprocess.run(command, cwd=ROOT, env=env).returncode


if __name__ == "__main__":
    raise SystemExit(main())
