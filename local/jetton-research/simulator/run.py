#!/usr/bin/env python3
"""Run the jetton-spam contract workload on a local collation network."""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import logging
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from pytoniq_core import (
    Address,
    Cell,
    CurrencyCollection,
    InternalMsgInfo,
    MessageAny,
    WalletMessage,
)
from tonlib.tonlibjson import TonlibError
from tontester.install import Install
from tontester.network import Network, StartOptions
from tontester.zerostate import ExternalBasechainState, SimplexConsensusConfig


FULL_SHARD = -(2**63)
LOG = logging.getLogger("jetton-simulator")


def parse_args() -> argparse.Namespace:
    source_root = Path(__file__).resolve().parents[3]
    timestamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    parser = argparse.ArgumentParser(
        description="One-validator collation oracle for the jetton-spam contract workload"
    )
    parser.add_argument("--source-root", type=Path, default=source_root)
    parser.add_argument("--build-dir", type=Path, default=source_root / "build")
    parser.add_argument(
        "--state-manifest",
        type=Path,
        help="bench-state-gen manifest for an externally generated workchain-0 zerostate",
    )
    parser.add_argument(
        "--workdir",
        type=Path,
        default=Path(__file__).resolve().parent / "runs" / timestamp,
    )
    parser.add_argument("--pool-size", type=int, default=10_000)
    parser.add_argument(
        "--init-mode",
        type=int,
        choices=(0, 1),
        default=0,
        help="recipient JettonWallet mode: 0 full StateInit, 1 bare address",
    )
    parser.add_argument("--rate", type=float, default=1_000.0)
    parser.add_argument(
        "--rate-schedule",
        help="comma-separated rate:seconds phases; overrides --rate and --duration",
    )
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--warmup", type=float, default=5.0)
    parser.add_argument("--drain", type=float, default=15.0)
    parser.add_argument("--track-sample", type=float, default=0.01)
    parser.add_argument("--rng-seed", type=int, default=1)
    parser.add_argument("--max-inflight", type=int, default=0)
    parser.add_argument("--presign", type=int, default=0)
    parser.add_argument("--signer-threads", type=int, default=0)
    parser.add_argument("--connections", type=int, default=1)
    parser.add_argument("--block-period-ms", type=int, default=400)
    parser.add_argument("--min-split", type=int, default=0)
    parser.add_argument("--max-split", type=int, default=0)
    parser.add_argument("--block-limit-mul", type=int, default=1)
    parser.add_argument("--gas-limit-mul", type=int, default=1)
    parser.add_argument(
        "--force-empty-first-slot",
        action="store_true",
        help="force slot 0 of every four-slot shard leader window to collate an empty block",
    )
    parser.add_argument("--node-threads", type=int, default=8)
    parser.add_argument(
        "--base-port",
        type=int,
        default=2_000,
        help="first local tontester port; choose a distinct range for concurrent networks",
    )
    parser.add_argument("--node-verbosity", type=int, default=2)
    parser.add_argument("--console-verbosity", type=int, default=0)
    parser.add_argument(
        "--node-env",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="set a validator-engine environment variable (repeatable; local experiments only)",
    )
    parser.add_argument("--startup-timeout", type=float, default=180.0)
    parser.add_argument("--deployment-timeout", type=float, default=1_800.0)
    parser.add_argument("--spam-timeout-slack", type=float, default=180.0)
    parser.add_argument("--node-arg", action="append", default=[])
    args = parser.parse_args()
    if args.state_manifest is not None:
        args.state_manifest = args.state_manifest.resolve()
        if not args.state_manifest.is_file():
            parser.error(f"--state-manifest does not exist: {args.state_manifest}")
    if args.pool_size < 1 or args.pool_size >= 2**32 - 1:
        parser.error("--pool-size must be in [1, 2^32-2]")
    if args.rate <= 0 or args.duration <= 0:
        parser.error("--rate and --duration must be positive")
    args.rate_phases = []
    if args.rate_schedule:
        try:
            for index, phase in enumerate(args.rate_schedule.split(",")):
                rate_text, duration_text = phase.split(":", 1)
                rate = float(rate_text)
                duration = float(duration_text)
                if rate < 0 or duration <= 0:
                    raise ValueError
                args.rate_phases.append({"phase": index, "rate": rate, "duration_s": duration})
        except ValueError:
            parser.error("--rate-schedule must contain non-negative rate:positive-seconds phases")
        if not args.rate_phases or not any(phase["rate"] > 0 for phase in args.rate_phases):
            parser.error("--rate-schedule must contain at least one positive-rate phase")
        args.duration = sum(phase["duration_s"] for phase in args.rate_phases)
    if args.warmup < 0 or args.drain < 0:
        parser.error("--warmup and --drain must be non-negative")
    if not 0 <= args.track_sample <= 1:
        parser.error("--track-sample must be in [0,1]")
    if args.block_period_ms <= 0 or args.node_threads <= 0:
        parser.error("--block-period-ms and --node-threads must be positive")
    if not 1_024 <= args.base_port <= 64_533:
        parser.error("--base-port must be in [1024, 64533]")
    if not 0 <= args.min_split <= args.max_split <= 60:
        parser.error("split bounds must satisfy 0 <= --min-split <= --max-split <= 60")
    if args.block_limit_mul <= 0 or args.gas_limit_mul <= 0:
        parser.error("--block-limit-mul and --gas-limit-mul must be positive")
    args.node_environment = {}
    for item in args.node_env:
        name, separator, value = item.partition("=")
        if not separator or not name:
            parser.error("--node-env must use NAME=VALUE")
        if name in args.node_environment:
            parser.error(f"duplicate --node-env name: {name}")
        args.node_environment[name] = value
    return args


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_metadata(source_root: Path) -> dict[str, Any]:
    def run(*args: str) -> str:
        return subprocess.run(
            ["git", *args],
            cwd=source_root,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        ).stdout.strip()

    return {
        "commit": run("rev-parse", "HEAD"),
        "branch": run("branch", "--show-current"),
        "dirty": bool(run("status", "--porcelain")),
    }


