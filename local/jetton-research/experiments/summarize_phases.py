#!/usr/bin/env python3
"""Summarize instrumented collation phases for steady productive shard blocks."""

from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("run", type=Path)
    args = parser.parse_args()

    with (args.run / "blocks.csv").open(newline="") as source:
        roots = {
            row["root_hash"]
            for row in csv.DictReader(source)
            if row["steady"] == "1" and int(row["transactions"]) > 0
        }
    with (args.run / "collation.jsonl").open() as source:
        records = [
            record
            for line in source
            if (record := json.loads(line)).get("root_hash") in roots
        ]
    if not records:
        raise RuntimeError(f"no steady productive collation records in {args.run}")

    transactions = sum(int(record["transactions"]) for record in records)
    real: defaultdict[str, float] = defaultdict(float)
    cpu: defaultdict[str, float] = defaultdict(float)
    for record in records:
        for name, seconds in record["work_real_s"].items():
            real[name] += float(seconds)
        for name, seconds in record["work_cpu_s"].items():
            cpu[name] += float(seconds)

    total_real = real["total"]
    print(f"run={args.run.name} blocks={len(records)} transactions={transactions}")
    nested = {
        "trx_tvm",
        "trx_storage",
        "trx_other",
        "trx_external",
        "trx_internal",
        "new_messages_route",
        "new_messages_prepare",
        "new_messages_execute",
        "new_messages_commit",
        "prelim_storage",
        "account_dict_estimator_update",
        "account_dict_estimator_proof",
        "account_dict_estimator_wait",
        "account_block_build",
        "final_account_dict_update",
        "account_storage_dict",
    }

    print("top-level phase               real_ms/block  cpu_ms/block  real_us/tx  real_share")
    top_level = [key for key in real if key != "total" and key not in nested]
    for name in sorted(top_level, key=lambda key: real[key], reverse=True):
        print(
            f"{name:29} {real[name] * 1e3 / len(records):13.3f}"
            f" {cpu[name] * 1e3 / len(records):12.3f}"
            f" {real[name] * 1e6 / transactions:11.3f}"
            f" {real[name] / total_real * 100:10.2f}%"
        )
    accounted = sum(real[name] for name in top_level)
    print(
        f"{'unaccounted':29} {(total_real - accounted) * 1e3 / len(records):13.3f}"
        f" {'':12} {(total_real - accounted) * 1e6 / transactions:11.3f}"
        f" {(total_real - accounted) / total_real * 100:10.2f}%"
    )
    print(f"{'total':29} {total_real * 1e3 / len(records):13.3f}")
    print("\nnested transaction timing    real_ms/block  cpu_ms/block  real_us/tx  real_share")
    for name in sorted(nested, key=lambda key: real[key], reverse=True):
        print(
            f"{name:29} {real[name] * 1e3 / len(records):13.3f}"
            f" {cpu[name] * 1e3 / len(records):12.3f}"
            f" {real[name] * 1e6 / transactions:11.3f}"
            f" {real[name] / total_real * 100:10.2f}%"
        )
    estimator_updates = sum(int(record.get("account_dict_estimator_updates", 0)) for record in records)
    estimator_proofs = sum(int(record.get("account_dict_estimator_proofs", 0)) for record in records)
    estimator_batches = sum(int(record.get("account_dict_estimator_batches", 0)) for record in records)
    estimator_async_batches = sum(int(record.get("account_dict_estimator_async_batches", 0)) for record in records)
    estimator_async_queue_max = max(
        (int(record.get("account_dict_estimator_async_queue_max", 0)) for record in records), default=0
    )
    estimator_estimated_bytes = sum(int(record.get("account_dict_estimator_estimated_bytes", 0)) for record in records)
    estimator_reused = sum(int(record.get("account_dict_estimator_reused", 0)) for record in records)
    estimator_corrections = sum(int(record.get("account_dict_estimator_corrections", 0)) for record in records)
    if estimator_updates or estimator_proofs or estimator_batches or estimator_reused or estimator_corrections:
        print(
            f"\nestimator operations: updates={estimator_updates} proofs={estimator_proofs} "
            f"batches={estimator_batches} "
            f"async_batches={estimator_async_batches} async_queue_max={estimator_async_queue_max} "
            f"reused={estimator_reused} corrections={estimator_corrections}"
        )
        if estimator_estimated_bytes:
            print(
                f"estimator proof contribution: bytes={estimator_estimated_bytes} "
                f"bytes/block={estimator_estimated_bytes / len(records):.1f} "
                f"bytes/update={estimator_estimated_bytes / estimator_updates:.1f}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
