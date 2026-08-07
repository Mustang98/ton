# Jetton collation simulator

This directory contains a local one-to-two-shard workload driver for the
`ton jetton-spam` transaction shape. It deploys the same custom WalletSpam and
prepaid JettonWallet contracts, sends the same signed external messages, runs a
real C++ validator/collator, and accounts for the complete TX1-TX4 chain from
accepted workchain blocks.

The exact frozen workload and the rationale for the architecture are in
[`DESIGN.md`](DESIGN.md).

## What one transfer means

One offered jetton transfer can create four workchain transactions:

1. TX1: the sender WalletSpam accepts the external and emits a transfer request.
2. TX2: the sender JettonWallet debits one jetton and emits `internal_transfer`.
3. TX3: the recipient JettonWallet credits one jetton and emits the excess reply.
4. TX4: the original WalletSpam receives that reply.

The reported jTPS is successful TX3 per second. Raw TPS counts every transaction
in accepted workchain-0 blocks in the measurement window. These are deliberately
separate numbers; for this workload, one completed transfer normally contributes
four raw transactions.

## Build

Use the repository's supported Ubuntu build entrypoint. The simulator target is
registered in that script together with `validator-engine` and `tonlibjson`:

```bash
cd /home/vallas/ton
bash assembly/native/build-ubuntu-shared.sh
./build/local/jetton-research/simulator/jetton-simulator self-test
```

After the first full configuration, an incremental rebuild is sufficient while
editing the simulator or instrumentation:

```bash
cmake --build build --target jetton-simulator validator-engine -j"$(nproc)"
```

The Python harness requires Python 3.13+ and the `test/tontester` environment. If
generated TL bindings are absent, generate them once:

```bash
PYTHONPATH=test/tontester/src \
  uv run --project test/tontester python test/tontester/generate_tl.py
```

## Run

The default command creates 10,000 active wallets and offers 1,000 transfers per
second for 60 seconds. It uses init mode 0, which attaches the full recipient
JettonWallet StateInit to `internal_transfer`:

```bash
uv run --project test/tontester python \
  local/jetton-research/simulator/run.py
```

Use a fresh explicit run directory for named experiments:

```bash
uv run --project test/tontester python \
  local/jetton-research/simulator/run.py \
  --pool-size 10000 \
  --rate 1000 \
  --duration 60 \
  --warmup 5 \
  --drain 15 \
  --workdir local/jetton-research/simulator/runs/10k-1000jtps
```

Useful controls are `--init-mode` (`0` full StateInit, `1` bare recipient address),
`--block-period-ms`, `--node-threads`, `--connections`,
`--max-inflight`, `--presign`, `--signer-threads`, `--track-sample`, and repeated
`--node-arg`. `--min-split` and `--max-split` control the workchain shard-depth
bounds. `--block-limit-mul` and `--gas-limit-mul` change diagnostic zerostate
limits. `--force-empty-first-slot` makes the first shard slot in every four-slot
Simplex leader window collate an empty block, providing a stable productive-slot
ratio for limit-scaling comparisons. Defaults use one DHT node, one full validator, Simplex v2, one unsplit
workchain shard, a 400 ms target block period, node verbosity 2, and no node log
mirroring to the terminal. Full raw node logs are still preserved.

A single process can run a rate staircase while preserving sender seqnos:

```bash
uv run --project test/tontester python \
  local/jetton-research/simulator/run.py \
  --pool-size 10000 \
  --min-split 0 \
  --max-split 1 \
  --rate-schedule '380:45,100:45,400:45' \
  --workdir local/jetton-research/simulator/runs/split-sweep
```

Use a pool larger than `rate * duration` for short correctness runs when wallet
reuse is not under test. With a tiny pool, a sender can be reused before its prior
seqno reaches chain state; the liteserver then correctly rejects future-seqno
externals. The production-shaped `10000 1000` workload normally leaves about ten
seconds between uses of the same sender.

## Analyze

Each completed run can be summarized with an exact accepted-block-to-candidate
hash join:

```bash
uv run --project test/tontester python \
  local/jetton-research/simulator/analyze.py \
  local/jetton-research/simulator/runs/10k-1000jtps
```

The analyzer writes `analysis.json` and prints the stage counts, effective jTPS,
raw TPS, candidate work-time distribution, CPU phase shares, queue movement, and
block-limit state. Treat a throughput point as sustainable only when TX3 tracks
accepted TX1, completion latency and queues do not trend upward, candidate cadence
holds, and block/parser joins are complete.

The first correctness and overload probes are recorded in
[INITIAL_RESULTS.md](INITIAL_RESULTS.md). The one-to-two-shard threshold and
raised-limit comparison are in [SHARDING_RESULTS.md](SHARDING_RESULTS.md).

## Artifacts

Every run directory contains:

| Path | Contents |
| --- | --- |
| `run.json` | Git state, binary hashes, topology, exact command, deployment checks, and final results |
| `pool/pool.json` | Contract hashes, deterministic addresses, key material, funding, and kickoff hash |
| `results.json` | Generator, transport, total/steady stage counts, rates, parser integrity, and latency percentiles |
| `blocks.csv` | Accepted workchain blocks, shard IDs, split/merge flags, exact root hashes, and TX1-TX4 counters |
| `timeline.csv` | Per-second rate phase, construction, offers, acknowledgements, errors, TX1, and TX3 progress |
| `traces.csv` | Sampled request identity, TX1-TX4 shard/block IDs, status, and causal stage latencies |
| `topology.csv` | Every observed workchain shard-set change and its top block seqnos |
| `collation.jsonl` | Valid JSON records for every successful candidate, including phase timing and queues |
| `analysis.json` | Joined bottleneck summary produced by `analyze.py` |
| `network/node1/log` | Complete validator log |
| `prepare.log`, `spam.log` | Workload preparation and C++ sender logs |

`acknowledged` means the liteserver accepted `sendMessage`; it is not block
inclusion. `tx1_total` is exact inclusion by external-message cell hash.
`unmatched_externals` includes rejected sends and messages still absent when the
drain window ends. A result with skipped blocks or parse failures has
`valid_stage_counts=false` and must not be used for throughput claims.

## Measurement sequence

Start with a no-reuse low-rate run, then deploy the exact 10,000-wallet pool at a
low rate. After both preserve TX1=TX2=TX3=TX4, ramp the offered rate at fixed block
cadence, bracket the first point with growing backlog, and profile that point with
`perf`. Compare optimizations only with identical pool, seed, block period, node
arguments, binary build mode, and warmup/drain windows.

This oracle still runs local consensus and block acceptance. Collator timings come
directly from the candidate builder, so consensus time is not charged to those
phase counters. A post-admission feeder is intentionally deferred until this
end-to-end oracle establishes correctness and quantifies whether checker or
liteserver work interferes with collation measurements.