def load_state_manifest(path: Path) -> tuple[ExternalBasechainState, Path, dict[str, Any]]:
    raw = json.loads(path.read_text())
    if not isinstance(raw, dict):
        raise RuntimeError(f"state manifest must be a JSON object: {path}")
    celldb_value = raw.get("celldb_path")
    if not isinstance(celldb_value, str):
        raise RuntimeError(f"state manifest has no string celldb_path: {path}")
    celldb = Path(celldb_value).resolve()
    if not (celldb / "CURRENT").is_file():
        raise RuntimeError(f"state manifest CellDB is not a RocksDB directory: {celldb}")
    return ExternalBasechainState.from_manifest(path), celldb, raw


async def run_process(command: list[str], *, cwd: Path, log_path: Path, timeout: float) -> None:
    LOG.info("running: %s", subprocess.list2cmdline(command))
    process = await asyncio.create_subprocess_exec(
        *command,
        cwd=cwd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )
    assert process.stdout is not None

    async def stream() -> None:
        with log_path.open("wb") as output:
            while line := await process.stdout.readline():
                output.write(line)
                output.flush()
                sys.stdout.buffer.write(line)
                sys.stdout.buffer.flush()

    stream_task = asyncio.create_task(stream())
    try:
        return_code = await asyncio.wait_for(process.wait(), timeout=timeout)
    except TimeoutError:
        process.terminate()
        await process.wait()
        raise RuntimeError(f"command timed out after {timeout:.0f}s: {command[0]}")
    finally:
        await stream_task
    if return_code != 0:
        raise RuntimeError(f"command exited with status {return_code}: {command[0]}")


async def wait_for_balance(client: Any, address: Address, timeout: float) -> int:
    async def poll() -> int:
        while True:
            try:
                state = await client.raw_get_account_state(address)
            except TonlibError:
                await asyncio.sleep(0.2)
                continue
            if state.balance > 0:
                return state.balance
            await asyncio.sleep(0.2)

    return await asyncio.wait_for(poll(), timeout=timeout)


async def wait_for_active(client: Any, address: Address, timeout: float) -> Any:
    async def poll() -> Any:
        while True:
            try:
                state = await client.raw_get_account_state(address)
            except TonlibError:
                await asyncio.sleep(0.2)
                continue
            if state.code:
                return state
            await asyncio.sleep(0.2)

    return await asyncio.wait_for(poll(), timeout=timeout)


async def read_wallet_seqno(client: Any, address: Address) -> int:
    state = await client.raw_get_account_state(address)
    if not state.code or not state.data:
        raise RuntimeError(f"WalletSpam is not active: {address.to_str()}")
    return Cell.one_from_boc(state.data).begin_parse().load_uint(32)


