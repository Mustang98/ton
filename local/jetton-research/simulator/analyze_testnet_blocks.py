#!/usr/bin/env python3

import argparse
import base64
import csv
import gzip
import json
import math
import re
import statistics
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


ROOT_SHARD_SIGNED = -(1 << 63)
ROOT_SHARD_HEX = "8000000000000000"


def parse_args():
    parser = argparse.ArgumentParser(
        description="Build exact size/transaction distributions for accepted testnet blocks"
    )
    parser.add_argument("--session-dir", required=True, type=Path)
    parser.add_argument("--lite-client", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--start-seqno", required=True, type=int)
    parser.add_argument("--end-seqno", required=True, type=int)
    parser.add_argument("--liteserver-index", type=int, default=0)
    parser.add_argument("--workers", type=int, default=4)
    return parser.parse_args()


def in_window(block, start_seqno, end_seqno):
    return (
        block.get("workchain") == 0
        and int(block.get("shard", 0)) == ROOT_SHARD_SIGNED
        and start_seqno <= block.get("seqno", -1) <= end_seqno
    )


def read_session_stats(session_dir, start_seqno, end_seqno):
    candidates = {}
    accepted_candidates = set()
    validated_sizes = {}
    collated_stats = {}

    for path in sorted(session_dir.glob("*.log.gz")):
        with gzip.open(path, "rt", encoding="utf-8") as stream:
            for line in stream:
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    continue

                record_type = record.get("@type")
                if record_type == "consensus.stats.events":
                    for timestamped in record.get("events", []):
                        event = timestamped.get("event", {})
                        event_type = event.get("@type")
                        candidate = event.get("id", {}).get("hash")
                        if event_type == "consensus.stats.candidateReceived":
                            block = event.get("block", {}).get("id")
                            if candidate and block:
                                candidates[candidate] = block
                        elif event_type == "consensus.stats.blockAccepted" and candidate:
                            accepted_candidates.add(candidate)
                    continue

                block = record.get("block_id", {})
                if not in_window(block, start_seqno, end_seqno):
                    continue
                seqno = block["seqno"]
                if record_type == "validatorStats.validatedBlock":
                    old = validated_sizes.setdefault(seqno, record["bytes"])
                    if old != record["bytes"]:
                        raise RuntimeError(f"inconsistent validated size for {seqno}: {old} vs {record['bytes']}")
                elif record_type == "validatorStats.collatedBlock":
                    collated_stats[seqno] = record

    accepted_blocks = {}
    for candidate in accepted_candidates:
        block = candidates.get(candidate)
        if block and in_window(block, start_seqno, end_seqno):
            seqno = block["seqno"]
            old = accepted_blocks.setdefault(seqno, block)
            if old["root_hash"] != block["root_hash"] or old["file_hash"] != block["file_hash"]:
                raise RuntimeError(f"multiple accepted blocks for seqno {seqno}")

    expected = set(range(start_seqno, end_seqno + 1))
    missing = sorted(expected - set(accepted_blocks))
    if missing:
        raise RuntimeError(f"accepted block IDs missing for seqnos: {missing}")
    return accepted_blocks, validated_sizes, collated_stats


def block_id_text(block):
    root_hash = base64.b64decode(block["root_hash"]).hex().upper()
    file_hash = base64.b64decode(block["file_hash"]).hex().upper()
    return f"(0,{ROOT_SHARD_HEX},{block['seqno']}):{root_hash}:{file_hash}"


def query_block(args, block):
    block_id = block_id_text(block)
    base_command = [
        str(args.lite_client),
        "-C",
        str(args.config),
        "-i",
        str(args.liteserver_index),
        "-t",
        "20",
        "-v",
        "3",
    ]
    command = base_command + [
        "-c",
        f"getblock {block_id}",
        "-c",
        f"listblocktrans {block_id} 1024",
    ]

    size = None
    page = None
    complete = False
    last_error = ""
    for attempt in range(3):
        completed = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output = completed.stdout
        size_match = re.search(
            rf"obtained (\d+) data bytes for block \(0,{ROOT_SHARD_HEX},{block['seqno']}\)", output
        )
        page = re.findall(
            r"transaction #(\d+): account ([0-9A-F]+) lt (\d+) hash [0-9A-F]+", output
        )
        complete = "(end of block transaction list)" in output
        incomplete = "(block transaction list incomplete)" in output
        if completed.returncode == 0 and size_match and (complete or incomplete):
            size = int(size_match.group(1))
            break
        last_error = output[-2000:]
        time.sleep(0.25 * (attempt + 1))
    if size is None or page is None:
        raise RuntimeError(f"liteserver query failed for {block['seqno']}:\n{last_error}")

    transaction_count = 0
    for page_index in range(10):
        page_numbers = [int(item[0]) for item in page]
        if page_numbers and page_numbers != list(range(1, len(page_numbers) + 1)):
            raise RuntimeError(f"non-contiguous transaction page for {block['seqno']}")
        transaction_count += len(page)
        if complete:
            return {"seqno": block["seqno"], "bytes": size, "transactions": transaction_count}
        if not page:
            raise RuntimeError(f"empty incomplete transaction page for {block['seqno']}")

        _, account, logical_time = page[-1]
        page_command = base_command + [
            "-c",
            f"listblocktrans {block_id} 1024 {account} {logical_time}",
        ]
        for attempt in range(3):
            completed = subprocess.run(
                page_command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
            )
            output = completed.stdout
            page = re.findall(
                r"transaction #(\d+): account ([0-9A-F]+) lt (\d+) hash [0-9A-F]+", output
            )
            complete = "(end of block transaction list)" in output
            incomplete = "(block transaction list incomplete)" in output
            if completed.returncode == 0 and (complete or incomplete):
                break
            last_error = output[-2000:]
            time.sleep(0.25 * (attempt + 1))
        else:
            raise RuntimeError(
                f"liteserver pagination failed for {block['seqno']} page {page_index + 2}:\n{last_error}"
            )

    raise RuntimeError(f"too many transaction pages for {block['seqno']}")


def percentile(values, percent):
    ordered = sorted(values)
    position = (len(ordered) - 1) * percent / 100
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def distribution(values):
    return {
        "count": len(values),
        "min": min(values),
        "p10": percentile(values, 10),
        "p25": percentile(values, 25),
        "p50": percentile(values, 50),
        "p75": percentile(values, 75),
        "p90": percentile(values, 90),
        "p95": percentile(values, 95),
        "p99": percentile(values, 99),
        "max": max(values),
        "mean": statistics.fmean(values),
        "stdev": statistics.pstdev(values),
        "sum": sum(values),
    }


def histogram(values, width):
    buckets = {}
    for value in values:
        lower = value // width * width
        buckets[lower] = buckets.get(lower, 0) + 1
    return [
        {"lower_inclusive": lower, "upper_exclusive": lower + width, "count": buckets[lower]}
        for lower in sorted(buckets)
    ]


def correlation(xs, ys):
    mean_x = statistics.fmean(xs)
    mean_y = statistics.fmean(ys)
    numerator = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys))
    denominator = math.sqrt(
        sum((x - mean_x) ** 2 for x in xs) * sum((y - mean_y) ** 2 for y in ys)
    )
    return numerator / denominator


