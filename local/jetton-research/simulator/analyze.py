#!/usr/bin/env python3
"""Join accepted workchain blocks with collator records and summarize a run."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable


TOP_LEVEL_PHASE_KEYS = (
    "preinit",
    "queue_cleanup",
    "dispatch",
    "ticktock",
    "inbound_internal",
    "inbound_external",
    "new_messages",
    "final_storage",
    "enqueue_new_messages",
    "combine_account_transactions",
    "create_shard_state",
    "create_block",
    "create_collated_data",
    "create_block_candidate",
)

LEGACY_PHASE_KEYS = (
    "preinit",
    "queue_cleanup",
    "prelim_storage",
    "trx_tvm",
    "trx_storage",
    "trx_other",
    "final_storage",
    "enqueue_new_messages",
    "combine_account_transactions",
    "create_shard_state",
    "create_block",
    "create_collated_data",
    "create_block_candidate",
)

LOAD_FRACTION_FIELDS = {
    "queue_cleanup": "load_fraction_queue_cleanup",
    "dispatch": "load_fraction_dispatch",
    "internals": "load_fraction_internals",
    "externals": "load_fraction_externals",
    "new_msgs": "load_fraction_new_msgs",
}

LIMIT_CATEGORY_FIELDS = {
    "bytes": "limit_category_bytes",
    "gas": "limit_category_gas",
    "lt_delta": "limit_category_lt_delta",
    "collated_data_bytes": "limit_category_collated_data_bytes",
}

LIMIT_CLASS_NAMES = {0: "underload", 1: "normal", 2: "soft", 3: "medium", 4: "hard"}

OVERLOAD_REASON_NAMES = {
    1: "block_limits",
    2: "out_queue_force_split",
    3: "collation_deadline",
    4: "dispatch_queue",
}

INTEGER_COLUMNS = {
    "steady",
    "seqno",
    "utime",
    "observed_at_unix_ms",
    "transactions",
    "tx1",
    "tx2_attempts",
    "tx2_success",
    "tx3_attempts",
    "tx3_success",
    "tx4_attempts",
    "tx4_success",
    "aborted",
    "after_merge",
    "before_split",
    "after_split",
    "want_split",
    "want_merge",
}

TRACE_INTEGER_COLUMNS = {
    "steady_offer",
    "ordinal",
    "sender_id",
    "recipient_id",
    "wallet_seqno",
    "offered_at_unix_ms",
    "tx1_block",
    "tx2_block",
    "tx3_block",
    "tx4_block",
}

TRACE_FLOAT_COLUMNS = {
    "external_to_tx1_ms",
    "tx1_to_tx2_ms",
    "tx2_to_tx3_ms",
    "tx3_to_tx4_ms",
    "external_to_tx3_ms",
    "external_to_tx4_ms",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--json-out", type=Path)
    return parser.parse_args()


def load_blocks(path: Path) -> list[dict[str, Any]]:
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source))
    for row in rows:
        for key in INTEGER_COLUMNS & row.keys():
            row[key] = int(row[key])
        if "root_hash" in row:
            row["root_hash"] = row["root_hash"].upper()
        row.setdefault("shard", "0x8000000000000000")
        row["shard"] = row["shard"].lower()
    return rows


def load_collations(path: Path) -> list[dict[str, Any]]:
    records = []
    with path.open(errors="replace") as source:
        for line_number, line in enumerate(source, 1):
            if "JETTON_SIM_COLLATION" in line:
                line = line[line.index("JETTON_SIM_COLLATION") + len("JETTON_SIM_COLLATION") :]
            start = line.find("{")
            end = line.rfind("}")
            if start < 0 or end < start:
                if line.strip():
                    raise RuntimeError(f"invalid collation record at {path}:{line_number}")
                continue
            records.append(json.loads(line[start : end + 1]))
    return records


def load_traces(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source))
    for row in rows:
        for key in TRACE_INTEGER_COLUMNS & row.keys():
            row[key] = int(row[key]) if row[key] else None
        for key in TRACE_FLOAT_COLUMNS & row.keys():
            row[key] = float(row[key]) if row[key] else None
        for stage in range(1, 5):
            row.setdefault(f"tx{stage}_shard", "0x8000000000000000")
            row[f"tx{stage}_shard"] = row[f"tx{stage}_shard"].lower()
    return rows


def load_topology(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    grouped: dict[int, dict[str, Any]] = {}
    with path.open(newline="") as source:
        for row in csv.DictReader(source):
            observed_at = int(row["observed_at_unix_ms"])
            event = grouped.setdefault(
                observed_at,
                {
                    "observed_at_unix_ms": observed_at,
                    "shard_count": int(row["shard_count"]),
                    "shards": [],
                },
            )
            event["shards"].append(
                {"shard": row["shard"].lower(), "top_seqno": int(row["top_seqno"])}
            )
    return [grouped[key] for key in sorted(grouped)]


def aggregate_blocks(rows: list[dict[str, Any]], window_s: float) -> dict[str, Any]:
    transactions = sum_field(rows, "transactions")
    tx1 = sum_field(rows, "tx1")
    tx3 = sum_field(rows, "tx3_success")
    return {
        "blocks": len(rows),
        "transactions": transactions,
        "tx1": tx1,
        "tx3_success": tx3,
        "raw_tps": transactions / window_s if window_s > 0 else 0.0,
        "tx1_tps": tx1 / window_s if window_s > 0 else 0.0,
        "tx3_jtps": tx3 / window_s if window_s > 0 else 0.0,
    }


def quantile(sorted_values: list[float], fraction: float) -> float:
    if not sorted_values:
        return 0.0
    position = fraction * (len(sorted_values) - 1)
    low = int(position)
    high = min(low + 1, len(sorted_values) - 1)
    return sorted_values[low] + (sorted_values[high] - sorted_values[low]) * (position - low)


def distribution(values: Iterable[float]) -> dict[str, float | int]:
    ordered = sorted(float(value) for value in values)
    if not ordered:
        return {"count": 0, "sum": 0.0, "mean": 0.0, "p50": 0.0, "p95": 0.0, "p99": 0.0, "max": 0.0}
    return {
        "count": len(ordered),
        "sum": sum(ordered),
        "mean": statistics.fmean(ordered),
        "p50": quantile(ordered, 0.50),
        "p95": quantile(ordered, 0.95),
        "p99": quantile(ordered, 0.99),
        "max": ordered[-1],
    }


def sum_field(rows: Iterable[dict[str, Any]], field: str) -> int:
    return sum(int(row.get(field, 0)) for row in rows)


def aggregate_trace_subset(rows: list[dict[str, Any]]) -> dict[str, Any]:
    completed = [
        row
        for row in rows
        if row.get("status") == "complete"
        and all(row.get(f"tx{stage}_block") is not None for stage in range(1, 5))
    ]
    status_counts = dict(sorted(Counter(str(row.get("status", "")) for row in rows).items()))
    leg_fields = {
        "tx1_to_tx2": ("tx1_block", "tx2_block"),
        "tx2_to_tx3": ("tx2_block", "tx3_block"),
        "tx3_to_tx4": ("tx3_block", "tx4_block"),
    }
    block_gaps = {
        name: distribution(
            int(row[end]) - int(row[start])
            for row in completed
            if row[start.replace("_block", "_shard")] == row[end.replace("_block", "_shard")]
        )
        for name, (start, end) in leg_fields.items()
    }
    cross_shard_legs = {
        name: sum(
            row[start.replace("_block", "_shard")] != row[end.replace("_block", "_shard")]
            for row in completed
        )
        for name, (start, end) in leg_fields.items()
    }
    same_block_legs = {
        name: (
            sum(
                row[start.replace("_block", "_shard")] == row[end.replace("_block", "_shard")]
                and int(row[start]) == int(row[end])
                for row in completed
            )
            / len(completed)
            if completed
            else 0.0
        )
        for name, (start, end) in leg_fields.items()
    }
    latency_fields = {
        "external_to_tx1": "external_to_tx1_ms",
        "tx1_to_tx2": "tx1_to_tx2_ms",
        "tx2_to_tx3": "tx2_to_tx3_ms",
        "tx3_to_tx4": "tx3_to_tx4_ms",
        "external_to_tx3": "external_to_tx3_ms",
        "external_to_tx4": "external_to_tx4_ms",
    }
    latencies = {
        name: distribution(row[field] for row in completed if row.get(field) is not None)
        for name, field in latency_fields.items()
    }
    causal_stages = ("external_to_tx1", "tx1_to_tx2", "tx2_to_tx3", "tx3_to_tx4")
    slowest_mean_stage = max(causal_stages, key=lambda name: float(latencies[name]["mean"])) if completed else None
    slowest_p50_stage = max(causal_stages, key=lambda name: float(latencies[name]["p50"])) if completed else None
    def placement(row: dict[str, Any], stage: int) -> tuple[str, int]:
        return str(row[f"tx{stage}_shard"]), int(row[f"tx{stage}_block"])

    same_block_tx1_tx3 = sum(len({placement(row, stage) for stage in range(1, 4)}) == 1 for row in completed)
    same_block_tx1_tx4 = sum(len({placement(row, stage) for stage in range(1, 5)}) == 1 for row in completed)
    one_shard_tx1_tx4 = sum(len({row[f"tx{stage}_shard"] for stage in range(1, 5)}) == 1 for row in completed)
    return {
        "tracked": len(rows),
        "completed": len(completed),
        "completion_fraction": len(completed) / len(rows) if rows else 0.0,
        "status_counts": status_counts,
        "same_block_tx1_tx3": same_block_tx1_tx3,
        "same_block_tx1_tx3_fraction": same_block_tx1_tx3 / len(completed) if completed else 0.0,
        "same_block_tx1_tx4": same_block_tx1_tx4,
        "same_block_tx1_tx4_fraction": same_block_tx1_tx4 / len(completed) if completed else 0.0,
        "one_shard_tx1_tx4": one_shard_tx1_tx4,
        "one_shard_tx1_tx4_fraction": one_shard_tx1_tx4 / len(completed) if completed else 0.0,
        "distinct_blocks": distribution(len({placement(row, stage) for stage in range(1, 5)}) for row in completed),
        "tx1_to_tx4_block_span": distribution(
            int(row["tx4_block"]) - int(row["tx1_block"])
            for row in completed
            if row["tx1_shard"] == row["tx4_shard"]
        ),
        "block_gaps": block_gaps,
        "cross_shard_leg_counts": cross_shard_legs,
        "cross_shard_leg_fraction": {
            name: count / len(completed) if completed else 0.0 for name, count in cross_shard_legs.items()
        },
        "same_block_leg_fraction": same_block_legs,
        "latency_ms": latencies,
        "slowest_mean_stage": slowest_mean_stage,
        "slowest_p50_stage": slowest_p50_stage,
    }


def aggregate_traces(rows: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "available": bool(rows),
        "all": aggregate_trace_subset(rows),
        "steady_offers": aggregate_trace_subset([row for row in rows if row.get("steady_offer") == 1]),
    }


def join_collations(
    blocks: list[dict[str, Any]], collations: list[dict[str, Any]]
) -> tuple[list[tuple[dict[str, Any], dict[str, Any]]], int, int]:
    exact: dict[tuple[str, int, str], list[dict[str, Any]]] = defaultdict(list)
    by_seqno: dict[tuple[str, int], list[dict[str, Any]]] = defaultdict(list)
    for record in collations:
        if int(record.get("workchain", -999)) != 0:
            continue
        seqno = int(record["seqno"])
        shard = str(record.get("shard", "0x8000000000000000")).lower()
        root_hash = str(record.get("root_hash", "")).upper()
        if root_hash:
            exact[(shard, seqno, root_hash)].append(record)
        by_seqno[(shard, seqno)].append(record)

    joined = []
    missing = 0
    ambiguous = 0
    for block in blocks:
        root_hash = str(block.get("root_hash", "")).upper()
        shard = str(block.get("shard", "0x8000000000000000")).lower()
        matches = (
            exact.get((shard, block["seqno"], root_hash), [])
            if root_hash
            else by_seqno.get((shard, block["seqno"]), [])
        )
        if not matches:
            missing += 1
        elif len(matches) > 1:
            ambiguous += 1
        else:
            joined.append((block, matches[0]))
    return joined, missing, ambiguous


def aggregate_collation(records: list[dict[str, Any]], window_s: float, completed: int) -> dict[str, Any]:
    real = [record.get("work_real_s", {}) for record in records]
    cpu = [record.get("work_cpu_s", {}) for record in records]
    total_real = distribution(item.get("total", 0.0) for item in real)
    total_cpu = distribution(item.get("total", 0.0) for item in cpu)

    has_top_level_stages = any("inbound_external" in item for item in cpu)
    phase_keys = TOP_LEVEL_PHASE_KEYS if has_top_level_stages else LEGACY_PHASE_KEYS
    phase_cpu_totals = {key: sum(float(item.get(key, 0.0)) for item in cpu) for key in phase_keys}
    measured_phase_cpu = sum(phase_cpu_totals.values())
    phase_cpu_totals["unattributed"] = max(float(total_cpu["sum"]) - measured_phase_cpu, 0.0)
    denominator = float(total_cpu["sum"])
    phase_cpu_share = {
        key: (value / denominator if denominator > 0 else 0.0) for key, value in phase_cpu_totals.items()
    }
    load_fractions = {
        name: distribution(record.get(field, -1.0) for record in records)
        for name, field in LOAD_FRACTION_FIELDS.items()
    }
    hottest_load_axis = max(load_fractions, key=lambda name: float(load_fractions[name]["mean"])) if records else None
    limit_categories = {
        name: max((int(record.get(field, 0)) for record in records), default=0)
        for name, field in LIMIT_CATEGORY_FIELDS.items()
    }
    binding_limit_axes = [name for name, category in limit_categories.items() if category >= 2]
    max_limit_class = max((int(record.get("peak_limit_class", 0)) for record in records), default=0)
    overload_reason_counts = {
        name: sum(int(record.get("overload_reason", 0)) == reason for record in records)
        for reason, name in OVERLOAD_REASON_NAMES.items()
    }
    long_collation_overload = sum(
        float(record.get("check_load_total_s", -1.0)) > 0.1
        and float(record.get("wait_externals_s", -1.0))
        < float(record.get("check_load_total_s", -1.0)) * 0.2
        and float(record.get("check_load_do_collate_s", -1.0))
        > float(record.get("check_load_total_s", -1.0)) * 0.6
        for record in records
    )
    long_collation_underload_blocked = sum(
        float(record.get("check_load_total_s", -1.0)) > 0.1
        and float(record.get("wait_externals_s", -1.0))
        < float(record.get("check_load_total_s", -1.0)) * 0.7
        and float(record.get("check_load_do_collate_s", -1.0))
        > float(record.get("check_load_total_s", -1.0)) * 0.6
        for record in records
    )
    overloaded = sum(overload_reason_counts.values())
    cpu_core_fraction = float(total_cpu["sum"]) / window_s if window_s > 0 else 0.0
    active_overload_reasons = [name for name, count in overload_reason_counts.items() if count]
    if len(active_overload_reasons) > 1:
        bottleneck_class = "mixed_overload"
    elif active_overload_reasons:
        bottleneck_class = active_overload_reasons[0]
    elif cpu_core_fraction >= 0.9:
        bottleneck_class = "collator_cpu"
    else:
        bottleneck_class = "not_saturated"

    old_first = int(records[0].get("old_out_queue", 0)) if records else 0
    new_last = int(records[-1].get("new_out_queue", 0)) if records else 0

    return {
        "candidate_count": len(records),
        "total_real_s": total_real,
        "total_cpu_s": total_cpu,
        "busy_wall_fraction": float(total_real["sum"]) / window_s if window_s > 0 else 0.0,
        "cpu_core_fraction": cpu_core_fraction,
        "cpu_s_per_completed_jetton": float(total_cpu["sum"]) / completed if completed > 0 else 0.0,
        "phase_cpu_s": phase_cpu_totals,
        "phase_cpu_share": phase_cpu_share,
        "phase_model": "top_level" if has_top_level_stages else "legacy_partial_overlapping",
        "transaction_cpu_s": {
            "tvm": sum(float(item.get("trx_tvm", 0.0)) for item in cpu),
            "storage": sum(float(item.get("trx_storage", 0.0)) for item in cpu),
            "other": sum(float(item.get("trx_other", 0.0)) for item in cpu),
            "prelim_storage": sum(float(item.get("prelim_storage", 0.0)) for item in cpu),
        },
        "input_cpu_s": {
            "external": sum(float(item.get("trx_external", 0.0)) for item in cpu),
            "internal": sum(float(item.get("trx_internal", 0.0)) for item in cpu),
        },
        "account_dict_estimator": {
            "update_cpu_s": sum(float(item.get("account_dict_estimator_update", 0.0)) for item in cpu),
            "proof_cpu_s": sum(float(item.get("account_dict_estimator_proof", 0.0)) for item in cpu),
            "wait_real_s": sum(float(item.get("account_dict_estimator_wait", 0.0)) for item in real),
            "updates": sum_field(records, "account_dict_estimator_updates"),
            "proofs": sum_field(records, "account_dict_estimator_proofs"),
            "batches": sum_field(records, "account_dict_estimator_batches"),
            "async_batches": sum_field(records, "account_dict_estimator_async_batches"),
            "async_queue_max": max(
                (int(record.get("account_dict_estimator_async_queue_max", 0)) for record in records), default=0
            ),
            "estimated_bytes": sum_field(records, "account_dict_estimator_estimated_bytes"),
            "proof_cells": sum_field(records, "account_dict_estimator_proof_cells"),
            "proof_bits": sum_field(records, "account_dict_estimator_proof_bits"),
            "proof_internal_refs": sum_field(records, "account_dict_estimator_proof_internal_refs"),
            "proof_external_refs": sum_field(records, "account_dict_estimator_proof_external_refs"),
            "reused": sum_field(records, "account_dict_estimator_reused"),
            "corrections": sum_field(records, "account_dict_estimator_corrections"),
        },
        "transactions": {
            "all": sum_field(records, "transactions"),
            "ordinary_external": sum_field(records, "ordinary_external"),
            "ordinary_internal": sum_field(records, "ordinary_internal"),
            "mean_per_candidate": statistics.fmean(float(record.get("transactions", 0)) for record in records)
            if records
            else 0.0,
        },
        "messages": {
            "generated": sum_field(records, "new_msgs_generated"),
            "immediate": sum_field(records, "new_msgs_immediate"),
            "enqueued": sum_field(records, "new_msgs_enqueued"),
            "deferred": sum_field(records, "new_msgs_deferred"),
            "external": sum_field(records, "new_msgs_external"),
            "peak_in_memory": max((int(record.get("peak_new_msgs", 0)) for record in records), default=0),
            "external_total": sum_field(records, "ext_msgs_total"),
            "external_accepted": sum_field(records, "ext_msgs_accepted"),
            "external_rejected": sum_field(records, "ext_msgs_rejected"),
        },
        "candidate": {
            "actual_bytes": distribution(record.get("actual_bytes", 0) for record in records),
            "actual_collated_data_bytes": distribution(
                record.get("actual_collated_data_bytes", 0) for record in records
            ),
            "estimated_bytes": distribution(record.get("estimated_bytes", 0) for record in records),
            "estimated_collated_data_bytes": distribution(
                record.get("estimated_collated_data_bytes", 0) for record in records
            ),
            "gas": distribution(record.get("gas", 0) for record in records),
            "lt_delta": distribution(record.get("lt_delta", 0) for record in records),
            "wait_externals_s": distribution(record.get("wait_externals_s", 0.0) for record in records),
        },
        "queues": {
            "old_first": old_first,
            "new_last": new_last,
            "new_max": max((int(record.get("new_out_queue", 0)) for record in records), default=0),
            "cleaned": sum_field(records, "msg_queue_cleaned"),
            "delta": new_last - old_first,
            "growth_per_s": (new_last - old_first) / window_s if window_s > 0 else 0.0,
        },
        "limits": {
            "max_class": max_limit_class,
            "max_class_name": LIMIT_CLASS_NAMES.get(max_limit_class, "unknown"),
            "overloaded_candidates": overloaded,
            "overload_reason_counts": overload_reason_counts,
            "long_collation_overload_condition_candidates": long_collation_overload,
            "long_collation_underload_blocked_condition_candidates": long_collation_underload_blocked,
            "want_split_candidates": sum(bool(record.get("want_split", False)) for record in records),
            "load_fraction": load_fractions,
            "hottest_load_axis": hottest_load_axis,
            "limit_category_max": limit_categories,
            "binding_limit_axes": binding_limit_axes,
        },
        "bottleneck_class": bottleneck_class,
    }


def analyze(run_dir: Path) -> dict[str, Any]:
    run_dir = run_dir.resolve()
    manifest = json.loads((run_dir / "run.json").read_text())
    results = json.loads((run_dir / "results.json").read_text())
    blocks = load_blocks(run_dir / "blocks.csv")
    collations = load_collations(run_dir / "collation.jsonl")
    traces = load_traces(run_dir / "traces.csv")
    topology = load_topology(run_dir / "topology.csv")

    if blocks and "steady" in blocks[0]:
        steady_blocks = [block for block in blocks if block["steady"]]
    else:
        start = int(results.get("measurement_start_unix_ms", 0))
        end = int(results.get("measurement_end_unix_ms", 2**63 - 1))
        steady_blocks = [block for block in blocks if start < block["observed_at_unix_ms"] <= end]

    joined_all, missing_all, ambiguous_all = join_collations(blocks, collations)
    steady_ids = {
        (block.get("shard", "0x8000000000000000"), block["seqno"], block.get("root_hash", ""))
        for block in steady_blocks
    }
    joined = [
        pair
        for pair in joined_all
        if (pair[0].get("shard", "0x8000000000000000"), pair[0]["seqno"], pair[0].get("root_hash", ""))
        in steady_ids
    ]
    joined_records = [record for _, record in joined]
    measurement_end_ms = int(results.get("measurement_end_unix_ms", 2**63 - 1))
    post_window = [record for block, record in joined_all if block["observed_at_unix_ms"] > measurement_end_ms]
    steady = results["steady"]
    totals = results.get("totals", {})
    window_s = float(results.get("measurement_window_s", 0.0))
    stage_totals = [
        int(totals.get("tx1", results.get("tx1_total", 0))),
        int(totals.get("tx2_success", 0)),
        int(totals.get("tx3_success", 0)),
        int(totals.get("tx4_success", 0)),
    ]
    steady_by_shard: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for block in steady_blocks:
        steady_by_shard[str(block.get("shard", "0x8000000000000000"))].append(block)
    collations_by_shard: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for record in joined_records:
        collations_by_shard[str(record.get("shard", "0x8000000000000000")).lower()].append(record)

    first_want_split = next(
        (
            {"block": block, "collation": record}
            for block, record in joined_all
            if bool(block.get("want_split", False)) or bool(record.get("want_split", False))
        ),
        None,
    )
    first_before_split = next((block for block in blocks if bool(block.get("before_split", False))), None)
    first_after_split = next((block for block in blocks if bool(block.get("after_split", False))), None)
    first_after_merge = next((block for block in blocks if bool(block.get("after_merge", False))), None)

    def shard_count_at(timestamp_ms: int) -> int:
        count = topology[0]["shard_count"] if topology else int(results.get("initial_shards", 0))
        for event in topology:
            if event["observed_at_unix_ms"] > timestamp_ms:
                break
            count = event["shard_count"]
        return count

    phase_reports = []
    phase_start_ms = int(
        results.get("measurement_start_unix_ms", 0) - float(results.get("warmup_s", 0.0)) * 1000
    )
    for phase in results.get("rate_schedule", []):
        duration_s = float(phase["duration_s"])
        phase_end_ms = phase_start_ms + int(duration_s * 1000)
        phase_blocks = [
            block for block in blocks if phase_start_ms < block["observed_at_unix_ms"] <= phase_end_ms
        ]
        phase_ids = {
            (block.get("shard", "0x8000000000000000"), block["seqno"], block.get("root_hash", ""))
            for block in phase_blocks
        }
        phase_records = [
            record
            for block, record in joined_all
            if (block.get("shard", "0x8000000000000000"), block["seqno"], block.get("root_hash", ""))
            in phase_ids
        ]
        phase_traces = [
            row
            for row in traces
            if row.get("offered_at_unix_ms") is not None
            and phase_start_ms < int(row["offered_at_unix_ms"]) <= phase_end_ms
        ]
        phase_throughput = aggregate_blocks(phase_blocks, duration_s)
        phase_reports.append(
            {
                "phase": int(phase["phase"]),
                "target_rate": float(phase["rate"]),
                "duration_s": duration_s,
                "start_unix_ms": phase_start_ms,
                "end_unix_ms": phase_end_ms,
                "expected_offers": float(phase["rate"]) * duration_s,
                "start_shards": shard_count_at(phase_start_ms),
                "end_shards": shard_count_at(phase_end_ms),
                "throughput": phase_throughput,
                "collation": aggregate_collation(
                    phase_records, duration_s, int(phase_throughput["tx3_success"])
                ),
                "pipeline": aggregate_trace_subset(phase_traces),
            }
        )
        phase_start_ms = phase_end_ms

    return {
        "version": 4,
        "run_dir": str(run_dir),
        "run_status": manifest.get("status"),
        "workload": manifest.get("workload", {}),
        "topology": manifest.get("topology", {}),
        "integrity": {
            "valid_stage_counts": bool(results.get("valid_stage_counts")),
            "blocks_skipped": int(results.get("blocks_skipped", 0)),
            "parse_failures": int(results.get("parse_failures", 0)),
            "steady_blocks": len(steady_blocks),
            "exact_collation_joins": len(joined),
            "accepted_blocks_observed": len(blocks),
            "all_exact_collation_joins": len(joined_all),
            "missing_collation_joins": missing_all,
            "ambiguous_collation_joins": ambiguous_all,
            "acknowledged_equals_tx1_total": int(results.get("acknowledged", 0))
            == int(results.get("tx1_total", 0)),
            "completed_stage_totals_equal": all(value > 0 for value in stage_totals) and len(set(stage_totals)) == 1,
        },
        "throughput": {
            "measurement_window_s": window_s,
            "offered": int(results.get("offered", 0)),
            "acknowledged": int(results.get("acknowledged", 0)),
            "send_errors": int(results.get("send_errors", 0)),
            "unmatched_externals": int(results.get("unmatched_externals", 0)),
            "totals": totals,
            "steady": steady,
            "inclusion_latency_ms": results.get("inclusion_latency_ms", {}),
            "steady_by_shard": {
                shard: aggregate_blocks(shard_blocks, window_s)
                for shard, shard_blocks in sorted(steady_by_shard.items())
            },
        },
        "collation": aggregate_collation(joined_records, window_s, int(steady.get("tx3_success", 0))),
        "collation_by_shard": {
            shard: aggregate_collation(
                records,
                window_s,
                sum(int(block.get("tx3_success", 0)) for block in steady_by_shard.get(shard, [])),
            )
            for shard, records in sorted(collations_by_shard.items())
        },
        "pipeline": aggregate_traces(traces),
        "rate_phases": phase_reports,
        "sharding": {
            "topology_events": topology,
            "initial_shard_count": topology[0]["shard_count"] if topology else results.get("initial_shards"),
            "final_shard_count": topology[-1]["shard_count"] if topology else results.get("final_shards"),
            "maximum_shard_count": max((event["shard_count"] for event in topology), default=0),
            "first_want_split": first_want_split,
            "first_before_split": first_before_split,
            "first_after_split": first_after_split,
            "first_after_merge": first_after_merge,
        },
        "drain": {
            "accepted_candidates_after_window": len(post_window),
            "final_out_queue": int(post_window[-1].get("new_out_queue", 0)) if post_window else None,
            "max_out_queue_after_window": max(
                (int(record.get("new_out_queue", 0)) for record in post_window), default=0
            ),
            "out_queue_drained": bool(post_window) and int(post_window[-1].get("new_out_queue", 0)) == 0,
        },
    }


def render_text(report: dict[str, Any]) -> str:
    integrity = report["integrity"]
    throughput = report["throughput"]
    steady = throughput["steady"]
    collation = report["collation"]
    total_cpu = collation["total_cpu_s"]
    total_real = collation["total_real_s"]
    phases = sorted(collation["phase_cpu_share"].items(), key=lambda item: item[1], reverse=True)
    phase_text = ", ".join(f"{name}={share * 100:.1f}%" for name, share in phases[:5])
    queues = collation["queues"]
    limits = collation["limits"]
    drain = report["drain"]
    pipeline = report["pipeline"]
    hottest_axis = limits["hottest_load_axis"]
    hottest_mean = limits["load_fraction"][hottest_axis]["mean"] if hottest_axis else 0.0
    binding_axes = ",".join(limits["binding_limit_axes"]) or "none"
    category_text = ",".join(
        f"{name}:{category}" for name, category in limits["limit_category_max"].items()
    )
    overload_reason_text = ",".join(
        f"{name}:{count}" for name, count in limits["overload_reason_counts"].items() if count
    ) or "none"
    lines = [
            f"Run: {report['run_dir']}",
            (
                "Integrity: "
                f"parser={'valid' if integrity['valid_stage_counts'] else 'INVALID'}, "
                f"collation joins={integrity['exact_collation_joins']}/{integrity['steady_blocks']} "
                f"steady and {integrity['all_exact_collation_joins']}/{integrity['accepted_blocks_observed']} all "
                f"(missing={integrity['missing_collation_joins']}, ambiguous={integrity['ambiguous_collation_joins']})"
            ),
            (
                "Load: "
                f"offered={throughput['offered']}, ack={throughput['acknowledged']}, "
                f"errors={throughput['send_errors']}, unmatched={throughput['unmatched_externals']}"
            ),
            (
                "Steady: "
                f"TX1/TX2/TX3/TX4={steady['tx1']}/{steady['tx2_success']}/{steady['tx3_success']}/{steady['tx4_success']}, "
                f"jTPS={steady['tx3_jtps']:.3f}, raw TPS={steady['raw_tps']:.3f}"
            ),
            (
                "Collation: "
                f"candidates={collation['candidate_count']}, mean real={total_real['mean'] * 1e3:.3f} ms, "
                f"p95 real={total_real['p95'] * 1e3:.3f} ms, mean CPU={total_cpu['mean'] * 1e3:.3f} ms, "
                f"CPU cores={collation['cpu_core_fraction']:.3f}"
            ),
            f"CPU phases ({collation['phase_model']}): {phase_text}",
            (
                "Queues/limits: "
                f"old-first={queues['old_first']}, new-last={queues['new_last']}, new-max={queues['new_max']}, "
                f"growth={queues['growth_per_s']:.1f}/s, limit-class={limits['max_class_name']}, "
                f"overloaded={limits['overloaded_candidates']}, hottest={hottest_axis}@{hottest_mean:.3f}, "
                f"binding={binding_axes}, axis-categories={category_text}, reasons={overload_reason_text}, "
                f"long-time-condition={limits['long_collation_overload_condition_candidates']}"
            ),
            (
                "Drain: "
                f"candidates={drain['accepted_candidates_after_window']}, final queue={drain['final_out_queue']}, "
                f"max queue={drain['max_out_queue_after_window']}, drained={drain['out_queue_drained']}"
            ),
            f"Bottleneck class: {collation['bottleneck_class']}",
        ]
    if pipeline["available"]:
        traced = pipeline["steady_offers"]
        latency = traced["latency_ms"]
        lines.insert(
            -1,
            (
                "Pipeline: "
                f"steady traces={traced['completed']}/{traced['tracked']}, "
                f"same-block TX1-TX4={traced['same_block_tx1_tx4_fraction'] * 100:.1f}%, "
                f"p50 block span={traced['tx1_to_tx4_block_span']['p50']:.1f}, "
                f"p50 ms ext/TX1/TX2/TX3/TX4={latency['external_to_tx1']['p50']:.1f}/"
                f"{latency['tx1_to_tx2']['p50']:.1f}/{latency['tx2_to_tx3']['p50']:.1f}/"
                f"{latency['tx3_to_tx4']['p50']:.1f}, slowest-mean={traced['slowest_mean_stage']}"
            ),
        )
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    report = analyze(args.run_dir)
    output = args.json_out or args.run_dir / "analysis.json"
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(render_text(report))
    print(f"Analysis JSON: {output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