async def fund_wallet_zero(main_wallet: Any, destination: Address, amount: int) -> None:
    message = WalletMessage(
        send_mode=3,
        message=MessageAny(
            info=InternalMsgInfo(
                ihr_disabled=True,
                bounce=False,
                bounced=False,
                src=main_wallet.address,
                dest=destination,
                value=CurrencyCollection(grams=amount),
                ihr_fee=0,
                fwd_fee=0,
                created_lt=0,
                created_at=0,
            ),
            init=None,
            body=Cell.empty(),
        ),
    )
    await main_wallet.send(message)


def extract_collation_stats(node_log: Path, output: Path) -> int:
    count = 0
    with node_log.open("r", errors="replace") as source, output.open("w") as destination:
        for line in source:
            marker = "JETTON_SIM_COLLATION"
            if marker not in line:
                continue
            payload = line[line.index(marker) + len(marker) :]
            start = payload.find("{")
            end = payload.rfind("}")
            if start < 0 or end < start:
                raise RuntimeError("malformed JETTON_SIM_COLLATION record")
            record = json.loads(payload[start : end + 1])
            destination.write(json.dumps(record, separators=(",", ":")) + "\n")
            count += 1
    return count


async def run(args: argparse.Namespace) -> int:
    source_root = args.source_root.resolve()
    build_dir = args.build_dir.resolve()
    workdir = args.workdir.resolve()
    if workdir.exists() and any(workdir.iterdir()):
        raise RuntimeError(f"run directory is not empty: {workdir}")
    workdir.mkdir(parents=True, exist_ok=True)
    pool_dir = workdir / "pool"
    pool_dir.mkdir()
    simulator = build_dir / "local/jetton-research/simulator/jetton-simulator"
    validator = build_dir / "validator-engine/validator-engine"
    tonlib = build_dir / "tonlib/libtonlibjson.so"
    state_generator = build_dir / "benchmark/bench-state-gen"
    required_artifacts = [simulator, validator, tonlib]
    if args.state_manifest is not None:
        required_artifacts.append(state_generator)
    for required in required_artifacts:
        if not required.exists():
            raise RuntimeError(f"required release-build artifact is missing: {required}")

    prepare_command = [
        str(simulator),
        "prepare",
        "--pool-size",
        str(args.pool_size),
        "--out-dir",
        str(pool_dir),
    ]
    await run_process(
        prepare_command,
        cwd=source_root,
        log_path=workdir / "prepare.log",
        timeout=max(60.0, args.pool_size / 1_000),
    )
    pool = json.loads((pool_dir / "pool.json").read_text())

    run_manifest: dict[str, Any] = {
        "version": 1,
        "status": "starting",
        "started_at_unix": time.time(),
        "source_root": str(source_root),
        "build_dir": str(build_dir),
        "workdir": str(workdir),
        "git": git_metadata(source_root),
        "binaries": {
            "jetton_simulator": {"path": str(simulator), "sha256": sha256(simulator)},
            "validator_engine": {"path": str(validator), "sha256": sha256(validator)},
            "tonlibjson": {"path": str(tonlib), "sha256": sha256(tonlib)},
        },
        "topology": {
            "dht_nodes": 1,
            "validator_full_nodes": 1,
            "workchain_min_split": args.min_split,
            "workchain_max_split": args.max_split,
            "shard_validators": 1,
            "consensus": "simplex-v2",
            "target_block_rate_ms": args.block_period_ms,
            "node_threads": args.node_threads,
            "base_port": args.base_port,
            "node_args": args.node_arg,
            "node_environment": args.node_environment,
            "split_merge_timings_s": {
                "delay": 20,
                "interval": 20,
                "minimum_interval": 10,
                "maximum_delay": 1000,
            },
            "block_limit_mul": args.block_limit_mul,
            "gas_limit_mul": args.gas_limit_mul,
            "force_empty_first_slot": args.force_empty_first_slot,
        },
        "workload": {
            "pool_size": args.pool_size,
            "init_mode": args.init_mode,
            "rate": args.rate,
            "rate_schedule": args.rate_phases,
            "duration": args.duration,
            "warmup": args.warmup,
            "drain": args.drain,
            "track_sample": args.track_sample,
            "rng_seed": args.rng_seed,
            "max_inflight": args.max_inflight,
            "presign": args.presign,
            "signer_threads": args.signer_threads,
            "connections": args.connections,
        },
        "pool": pool,
    }
    external_state = None
    source_celldb = None
    if args.state_manifest is not None:
        external_state, source_celldb, state_manifest = load_state_manifest(args.state_manifest)
        run_manifest["external_state"] = {
            "manifest": str(args.state_manifest),
            "manifest_sha256": sha256(args.state_manifest),
            "celldb": str(source_celldb),
            "root_hash_hex": state_manifest.get("root_hash_hex"),
            "file_hash_hex": state_manifest.get("file_hash_hex"),
            "num_v5": state_manifest.get("num_v5"),
            "num_ballast": state_manifest.get("num_ballast"),
            "ballast_cells": state_manifest.get("ballast_cells"),
        }
        run_manifest["binaries"]["state_generator"] = {
            "path": str(state_generator),
            "sha256": sha256(state_generator),
        }
    manifest_path = workdir / "run.json"
    manifest_path.write_text(json.dumps(run_manifest, indent=2) + "\n")

    install = Install(build_dir, source_root)
    install.tonlibjson.client_set_verbosity_level(0)
    try:
        async with Network(install, workdir / "network") as network:
            # The simulator may coexist with other local networks. Tontester allocates
            # all node and public-overlay ports relative to this internal cursor.
            network._port = args.base_port
            consensus = SimplexConsensusConfig(target_block_rate_ms=args.block_period_ms)
            network.config.mc_consensus = consensus
            network.config.shard_consensus = consensus
            network.config.monitor_min_split = 0
            network.config.split = args.min_split
            network.config.max_split = args.max_split
            network.config.shard_validators = 1
            network.config.block_limit_mul = args.block_limit_mul
            network.config.gas_limit_mul = args.gas_limit_mul
            network.external_basechain = external_state

            dht = network.create_dht_node()
            node = network.create_full_node()
            node.make_initial_validator()
            node.announce_to(dht)
            if source_celldb is not None:
                celldb_dst = node.directory / "celldb"
                if celldb_dst.exists() or celldb_dst.is_symlink():
                    if celldb_dst.is_dir() and not celldb_dst.is_symlink():
                        shutil.rmtree(celldb_dst)
                    else:
                        celldb_dst.unlink()
                await run_process(
                    [
                        str(state_generator),
                        "checkpoint",
                        "--src",
                        str(source_celldb),
                        "--dst",
                        str(celldb_dst),
                    ],
                    cwd=source_root,
                    log_path=workdir / "checkpoint.log",
                    timeout=1_800.0,
                )
            node_environment = dict(args.node_environment)
            if args.force_empty_first_slot:
                node_environment["TON_SIM_FORCE_EMPTY_FIRST_SLOT"] = "1"
            node_options = StartOptions(
                threads=args.node_threads,
                verbosity=args.node_verbosity,
                console_verbosity=args.console_verbosity,
                env=node_environment,
                args=tuple(
                    (["--disable-state-serializer"] if external_state is not None else [])
                    + args.node_arg
                ),
            )
            async with asyncio.TaskGroup() as start_group:
                start_group.create_task(dht.run())
                start_group.create_task(node.run(node_options))

            await asyncio.wait_for(network.wait_mc_block(seqno=2), timeout=args.startup_timeout)
            await asyncio.wait_for(
                network.wait_block(workchain=0, shard=FULL_SHARD, seqno=2),
                timeout=args.startup_timeout,
            )
            client = await node.tonlib_client()
            main_wallet = network.zerostate.main_wallet(client)
            wallet0 = Address(pool["wallet0"])
            wallet1 = Address(pool["wallet1"])
            sentinel_jetton = Address(pool["sentinel_jetton"])

            LOG.info("funding WalletSpam #0 with %s nanotons", pool["funding_nanoton"])
            await fund_wallet_zero(main_wallet, wallet0, int(pool["funding_nanoton"]))
            funded_balance = await wait_for_balance(client, wallet0, args.startup_timeout)
            run_manifest["funded_wallet0_balance"] = funded_balance
            manifest_path.write_text(json.dumps(run_manifest, indent=2) + "\n")

            LOG.info("submitting deployment kickoff %s", pool["kickoff_message_hash"])
            await client.raw_send_message((pool_dir / "kickoff-external.boc").read_bytes())
            await wait_for_active(client, sentinel_jetton, args.deployment_timeout)
            wallet0_seqno = await read_wallet_seqno(client, wallet0)
            wallet1_seqno = await read_wallet_seqno(client, wallet1)
            if wallet0_seqno != 1 or wallet1_seqno != 0:
                raise RuntimeError(
                    f"unexpected post-deployment seqnos: wallet0={wallet0_seqno}, wallet1={wallet1_seqno}"
                )
            run_manifest["deployment"] = {
                "ready_at_unix": time.time(),
                "wallet0_seqno": wallet0_seqno,
                "wallet1_seqno": wallet1_seqno,
                "sentinel_jetton_active": True,
            }
            endpoint = node.liteserver_endpoint()
            run_manifest["liteserver"] = {
                "host": endpoint.host,
                "port": endpoint.port,
                "pubkey_b64": endpoint.pubkey_b64,
            }
            manifest_path.write_text(json.dumps(run_manifest, indent=2) + "\n")

            spam_command = [
                str(simulator),
                "spam",
                "--pool-size",
                str(args.pool_size),
                "--init-mode",
                str(args.init_mode),
                "--liteserver",
                f"{endpoint.host}:{endpoint.port}",
                "--liteserver-pubkey-b64",
                endpoint.pubkey_b64,
                "--rate",
                str(args.rate),
                "--duration",
                str(args.duration),
                "--warmup",
                str(args.warmup),
                "--drain",
                str(args.drain),
                "--track-sample",
                str(args.track_sample),
                "--rng-seed",
                str(args.rng_seed),
                "--max-inflight",
                str(args.max_inflight),
                "--presign",
                str(args.presign),
                "--signer-threads",
                str(args.signer_threads),
                "--connections",
                str(args.connections),
                "--out",
                str(workdir / "results.json"),
                "--blocks-csv",
                str(workdir / "blocks.csv"),
                "--timeline-csv",
                str(workdir / "timeline.csv"),
                "--traces-csv",
                str(workdir / "traces.csv"),
                "--topology-csv",
                str(workdir / "topology.csv"),
            ]
            if args.rate_schedule:
                spam_command.extend(["--rate-schedule", args.rate_schedule])
            run_manifest["spam_command"] = spam_command
            manifest_path.write_text(json.dumps(run_manifest, indent=2) + "\n")
            await run_process(
                spam_command,
                cwd=source_root,
                log_path=workdir / "spam.log",
                timeout=args.duration + args.drain + args.spam_timeout_slack,
            )
            results = json.loads((workdir / "results.json").read_text())
            run_manifest["results"] = results
            run_manifest["status"] = "complete"
            run_manifest["completed_at_unix"] = time.time()
            manifest_path.write_text(json.dumps(run_manifest, indent=2) + "\n")
    except BaseException as error:
        run_manifest["status"] = "failed"
        run_manifest["failure"] = repr(error)
        run_manifest["completed_at_unix"] = time.time()
        manifest_path.write_text(json.dumps(run_manifest, indent=2) + "\n")
        raise
    finally:
        node_log = workdir / "network/node1/log"
        if node_log.exists():
            count = extract_collation_stats(node_log, workdir / "collation.jsonl")
            run_manifest["collation_records"] = count
            manifest_path.write_text(json.dumps(run_manifest, indent=2) + "\n")

    analysis_path = workdir / "analysis.json"
    try:
        await run_process(
            [
                sys.executable,
                str(Path(__file__).resolve().parent / "analyze.py"),
                str(workdir),
                "--json-out",
                str(analysis_path),
            ],
            cwd=source_root,
            log_path=workdir / "analysis.log",
            timeout=60.0,
        )
        run_manifest["analysis"] = {"path": str(analysis_path)}
        manifest_path.write_text(json.dumps(run_manifest, indent=2) + "\n")
    except BaseException as error:
        run_manifest["status"] = "failed"
        run_manifest["analysis_failure"] = repr(error)
        manifest_path.write_text(json.dumps(run_manifest, indent=2) + "\n")
        raise

    LOG.info("run complete: %s", workdir)
    return 0


def main() -> int:
    args = parse_args()
    logging.basicConfig(
        level=logging.INFO,
        format="[%(levelname)s][%(asctime)s][%(name)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )
    return asyncio.run(run(args))


if __name__ == "__main__":
    raise SystemExit(main())