def main():
    args = parse_args()
    accepted, validated_sizes, collated_stats = read_session_stats(
        args.session_dir, args.start_seqno, args.end_seqno
    )

    queried = {}
    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = {executor.submit(query_block, args, block): seqno for seqno, block in accepted.items()}
        for index, future in enumerate(as_completed(futures), 1):
            result = future.result()
            queried[result["seqno"]] = result
            if index % 25 == 0 or index == len(futures):
                print(f"queried {index}/{len(futures)} blocks", flush=True)

    for seqno, size in validated_sizes.items():
        if queried[seqno]["bytes"] != size:
            raise RuntimeError(
                f"size mismatch for {seqno}: session-stats={size}, liteserver={queried[seqno]['bytes']}"
            )
    for seqno, record in collated_stats.items():
        expected = record["block_stats"]["transactions"]
        if queried[seqno]["transactions"] != expected:
            raise RuntimeError(
                f"transaction mismatch for {seqno}: session-stats={expected}, liteserver={queried[seqno]['transactions']}"
            )

    rows = []
    for seqno in sorted(accepted):
        block = accepted[seqno]
        result = queried[seqno]
        rows.append(
            {
                "seqno": seqno,
                "root_hash": block["root_hash"],
                "file_hash": block["file_hash"],
                "bytes": result["bytes"],
                "kib": result["bytes"] / 1024,
                "transactions": result["transactions"],
                "bytes_per_transaction": (
                    result["bytes"] / result["transactions"] if result["transactions"] else None
                ),
            }
        )

    sizes = [row["bytes"] for row in rows]
    transactions = [row["transactions"] for row in rows]
    summary = {
        "seqno_start": args.start_seqno,
        "seqno_end": args.end_seqno,
        "block_count": len(rows),
        "definition": {
            "bytes": "serialized block BOC byte length returned by lite-client getblock",
            "transactions": "complete transaction count returned by lite-client listblocktrans",
        },
        "verification": {
            "sizes_matched_session_stats": len(validated_sizes),
            "transactions_matched_collator_stats": len(collated_stats),
        },
        "bytes": distribution(sizes),
        "kib": distribution([value / 1024 for value in sizes]),
        "transactions": distribution(transactions),
        "histograms": {
            "bytes_100kib": histogram(sizes, 100 * 1024),
            "transactions_50": histogram(transactions, 50),
        },
        "pearson_size_transactions": correlation(sizes, transactions),
    }

    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (args.output_dir / "blocks.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    with (args.output_dir / "distribution.json").open("w", encoding="utf-8") as stream:
        json.dump(summary, stream, indent=2)
        stream.write("\n")

    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
