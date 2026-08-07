#!/usr/bin/env python3
"""Print high-signal collation metrics for simulator run directories."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path


def get(value: dict, *path: str, default: float = 0.0) -> float:
    current: object = value
    for part in path:
        if not isinstance(current, dict):
            return default
        current = current.get(part)
    return float(current) if isinstance(current, (int, float)) else default


def percentile(values: list[float], fraction: float) -> float:
    values.sort()
    if not values:
        return 0.0
    position = fraction * (len(values) - 1)
    low = int(position)
    high = min(low + 1, len(values) - 1)
    return values[low] * (high - position) + values[high] * (position - low)


VALIDATION_RE = re.compile(
    r"validateblock\(0,8000000000000000\):(\d+)#.*"
    r"Validate query work time = ([0-9.]+)s"
)


def productive_validation_ms(run: Path, seqnos: set[int]) -> list[float]:
    by_seqno: dict[int, float] = {}
    with (run / "network/node1/log").open(errors="replace") as source:
        for line in source:
            match = VALIDATION_RE.search(line)
            if match and int(match.group(1)) in seqnos:
                by_seqno[int(match.group(1))] = float(match.group(2)) * 1000
    return list(by_seqno.values())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runs", nargs="+", type=Path)
    args = parser.parse_args()
    columns = (
        "run",
        "jtps",
        "tps",
        "tx/block",
        "coll mean ms",
        "coll p95 ms",
        "val mean ms",
        "val p95 ms",
        "block mean MB",
        "block p95 MB",
        "ext examined",
        "ancestor skip",
        "pool skip",
        "ext rejected",
        "deadline",
        "queue end",
    )
    rows = []
    for run in args.runs:
        report = json.loads((run / "analysis.json").read_text())
        throughput = report["throughput"]
        collation = report["collation"]
        messages = collation["messages"]
        with (run / "blocks.csv").open(newline="") as source:
            productive_blocks = [
                row
                for row in csv.DictReader(source)
                if row["steady"] == "1" and int(row["transactions"]) > 0
            ]
        steady_roots = {row["root_hash"] for row in productive_blocks}
        steady_seqnos = {int(row["seqno"]) for row in productive_blocks}
        with (run / "collation.jsonl").open() as source:
            productive = [
                row
                for line in source
                if (row := json.loads(line)).get("root_hash") in steady_roots
            ]
        collation_ms = [float(row["work_real_s"]["total"]) * 1000 for row in productive]
        validation_ms = productive_validation_ms(run, steady_seqnos)
        block_mb = [float(row["actual_bytes"]) / 1_000_000 for row in productive]
        productive_transactions = sum(int(row["transactions"]) for row in productive)
        rows.append(
            (
                run.name,
                get(throughput, "steady", "tx3_jtps"),
                get(throughput, "steady", "raw_tps"),
                productive_transactions / len(productive) if productive else 0.0,
                sum(collation_ms) / len(collation_ms) if collation_ms else 0.0,
                percentile(collation_ms, 0.95),
                sum(validation_ms) / len(validation_ms) if validation_ms else 0.0,
                percentile(validation_ms, 0.95),
                sum(block_mb) / len(block_mb) if block_mb else 0.0,
                percentile(block_mb, 0.95),
                get(messages, "external_total"),
                sum(int(row.get("ext_msgs_ancestor_filtered", 0)) for row in productive),
                sum(int(row.get("ext_msgs_pool_filtered", 0)) for row in productive),
                get(messages, "external_rejected"),
                get(collation, "limits", "overload_reason_counts", "collation_deadline"),
                get(collation, "queues", "new_last"),
            )
        )
    widths = [
        max(len(columns[i]), *(len(f"{row[i]:.3f}") if i else len(row[i]) for row in rows))
        for i in range(len(columns))
    ]
    print("  ".join(columns[i].ljust(widths[i]) for i in range(len(columns))))
    for row in rows:
        print("  ".join((row[i] if i == 0 else f"{row[i]:.3f}").ljust(widths[i]) for i in range(len(columns))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
