# One-Shard Jetton Collation Optimization Report

**Research date:** 2026-08-05 through 2026-08-07 UTC  
**Repository:** `/home/vallas/ton`  
**Branch:** `bench-parallel-merge-collators`  
**Recorded Git HEAD:** `5f07d9346d0cdbb707f72cc7a1b5ca25fcd912d2`  
**Status:** Experimental result, reproduced twice below 100 ms; not yet a production-ready patch set

## 1. Executive summary

This research measured and optimized the time required by one TON basechain
collator to build productive blocks under a fixed synthetic jetton-transfer load.
The benchmark intentionally isolates collation while retaining real TON smart
contracts, TVM execution, state dictionaries, Merkle proofs, block serialization,
candidate validation, and one-validator consensus.

The controlled workload was:

- one basechain shard;
- a generated state containing 45,000,000 ballast accounts;
- 10,000 active `WalletSpam` sender contracts;
- 380 jetton-transfer requests per second;
- four workchain transactions per successful request;
- approximately 1,520 transactions per second;
- approximately 810 transactions per productive block;
- doubled block-byte and gas limits;
- one deliberately empty first slot in every four-slot leader window.

The original productive-block collation time was **332.809 ms mean and 444.851
ms P95**. The final two independent runs measured:

| Run | Mean collation | P95 collation | Mean validation | TX/productive block | Mean block size |
|---|---:|---:|---:|---:|---:|
| H105 | 98.660 ms | 123.138 ms | 93.890 ms | 810.815 | 1.551 MB |
| H106 repeat | 97.771 ms | 125.221 ms | 94.666 ms | 813.037 | 1.565 MB |
| Final average | **98.216 ms** | **124.180 ms** | **94.278 ms** | **811.926** | **1.558 MB** |

A later 120-second optimized doubled-limit run measured 100.373 ms mean and
125.689 ms P95 at 809.198 transactions per productive block. This is 2.158 ms
above the two short-run mean but still a 69.8% reduction from the matching long
baseline. Section 13.6 gives the full four-way long-run comparison.

Relative to the baseline, the average of the two final confirmations gives:

- **70.5% less mean collation wall time**;
- a **3.39x collation-time ratio improvement**;
- **72.1% less P95 collation time**;
- **25.2% less mean validation time**;
- no reduction in transactions per productive block;
- about **4.2% smaller mean serialized blocks**;
- **65 deadline-limited baseline candidates reduced to zero**.

The gain did not come from one change. It came from removing duplicate external
work, caching repeated cryptographic verification, replacing repeated dictionary
mutations with exact batched mutations, moving exact state accounting off the
critical path, executing independent account work in deterministic parallel
waves, batching external-pool delivery, and overlapping or parallelizing final
state proofs and serialization.

The final result is a reproducible research result, not a claim that the same
speedup is immediately safe on mainnet. The current implementation is a dirty
feature-flagged research branch. Multi-validator determinism, long-duration
soaks, diverse contract workloads, adversarial inputs, concurrency sanitizers,
and default-limit measurements remain production gates.

## 2. What exactly was measured

### 2.1 Target metric

The primary metric is the wall-clock work time reported by the collator for an
accepted, productive basechain candidate:

```text
productive collation time = collation.jsonl[work_real_s.total]
```

Only records satisfying all of these conditions are included:

1. The block is in the steady measurement window.
2. The block was accepted by the local chain.
3. The block has at least one transaction.
4. The accepted block root hash exactly matches the collator record root hash.

`compare_runs.py` performs this exact join between `blocks.csv` and
`collation.jsonl`. This avoids mixing discarded candidates, forced-empty slots,
startup blocks, or drain-period blocks into the productive collation statistic.

Consensus and final block acceptance are real in this setup, but their time is
not included in `work_real_s.total`. Validation is measured separately from the
validator log.

### 2.2 Secondary metrics

The report also tracks:

- P95 productive collation time;
- mean and P95 validation time;
- jetton transfers per second, or jTPS;
- all workchain transactions per second, or raw TPS;
- transactions per productive block;
- serialized block bytes;
- collation deadline overloads;
- final internal-message queue depth;
- transaction aborts and completion of TX1 through TX4;
- exact external-pool filter and rejection counters;
- selected internal collation phase times.

### 2.3 jTPS and TPS definitions

One generated request is not one blockchain transaction. A successfully
completed benchmark jetton transfer executes four workchain transactions:

1. **TX1, WalletSpam:** accepts the external request and sends an internal
   transfer request to the sender JettonWallet.
2. **TX2, sender JettonWallet:** debits the sender jetton balance and emits the
   transfer notification to the recipient JettonWallet.
3. **TX3, recipient JettonWallet:** credits the recipient. The analyzer counts
   this event as one completed jetton transfer.
4. **TX4, WalletSpam reply:** receives the excess or completion message.

Therefore:

```text
jTPS     = completed TX3 transactions / measured seconds
raw TPS  = all TX1 + TX2 + TX3 + TX4 transactions / measured seconds
raw TPS  is approximately 4 * jTPS for a fully drained run
```

At 380 offered requests/s, the expected completed load is approximately 380
jTPS and 1,520 TPS.

### 2.4 Why approximately 810 transactions per productive block

The test uses a 400 ms target slot and a four-slot leader window, but forces the
first slot of each complete leader window to be empty. The resulting productive
cadence is approximately 1.875 blocks/s:

```text
380 requests/s * 4 tx/request / 1.875 productive blocks/s
  = approximately 811 transactions/productive block
```

This forced schedule is a benchmark invariant. A run with four productive slots
per window is not directly comparable because it distributes the same request
rate over more, smaller blocks.

## 3. Scope and non-goals

The experiment focuses on **one-shard collation throughput and latency**. It
retains enough of the real node to exercise the actual collation data structures
and output, while placing these subjects outside the primary claim:

- liteserver request latency;
- public-overlay external-message propagation;
- multi-validator candidate propagation;
- consensus latency and network loss;
- block finality latency;
- indexer and user-wallet synchronization;
- shard split/merge behavior;
- cross-shard message queues;
- mainnet state and contract diversity.

Those systems matter in production. They were deliberately held fixed here so
that a change in measured time could be attributed to the collator and validator
rather than to network variance.

## 4. Experimental system

### 4.1 Host

| Property | Value |
|---|---|
| CPU | AMD Ryzen 9 7950X3D |
| Physical/logical CPUs | 16 cores / 32 hardware threads |
| L3 cache | 128 MiB |
| RAM | 124 GiB visible |
| Kernel | Ubuntu `5.15.0-177-generic` |
| Compiler | `/usr/bin/clang++-22` |
| Build type | Release, directory `build-clang-release` |

Thread-count results are specific to this topology. The final configuration uses
32 transaction worker threads and bounded task counts of 16 for selected proof
and rebind operations. These values require retuning on NUMA hosts, smaller
machines, and production validators that perform additional work concurrently.

### 4.2 Node topology

The simulator starts:

- one DHT node;
- one validator/full node;
- one basechain shard, with `min_split=0` and `max_split=0`;
- Simplex consensus v2;
- a 400 ms target slot;
- 8 validator-engine actor threads;
- real candidate validation and local block acceptance.

The one-validator network is an oracle-style test topology. It preserves the
collator and validator pipeline but removes remote validator and network timing
variance.

### 4.3 Block limits

The benchmark uses a `2x` multiplier for block-byte and gas limits. The relevant
basechain limits are:

| Limit | Default | Test value |
|---|---:|---:|
| Bytes, underload | 262,144 | 262,144 |
| Bytes, soft | 1,048,576 | 2,097,152 |
| Bytes, hard | 2,097,152 | 4,194,304 |
| Gas, underload | 2,000,000 | 2,000,000 |
| Gas, soft | 10,000,000 | 20,000,000 |
| Gas, hard | 20,000,000 | 40,000,000 |
| LT delta, underload | 1,000 | 1,000 |
| LT delta, soft | 5,000 | 10,000 |
| LT delta, hard | 10,000 | 20,000 |

`full_collated_data` is enabled. The multipliers apply to soft and hard limits;
underload thresholds remain unchanged. The node derives intermediate thresholds
from these configured values.

This is important: the result is not a claim that default mainnet limits can
carry 810 transactions in every productive block. The benchmark asks whether a
single collator can build and validate that larger block in time when the
protocol limits permit it.

## 5. Generated blockchain state

The base state was produced locally by `bench-state-gen` and is stored outside
Git under:

```text
_local/jetton-research/generated-db-45m/state
```

### 5.1 State identity

| Field | Value |
|---|---|
| State manifest SHA-256 | `17b701a6193be8169afc43d737804fc1de216b81cbd30b87f061b47aa3916aa5` |
| State root hash | `6391ba26223923f1d94e56ac45d6f1c8252ec01e6bb04a1341ce7e21193fd765` |
| State file hash | `a26bc2f5eba017412475248ba119c5e28332329242e0e6a8835a8faa182254cb` |
| V5 accounts | 0 |
| Ballast accounts | 45,000,000 |
| Cells per ballast account | 17 |
| Generator seed | `299d6b1c2c8e16cc2ac04cb10bf2395239136cdd3dfc311c3dcdcbcd8e4b2ea1` |
| Generation Unix time | 1,785,887,463 |

The cell database has roughly 169 GB of logical SST content. It exists to make
dictionary access, proof generation, and cell storage more realistic than a tiny
fresh chain. It is still synthetic and must not be treated as a byte-for-byte
mainnet state substitute.

### 5.2 Exact generation command

The authoritative local script is:

```text
_local/jetton-research/generated-db-45m/generate.sh
```

Its effective generator invocation is:

```bash
build-clang-release/benchmark/bench-state-gen gen \
  --v5-count 0 \
  --ballast-count 45000000 \
  --ballast-cells 17 \
  --seed-hex 299d6b1c2c8e16cc2ac04cb10bf2395239136cdd3dfc311c3dcdcbcd8e4b2ea1 \
  --gen-utime 1785887463 \
  --contracts-dir _local/jetton-research/generated-db-45m/contracts \
  --out-dir _local/jetton-research/generated-db-45m/state \
  --tmp-dir _local/jetton-research/generated-db-45m/tmp \
  --threads 32 \
  --merge-shards 32 \
  --overwrite
```

The checked local script points at its preserved generator binary instead of
`build-clang-release`; the arguments above are otherwise identical. Reproduction
must verify the resulting manifest hashes before comparing performance.

## 6. Workload construction

### 6.1 Sender and recipient pool

The harness deploys 10,001 `WalletSpam` contracts: 10,000 load-generating
senders and one readiness sentinel. The active contracts share a deterministic
Ed25519 key and differ by subwallet ID. Each sender has a prepaid JettonWallet.

Random sender/recipient selection is deterministic:

```text
pool RNG seed: 1
pool seed: 0123456789ABCDEFFEDCBA98765432100123456789ABCDEFFEDCBA9876543210
```

This reproducibility has one performance consequence: a shared public key and
repeated exact signed input paths create a favorable workload for the successful
signature cache. Production testing must add many independent keys and a real
distribution of retries and invalid signatures.

### 6.2 StateInit mode

The workload uses `init_mode=0`, which includes the recipient JettonWallet
`StateInit` in the transfer message. Recipients are predeployed, so this tests the
ordinary message shape and serialization/accounting cost without turning every
transfer into a new deployment.

The earlier `init_mode=1` mode omitted this StateInit and is not used in the
controlled optimization chain.

### 6.3 Offered load and acceptance

The sender emits exactly 380 requests/s for the requested duration. The
simulator's `acknowledged` counter means the liteserver accepted the send. It does
not by itself prove block inclusion. Inclusion is established separately by the
TX1 through TX4 analyzer and by the final queue depth.

The baseline used a long measurement:

```text
duration: 120 s
warmup:    10 s
drain:     30 s
```

Most later cumulative A/B runs used:

```text
duration: 20 s
warmup:     5 s
drain:     12 s
```

The final two runs each offered and acknowledged 7,600 requests and observed
exactly 30,400 workchain transactions. Both drained to an empty queue with no
transaction aborts.

## 7. Simulator and artifact layout

The simulator implementation is under:

```text
local/jetton-research/simulator
```

The fixed comparable-run wrapper and analysis tools are under:

```text
local/jetton-research/experiments
```

`run_fixed.py` hard-codes the comparison invariants:

- generated 45M-account state manifest;
- pool size 10,000;
- `init_mode=0`;
- one shard;
- forced empty first slot;
- 8 node actor threads.

Every run directory is self-describing. Important files are:

| Artifact | Purpose |
|---|---|
| `run.json` | command, environment, timings, binary provenance |
| `pool/pool.json` | generated wallet identities and deterministic pool metadata |
| `results.json` | simulator completion and request totals |
| `blocks.csv` | accepted blocks, transaction counts, roots, steady-window flag |
| `timeline.csv` | request and block timeline |
| `traces.csv` | TX1 through TX4 flow reconstruction |
| `topology.csv` | observed shard topology |
| `collation.jsonl` | per-candidate collation counters and phase timings |
| `analysis.json` | aggregate throughput, queue, block, and collation report |
| `network/node1/log` | validator/collator/validation log |
| `prepare.log` | network and account preparation |
| `spam.log` | external-message load generator |

Do not compare runs only by their directory name. Verify `run.json`, state
manifest, feature environment, transaction density, final queue, and accepted
block schedule first.

## 8. Controlled baseline

The authoritative baseline is:

```text
local/jetton-research/simulator/runs/
  45m-one-shard-380rps-limits2x-force-empty-first-10k-20260805
```

| Metric | Baseline |
|---|---:|
| Offered requests | 45,600 |
| Acknowledged requests | 45,500 |
| Send errors | 100 |
| Completed TX1 through TX4 | 45,500 each |
| Total workchain transactions | 182,000 |
| Transaction aborts | 0 |
| Productive steady blocks | 275 |
| Steady jTPS | 378.976 |
| Steady raw TPS | 1,515.303 |
| Transactions/productive block | 809.117 |
| Mean/P95 productive collation | **332.809 / 444.851 ms** |
| Mean/P95 productive validation | 125.979 / 174.472 ms |
| Mean/P95 block size | 1.626 / 2.018 MB |
| Collation-deadline candidates | 65 |
| Final internal-message queue | 0 |
| Final shard count | 1 |

The 100 send errors occurred at the offered-message boundary, not as incomplete
on-chain transfers: every acknowledged request completed all four transactions.

The run was close to the configured byte/collated-data capacity and repeatedly
hit the collation deadline. It established the problem to solve: build the same
approximately 810-transaction productive block well below the 400 ms slot target
without allowing a queue to accumulate.

## 9. Initial profiling and hypothesis selection

Native `perf` sampling was used to locate expensive code, never to report
throughput. Throughput numbers always come from normal Release runs without a
profiler attached.

The initial process-wide profile contained these high-cost symbols or families:

| Native work | Approximate process samples |
|---|---:|
| SHA-256 | 6.69% |
| `DataCell::create` | 4.75% |
| Ed25519 field arithmetic | 7.61% |
| `CellSlice::prefetch_ref` | 2.85% |
| `memmove` | 1.95% |
| DataCell destruction | 1.86% |

This profile led to four major lines of work:

1. Eliminate repeated external-message and signature work.
2. Reduce repeated Patricia-dictionary mutation and proof accounting.
3. Parallelize independent account work while preserving deterministic order.
4. Overlap the final state proof, rebind, hashing, and BOC stages.

A later H98 profile showed that the bottleneck had moved:

| Native work | Approximate process samples |
|---|---:|
| SHA-256 | 5.83% |
| `DataCell::create` | 4.12% |
| `CellUsageTree::create_child` | 3.31% |
| `CellUsageTree::on_load` | 3.08% |
| `CellSlice::prefetch_ref` | 2.55% |
| Ed25519 field multiplication | 2.52% |

That movement is consistent with the experiment: early changes removed TVM and
dictionary critical-path time; later changes had to address usage-tree rebind and
Merkle proof construction.

## 10. Cumulative optimization path

The table below is the primary progress ledger. Each `saved` value is the
observed difference from the immediately preceding representative milestone.
The runs are cumulative, so this is not a claim that every delta is an isolated,
portable microbenchmark result.

| Step | Representative result | Mean before | Mean after | Saved | Step reduction | Total reduction |
|---|---|---:|---:|---:|---:|---:|
| Baseline | 2026-08-05 baseline | - | 332.809 ms | - | - | - |
| Exact-ancestor external filtering | H3b | 332.809 | 306.635 | 26.174 ms | 7.9% | 7.9% |
| Successful `CHKSIGNU` cache | H10 repeat | 306.635 | 281.077 | 25.558 ms | 8.3% | 15.5% |
| Async exact account estimator | H11b | 281.077 | 228.820 | 52.257 ms | 18.6% | 31.2% |
| Descriptor batching, 64 | H14 | 228.820 | 214.346 | 14.474 ms | 6.3% | 35.6% |
| Early block BOC | H20 | 214.346 | 208.259 | 6.087 ms | 2.8% | 37.4% |
| Analytical estimator and async final dictionary | H38 | 208.259 | 201.781 | 6.478 ms | 3.1% | 39.4% |
| Parallel account/storage preparation | H44 | 201.781 | 190.198 | 11.583 ms | 5.7% | 42.9% |
| Parallel transaction waves/account blocks | H54b | 190.198 | 176.231 | 13.967 ms | 7.3% | 47.0% |
| Batched external-pool delivery | H68b | 176.231 | 141.554 | 34.677 ms | 19.7% | 57.5% |
| 16 workers and parallel final rebind | H70 | 141.554 | 133.551 | 8.003 ms | 5.7% | 59.9% |
| Pipelined state/proof/BOC finalization | H79 | 133.551 | 124.757 | 8.794 ms | 6.6% | 62.5% |
| Proof-backed rebind and 32 workers | H88 repeat | 124.757 | 116.268 | 8.489 ms | 6.8% | 65.1% |
| Descriptor batch size 256 | H98 | 116.268 | 108.572 | 7.696 ms | 6.6% | 67.4% |
| Parallel state proofs | H103 | 108.572 | 104.162 | 4.410 ms | 4.1% | 68.7% |
| Inner proof-backed rebind | H104 | 104.162 | 102.072 | 2.090 ms | 2.0% | 69.3% |
| Proof/rebind task count 16 | H105 | 102.072 | 98.660 | 3.412 ms | 3.3% | **70.4%** |

The arithmetic total is:

```text
332.809 ms - 98.660 ms = 234.149 ms saved in H105
332.809 ms - 97.771 ms = 235.038 ms saved in H106
```

### 10.1 Confidence levels

- H3, H10, and H11 were measured in long or repeated runs and provide the best
  independent attribution.
- H68's external-delivery result was repeated at 138.277 and 141.554 ms and is
  the strongest later-stage individual win.
- Most H14 through H104 results are exact cumulative A/B measurements lasting
  20 to 30 seconds. Their direction is useful; their precise millisecond delta
  includes run-to-run host and block-phase variance.
- H105 and H106 independently confirm the final threshold with the same binary,
  workload, and environment.

## 11. Optimization details

### 11.1 Exact-ancestor external filtering and pool pruning

**Observed problem.** An external included in an unfinalized candidate could be
offered again to the next collator on the same candidate branch. The collator
then repeated admission and transaction work for a message already represented
in the exact ancestor state.

**Implementation.** The node parses ancestor `InMsgDescr` entries of type
`msg_import_ext`, normalizes their external-message hashes, and carries a sorted
set of those exact-ancestor hashes into the collator's external callback. The
callback removes matches from its private persistent-treap snapshot and filters
matching live arrivals.

The global external pool is not mutated at this stage. Global deletion remains
coupled to finalized-block cleanup. A competing fork therefore still has access
to the external. This exact-branch boundary is the key correctness property.

**Measurement.** H3a filtered in the collator loop and measured 310.9 ms mean.
H3b moved the filter to pool delivery, removed 59,498 stale externals from private
snapshots, and measured 306.635 ms mean. Relative to baseline, H3b saved **26.174
ms per productive block**.

**Checks.** Productive blocks had no rejected duplicate transactions, all
accepted requests completed TX1 through TX4, and the final queue drained.
Telemetry is exposed through `ext_msgs_ancestor_filtered` and pool-filter counts.

**Primary code.** `validator/impl/collator.cpp`,
`validator/impl/ext-message-pool.cpp`, and finalized external cleanup.

### 11.2 Successful `CHKSIGNU` verification cache

**Observed problem.** The exact same valid Ed25519 check occurred in external
admission, candidate collation, and candidate validation. Ed25519 field
arithmetic represented about 7.61% of process-wide profile samples.

**Implementation.** A bounded, 64-shard cache stores successful `CHKSIGNU`
results. The full 128-byte identity is checked:

```text
32-byte message hash + 64-byte signature + 32-byte public key
```

Each shard has 256 direct entries. Only successful `CHKSIGNU` results are cached.
Invalid signatures are always evaluated, and `CHKSIGNS` is not cached. Full-key
comparison prevents accepting a collision based only on a shortened cache tag.

**Measurement.** H10's two long runs measured 283.708 and 281.077 ms mean versus
H3b's 306.635 ms. The conservative representative gain was **25.558 ms**, or
8.3% for this step. Cache telemetry showed a 65.8% hit rate.

The productive-block TVM phase fell from about 35.4 ms to 12.8-12.9 ms. Mean
validation fell from 119.8 ms to approximately 100.6-101.1 ms, which confirms
that repeated verification also existed on the validation path.

**Checks.** `test-smartcont` passed 10 tests and `test-vm` passed 34 tests with
the feature enabled. The cache is bounded and invalid-signature behavior remains
uncached.

**Primary code.** `crypto/vm/tonops.cpp`.

**Production concern.** The benchmark shares a public key among senders and may
overstate hit rate. Production acceptance requires key-cardinality tests,
adversarial invalid signatures, memory-bound verification, and security review.

### 11.3 Bulk sorted dictionary mutation and async exact estimator

**Observed problem.** Account changes repeatedly rebuilt overlapping paths in
the large `ShardAccounts` Patricia dictionary. Exact block-limit accounting also
materialized proof effects on the collator's critical path.

**Dictionary primitive.** `AugmentedDictionary::multiset` applies a sorted batch
of key/value changes in one traversal while preserving the exact root that
sequential updates would produce. Tests compare batched and sequential roots.

**Async estimator.** A dedicated worker owns a cloned estimator dictionary and
its own `CellUsageTree`. The collator submits the first change for each account,
continues transaction work, and joins the worker at the existing 16-operation
proof-accounting barriers. The resulting cell, bit, and reference counts remain
exact.

The worker performed about 84.6-89.9 ms of CPU work per productive block, but
only about 15-18 ms was exposed as critical-path waiting because most of it
overlapped transaction processing.

**Measurement.** H11's first and repeat runs measured 217.516 and 228.820 ms.
Using the conservative repeat, the gain from H10 was **52.257 ms**, the largest
early collation-path reduction.

Mean block size remained 1.591-1.592 MB and transaction density remained about
809 transactions per productive block. Validation stayed around 96-99 ms.

**Why exact accounting matters.** Telemetry measured approximately 1,272-1,280
proof bytes per updated account, or roughly 885 KB per productive block. A flat
historical estimate of 200 bytes/account would undercount by hundreds of
kilobytes and could let the collator violate block limits.

**Primary code.** `crypto/vm/dict.cpp`, `crypto/vm/dict.h`, and the estimator
worker in `validator/impl/collator.cpp`.

### 11.4 Batched inbound and outbound message descriptors

**Observed problem.** Every inbound and outbound message immediately mutated
`InMsgDescr` or `OutMsgDescr`, repeatedly rebuilding dictionary paths.

**Implementation.** Descriptor entries are deferred, sorted by deterministic
key, and committed with `multiset`. Flushes occur at semantic and limit-accounting
boundaries, so later code never observes an uncommitted descriptor set where it
requires the materialized dictionary.

**Measurements.** A batch size of 64 reduced mean collation from 228.820 to
214.346 ms, saving **14.474 ms**. Much later, after other bottlenecks had moved,
increasing the batch size to 256 reduced 116.268 to 108.572 ms, saving another
**7.696 ms**.

The second delta is not expected to transfer unchanged to the early stack. It
was measured after parallel execution, pool batching, and pipelined finalization
had changed phase overlap.

**Checks.** Dictionary tests compare batched roots with sequential roots, and
integration runs verify the exact four-transaction pipeline and accepted block
roots.

**Primary code.** `validator/impl/collator.cpp` and
`crypto/vm/dict.cpp`.

### 11.5 Early block BOC, collated BOC, and file hash

**Observed problem.** Once a root became immutable, independent serialization
and hashing work still occurred serially late in collation.

**Implementation.** The collator starts block BOC serialization after the block
root becomes immutable while collated-data construction continues. Later stages
similarly start the collated-data BOC and block file hash as soon as their exact
inputs are immutable. Results and errors are joined before candidate completion.

**Measurement.** The first isolated early block BOC step reduced 214.346 to
208.259 ms, saving **6.087 ms**. Later early BOC and file-hash work is included
in the H79 and H88 cumulative gains.

**Correctness constraint.** Parallelization is only applied to independent
immutable inputs. The produced bytes, root hash, file hash, and error behavior
must remain deterministic.

**Primary code.** `crypto/vm/boc.cpp`, `crypto/vm/boc.h`, and candidate
finalization in `validator/impl/collator.cpp`.

### 11.6 Analytical estimator and asynchronous final `ShardAccounts`

**Observed problem.** The estimator still performed work proportional to
materialized dictionary/proof mutation, and the final account dictionary had to
be reconstructed after execution.

**Analytical estimator.** `estimate_replacement_proof_increment` traverses old
trie paths for sorted current and previous key sets and calculates exact cells,
bits, internal references, and external references. It was first run in shadow
mode and compared against a fully materialized proof before becoming active.

**Final dictionary worker.** A separate worker batches final account updates,
keeps only the last update for each address, performs sorted `multiset` commits,
and records exact loaded-cell evidence.

The worker performed about 78 ms of CPU work in representative blocks, while
the final join exposed only about 1.9 ms. The remaining expensive operation was
transferring its usage evidence back to the canonical state usage tree; later
rebind optimizations target that operation.

**Measurement.** The cumulative H20 to H38 change reduced 208.259 to 201.781 ms,
saving **6.478 ms**. This number groups the analytical estimator, async final
dictionary, and final-dictionary batching because no clean isolated long run
separates them.

**Primary code.** `crypto/vm/dict.cpp`, `crypto/vm/dict.h`,
`crypto/vm/cells/CellUsageTree.*`, and `validator/impl/collator.cpp`.

### 11.7 Parallel account and storage preparation

**Observed problem.** Before TVM execution, independent accounts underwent
read-only dictionary lookup, unpacking, and storage-stat preparation serially.

**Implementation.** Independent account preparation runs through a persistent
worker executor. The mutable collator state is updated only during deterministic
commit. Storage proof statistics are accumulated locally and merged in a fixed
order, avoiding concurrent mutation of the canonical `CellUsageTree`.

**Measurement.** Parallel account preparation reached 195.586 ms in H40. Adding
parallel storage preparation reached 190.198 ms in H44. Relative to H38, the
grouped saving was **11.583 ms**.

**Correctness constraint.** Worker jobs may read immutable snapshots and produce
private results. They may not allocate global logical times, mutate shared
message queues, or commit account state out of order.

**Primary code.** `validator/impl/collator-impl.h`,
`validator/impl/collator.cpp`, and storage-stat support.

### 11.8 Deterministic parallel transaction waves and account blocks

**Observed problem.** The 10,000-wallet workload has high account independence,
but transaction execution was mostly serial. A naive parallel loop would be
incorrect because transactions share global logical-time assignment, generated
message order, block limits, value flow, and account dependencies.

**Implementation.** A persistent `WaveExecutor` computes independent account
work in bounded waves. The actor/collator thread participates in work. Results
are committed in deterministic order, preserving account dependencies and
global ordering. Per-account `AccountBlock` values are also built independently
and then inserted into the block dictionary in sorted order.

**Measurement.** Transaction waves reduced H44's 190.198 ms to 179.389 ms.
Parallel account-block construction then reduced it to 176.231 ms. The grouped
saving was **13.967 ms**.

**Checks.** Integration runs preserved exact TX1 through TX4 counts, no aborts,
comparable transaction density, and empty final queues. Unit tests cover shared
cell-tree concurrency used by the worker paths.

**Primary code.** `WaveExecutor` in `validator/impl/collator-impl.h` and the
parallel execution/commit stages in `validator/impl/collator.cpp`.

### 11.9 Batched external-pool delivery

**Observed problem.** External messages entered the collator through per-message
actor queue wakeups and handoffs. At 380 requests/s this inflated both inbound
external handling and `new_messages` scheduling work.

**Implementation.** `ExtMsgQueueBatch` carries vectors of external entries.
Existing snapshot delivery is batched, and live arrivals are flushed when either
32 entries accumulate or 80 ms elapse. Priority information and exact message
objects are preserved. Exact-ancestor filtering runs at this boundary.

**Measurement.** H68 measured 138.277 ms and the conservative H68b repeat
measured 141.554 ms, versus H54b's 176.231 ms. The accepted representative gain
was **34.677 ms**, or 19.7% for the step.

Inbound-external time fell from about 32.05 ms to around 21 ms, while the broad
`new_messages` phase fell from about 54.17 ms to around 28 ms. The latter includes
candidate scheduling effects, so not all of its delta should be attributed to
queue push overhead alone.

Both runs delivered the exact offered workload and had no collation deadline
events. H68 drained to queue depth zero. H68b began its steady window at queue
depth 205 and ended at 3, so it removed the backlog and was effectively drained,
but its recorded final value was not literally zero.

**Primary code.** `validator/interfaces/validator-manager.h`,
`validator/impl/ext-message-pool.cpp`, and external intake in
`validator/impl/collator.cpp`.

### 11.10 Worker scaling and parallel final-account rebind

**Observed problem.** After transaction phases became parallel, final account
dictionary rebind and other background work occupied a larger fraction of the
critical path.

**Implementation.** The execution worker pool was first increased from 8 to 16.
Final-account usage-tree traversal was then allowed to split independent branches
across the bounded executor. Later H87/H88 increased execution to 32 workers on
the 32-hardware-thread test host.

**Measurement.** The first grouped worker/rebind step reduced 141.554 to 133.551
ms, saving **8.003 ms**. The later 32-worker change is grouped with proof-backed
rebind in H88 because the cumulative run changed both mechanisms.

**Production concern.** Worker count is not a protocol constant. More threads
can increase cache misses and contention, particularly when validator actors,
RocksDB, networking, and validation execute concurrently on the same host.

### 11.11 Pipelined state finalization

**Observed problem.** Final state proof, Merkle update, candidate proofs, BOCs,
and hashing had independent portions but were scheduled mostly in series.

**Implementation.** This group introduced:

- direct access to the exact Merkle usage node, avoiding redundant root/path
  lookup;
- an immutable proof-usage snapshot;
- background construction of old/new Merkle-update proofs while candidate proofs
  are prepared;
- early start of the collated state proof when its proof-stat snapshot is ready;
- early collated-data BOC construction;
- overlap of independent candidate BOCs and hashes.

Every background result is joined before the candidate leaves the collator. A
worker failure is propagated as a candidate error rather than silently falling
back to incomplete data.

**Measurement.** H70 to H79 reduced mean collation from 133.551 to 124.757 ms,
saving **8.794 ms**.

**How to read phase times.** Once stages overlap, individual phase wall times
cannot be summed to reconstruct total collation. Some phase timers represent
work performed concurrently with another phase. Total `work_real_s.total` is the
authoritative critical-path metric.

**Primary code.** `crypto/vm/cells/MerkleUpdate.*`, usage-tree support,
`crypto/vm/boc.*`, and finalization logic in `validator/impl/collator.cpp`.

### 11.12 Proof-backed final-account rebind and early file hash

**Observed problem.** The async final account dictionary had the correct content
root but belonged to a worker `CellUsageTree`. The collator needed the same cells
wrapped against the canonical state usage tree so that collated-data proof
accounting remained exact. Reconstructing this mapping consumed approximately
15 ms at H79.

**Implementation.** The proof-backed rebind imports loaded paths into the target
tree, maps source nodes to canonical target nodes, rewraps worker `UsageCell`
objects using that mapping, and adds the worker's exact loaded proof statistics.
Traversal can run in bounded parallel branches. Block file hashing starts early
once its input BOC is immutable.

**Measurement.** Rebind fell to approximately 7.18 ms in H85 and 6.2 ms in H88.
The broader combine-account stage fell from about 18.57 to 10.14 ms. Together
with 32-worker tuning, H79 to repeated H88 reduced 124.757 to 116.268 ms, saving
**8.489 ms**.

**Correctness constraint.** A content-equal dictionary root is insufficient.
The exact canonical usage evidence must be transferred, or block/collated-data
size accounting changes. H4 demonstrated this failure mode.

**Primary code.** Rebind functions and early final dictionary state in
`validator/impl/collator.cpp`, plus `UsageCell::rebind_tree_node`.

### 11.13 Parallel Merkle proof generation

**Observed problem.** After rebind became cheaper, collated state proof and old
state-update proof generation remained visible on the critical path.

**Implementation.** `MerkleProof::generate_parallel` and
`generate_raw_parallel` accept a bounded task budget. Traversal forks sibling
branches early enough to distribute work, while preserving deterministic child
order and exact pruning semantics. The first scheduler consumed most of its task
budget near leaves and was corrected before acceptance.

H102 parallelized the collated state proof. H103 also parallelized the old-state
proof used by the state update.

**Measurement.** The collated state proof subphase fell from about 22.795 to
8.313 ms, but total collation improved less because the proof already overlapped
other work and additional workers contend for CPU. H98 to H103 reduced total mean
from 108.572 to 104.162 ms, saving **4.410 ms**.

**Checks.** Unit tests compare serial and parallel roots and byte-identical BOC
output for both predicate-driven and usage-tree-driven proof traversal.

**Primary code.** `crypto/vm/cells/MerkleProof.cpp`,
`crypto/vm/cells/MerkleProof.h`, and state-proof call sites in
`validator/impl/collator.cpp`.

### 11.14 Inner proof-backed rebind traversal

**Observed problem.** Path import already created a complete source-to-target
usage-node map, but the inner rebind traversal redundantly reconstructed portions
of the same mapping through repeated path lookup and child creation.

**Implementation.** H104 reuses the precomputed node vector and performs a
bounded deterministic branch traversal. It avoids repeated `create_child` work
without weakening the proof-backed mapping requirement.

**Measurement.** Final-account rebind fell from approximately 6.45 to
3.82-3.93 ms, and combine-account work fell from about 9.45 to 6.99 ms. Total
mean collation improved from 104.162 to 102.072 ms, saving **2.090 ms**.

**Primary code.** `rebind_usage_cells_by_path` in
`validator/impl/collator.cpp`.

### 11.15 Final bounded-task tuning

**Observed problem.** Rebind had reached a roughly 3.8 ms plateau, while the
parallel collated proof still had enough independent branches to use a larger
bounded task budget.

**Implementation.** H105 sets 16 tasks for each of:

- final-account proof-backed rebind;
- pipelined collated state proof;
- old-state proof for the state update.

**Measurement.** Collated state proof fell from approximately 8.57 to
6.63-7.10 ms while state update remained approximately 17.67-17.75 ms. Total
mean collation fell from 102.072 to **98.660 ms**, saving **3.412 ms**.

H106 repeated the complete configuration at **97.771 ms mean**, confirming that
the sub-100 ms result was not a single-run outlier.

Task count 16 was best on this host and workload. Larger task budgets are not
assumed to be universally faster.

## 12. Rejected and negative experiments

Negative results are part of the reproducibility record. They prevent unsafe or
already-disproved ideas from being rediscovered later.

### 12.1 BufferSlice clone elimination

The hypothesis assumed `td::BufferSlice::clone()` copied candidate bytes. Source
inspection showed that it is a shallow refcounted clone; `copy()` performs the
byte copy. No optimization was applied.

### 12.2 Direct final dictionary root reuse without usage evidence

H4 reused a content-correct estimator root without transferring canonical
`CellUsageTree` evidence. Mean block size grew to 2.118 MB, the final queue grew
to 698, and mean collation regressed to 341.7 ms. Root equality alone is not a
valid proof-accounting criterion.

### 12.3 Flat per-account proof reserve

An approximate 200-byte reserve per updated account was considered. Exact
measurement showed roughly 1,272-1,280 bytes/account for this workload. The flat
reserve would materially undercount and was rejected.

### 12.4 Naive parallel transaction loop

Transactions cannot be submitted independently without accounting for account
dependencies, generated-message order, global logical time, value flow, and
block limits. Only compute/commit waves with deterministic ordering were used.

### 12.5 `new_msgs` heap micro-optimization

The broad `new_messages` timer looked expensive, but native profiles did not
identify heap operations as a primary cost. Queue delivery and actor handoffs
were optimized instead and produced a large measured gain.

### 12.6 Lazy, direct, and in-place rebind variants

Several rebind shortcuts either lost exact usage evidence or increased serialized
size and queue pressure. A representative lazy run reached approximately 406 ms,
2.043 MB mean blocks, and a final queue near 891. Only proof-backed canonical
rebind was retained.

### 12.7 Excessive proof task counts and partitioning

More tasks can reduce one proof subphase while increasing CPU contention and
leaving total collation unchanged. H100's partitioned proof improved a local
phase but not the critical path. The final task count was selected from total
ordinary-run collation, not from subphase time alone.

## 13. Final result audit

### 13.1 Final run directories

```text
local/jetton-research/experiments/runs/
  h105-proof-rebind-tasks16-h104-20260807-130104

local/jetton-research/experiments/runs/
  h106-repeat-proof-rebind-tasks16-h105-20260807-130641
```

### 13.2 Final workload integrity

| Integrity check | H105 | H106 |
|---|---:|---:|
| Offered requests | 7,600 | 7,600 |
| Acknowledged requests | 7,600 | 7,600 |
| Workchain transactions | 30,400 | 30,400 |
| Expected 4 tx/request | exact | exact |
| Final queue | 0 | 0 |
| Deadline candidates | 0 | 0 |
| `want_split` | 0 | 0 |
| Complete leader windows | one empty + three productive | one empty + three productive |

The analyzer reports approximately 364.912 and 365.892 steady-window jTPS for
H105 and H106. This is a measurement-window boundary effect in short runs: the
steady interval crops requests and completions at different pipeline phases. It
does not indicate dropped offered load. Exact totals show all 380 requests/s were
accepted, completed four transactions, and drained.

### 13.3 Baseline versus final

| Metric | Baseline | H105 | H106 | Final average | Change from baseline |
|---|---:|---:|---:|---:|---:|
| Mean collation | 332.809 ms | 98.660 | 97.771 | **98.216** | **-70.5%** |
| P95 collation | 444.851 ms | 123.138 | 125.221 | **124.180** | **-72.1%** |
| Mean validation | 125.979 ms | 93.890 | 94.666 | **94.278** | **-25.2%** |
| P95 validation | 174.472 ms | 129.717 | 126.231 | **127.974** | **-26.7%** |
| TX/productive block | 809.117 | 810.815 | 813.037 | **811.926** | +0.3% |
| Mean block size | 1.626 MB | 1.551 | 1.565 | **1.558** | -4.2% |
| P95 block size | 2.018 MB | 1.896 | 1.929 | **1.913** | -5.2% |
| Deadline candidates | 65 | 0 | 0 | **0** | eliminated |
| Final queue | 0 | 0 | 0 | **0** | unchanged |

### 13.4 Binary identity

The final H105/H106 runs used:

| Binary/input | SHA-256 |
|---|---|
| `validator-engine` | `3f4f2ba917ceeca44e3ec4b0293c55350b0139096f56b69a9c4ab60bd20567bf` |
| `jetton-simulator` | `9d9bb6286b8706b3b1487d8d2e2d0fda0ce18d620d7ece31de4f179ab1cc6605` |
| `tonlibjson` | `c791f9bf5af7f4a929ab779dc4dc665b2489175ed04c85b88ffc613380bb3014` |
| `bench-state-gen` | `5226260f9c4fca8a87788fe18a648b5a77f4672de29634150e0e461aa779c638` |

These hashes identify the historical H105/H106 binary. The smaller, cleaned
implementation and its final binary-exact acceptance run are recorded in
section 13.7.

### 13.5 Exact milestone artifact index

All paths below are relative to
`local/jetton-research/experiments/runs/`. The first H38 launch ending in
`201011` failed before measurement; the complete H38 artifact is the `201035`
directory listed here.

| Milestone | Run directory |
|---|---|
| H3b | `h3b-pool-prune-380-20260806-115239` |
| H10 repeat | `h10-repeat-estimator-probes-380-20260806-123321` |
| H11b | `h11b-async-account-estimator-clean-380-20260806-135845` |
| H14 | `h14-batch-msg-descr-380-20260806-145538` |
| H20 | `h20-early-boc-only-380-short-20260806-173815` |
| H38 | `h38-batch32-final-worker-380-short-20260806-201035` |
| H44 | `h44-parallel-storage-prepare-380-short-20260806-211344` |
| H54b | `h54b-parallel-account-blocks-h53-380-short-20260806-225842` |
| H68 | `h68-live-ext-pool-batches-h54-20260807-005800` |
| H68b | `h68b-live-ext-pool-batches-h54-20260807-010331` |
| H70 | `h70-parallel-rebind-16-workers-20260807-071613` |
| H79 | `h79-overlap-candidate-bocs-h78-20260807-082115` |
| H88 repeat | `h88-repeat-32-workers-early-overlap-h87-20260807-092843` |
| H98 | `h98-descriptor-batch256-h88-20260807-114226` |
| H103 | `h103-parallel-old-and-collated-proof8-h98-20260807-124418` |
| H104 | `h104-parallel-fast-rebind8-h103-20260807-125507` |
| H105 | `h105-proof-rebind-tasks16-h104-20260807-130104` |
| H106 repeat | `h106-repeat-proof-rebind-tasks16-h105-20260807-130641` |

### 13.6 Normal and doubled limits, baseline versus optimized

This comparison extends the original normal-limit/doubled-limit table with two
fresh optimized measurements. All four runs use the same generated state,
10,000-wallet pool, `init_mode=0`, one shard, forced-empty first slot, 120-second
load, 10-second warmup, and 30-second drain. The optimized runs use the complete
H105/H106 feature environment and the same final binary hashes recorded above.

| Metric | Baseline single, 190 RPS | Baseline double, 380 RPS | Optimized single, 190 RPS | Optimized double, 380 RPS |
|---|---:|---:|---:|---:|
| jTPS | 190.7 | 379.0 | 190.7 | 380.7 |
| Total TPS | 762.7 | 1,515.3 | 762.6 | 1,522.8 |
| Productive blocks/s | 1.873 | 1.873 | 1.882 | 1.882 |
| Tx/non-empty block | 407.3 | 809.1 | 405.2 | 809.2 |
| Mean block size | 0.837 MB | 1.626 MB | 0.806 MB | 1.549 MB |
| P95 block size | 1.041 MB | 2.018 MB | 1.014 MB | 1.910 MB |
| Mean collation | 184.6 ms | 332.8 ms | **54.2 ms** | **100.4 ms** |
| P95 collation | 239.9 ms | 444.9 ms | **69.8 ms** | **125.7 ms** |
| Mean validation | 67.2 ms | 126.0 ms | **44.0 ms** | **93.4 ms** |
| P95 validation | 95.8 ms | 174.5 ms | **64.9 ms** | **127.1 ms** |

The optimized normal-limit run reduces mean/P95 productive collation by
70.6%/70.9%. The optimized doubled-limit run reduces it by 69.8%/71.7%. Mean
validation improves by 34.5% at normal limits and 25.8% at doubled limits.

The small productive-block-rate difference is a steady-window boundary effect:
the baseline windows contain 206 productive blocks and the optimized windows
contain 207. Transaction density is unchanged at the doubled operating point,
so the collation reduction is not caused by smaller transaction batches. Mean
serialized size is 3.7% lower at normal limits and 4.7% lower at doubled limits.

All acknowledged requests completed TX1 through TX4 with zero aborts. Both new
optimized runs ended with queue depth zero, one shard, no `want_split`, and no
collation-deadline overloads. The fresh optimized run artifacts are:

```text
local/jetton-research/experiments/runs/
  optimized-normal-190-long-20260807-134959

local/jetton-research/experiments/runs/
  optimized-double-380-long-20260807-135723
```

### 13.7 Final-stack cleanup and revalidation

The optimization stack was implemented incrementally, so the final source still
contained rejected alternatives, diagnostics, and switches whose behavior had
later been superseded. A cleanup pass re-audited dependencies and reduced the
optimization commit by **2,749 net lines** across 21 production/test files
(`195` additions and `2,944` deletions relative to the pre-cleanup commit).

The cleanup removed:

- the superseded exact, batched, and asynchronous account-dictionary estimator
  paths; the measured analytical estimator is now the only optimized estimator;
- rejected account-prefetch, dictionary-rebind, partitioned-proof, shared-proof,
  late-serialization, move-candidate, and storage-threshold experiments;
- duplicate switches for behavior now owned by one parent feature;
- experimental counters, warning logs, and tests that exercised removed APIs.

It retained focused correctness tests for analytical proof estimation,
multiset dictionary updates, separate proof accounting, direct usage nodes,
parallel predicate proofs, parallel usage-tree proofs, and concurrent usage-tree
access.

Two parent switches now describe complete pipelines:

- `TON_SIM_EARLY_BLOCK_BOC=1` starts block BOC serialization and computes the
  block file hash and collated BOC from that early result;
- `TON_SIM_PIPELINED_STATE_FINALIZATION=1` enables early proof-backed final
  account rebind and parallel state-proof/Merkle-update construction.

The following matched checks were run against the 45M-account state. The
ablation runs used a 20-second load; the reference and worker-count checks used
30 seconds. All used 10,000 wallets, 380 requests/s, doubled limits, one shard,
and the forced-empty first slot.

| Check | Productive collation mean | P95 | Tx/productive block | Result |
|---|---:|---:|---:|---|
| Pre-cleanup full stack | 100.111 ms | 124.050 ms | 807.217 | reference |
| Without analytical estimator | 164.512 ms | 210.770 ms | 819.857 | retain estimator |
| Without early serialization pipeline | 110.865 ms | 138.767 ms | 819.000 | retain early serialization |
| Cleaned stack, 32 workers | 100.292 ms | 124.751 ms | 812.426 | pass |
| Cleaned stack, 32-worker repeat | 100.742 ms | 126.936 ms | 814.174 | pass |
| Cleaned stack, 16 workers | 101.948 ms | 124.194 ms | 810.809 | retain 32 workers |
| Final binary acceptance, 32 workers | 101.846 ms | 126.124 ms | 812.596 | pass |

The cleaned 32-worker runs contain 5-7 more transactions per productive block
than the pre-cleanup reference. Pooling all 140 productive blocks from the three
cleaned 32-worker runs gives **100.961 ms/block**, **813.057 tx/block**, and
**0.12418 ms/transaction**. The pre-cleanup reference is **100.111 ms/block**,
**807.217 tx/block**, and **0.12402 ms/transaction**. The normalized difference
is 0.12%, below run-to-run noise. Across all steady candidates, including the
deliberately empty first slots, the cleaned means are 75.414, 75.336, and 76.514
ms. All three runs drained the queue and had no deadline overload; sampled
same-block TX1-through-TX4 completion was 100%, 100%, and 99%.

This revalidation does not replace the historical H105/H106 observation of
98.216 ms. It shows that the smaller implementation preserves the same
approximately-100-ms operating point within run-to-run block-composition
variance. The strict `<100 ms` productive-block mean was not stable even before
cleanup: the duration-matched pre-cleanup run measured 100.373 ms. The robust
claim is therefore approximately 100 ms and approximately 0.124 ms per
transaction, not that every short run must fall below an exact decimal boundary.

The binary-exact acceptance run used:

| Binary/input | SHA-256 |
|---|---|
| `validator-engine` | `20acf5a9927150a6a9e1ef7513b1dbd446a23768865f5815a77bc4f54e439dcb` |
| `jetton-simulator` | `fc737ec367ff5a6861a46ae6567bda84ea9f8af02f7f354b3da6fea7ae62cbd2` |
| `tonlibjson` | `c791f9bf5af7f4a929ab779dc4dc665b2489175ed04c85b88ffc613380bb3014` |
| `bench-state-gen` | `5226260f9c4fca8a87788fe18a648b5a77f4672de29634150e0e461aa779c638` |

Cleanup artifacts:

```text
local/jetton-research/experiments/runs/
  cleanup-reference-20260807-155852
  cleanup-ablate-analytical-20260807-160435
  cleanup-ablate-early-serialization-20260807-161006
  cleanup-final-32w-20260807-20260807-163634
  cleanup-final-32w-repeat-20260807-20260807-164802
  cleanup-final-16w-20260807-20260807-164205
  cleanup-acceptance-final-20260807-20260807-165706
```

## 14. Final feature configuration

After cleanup, the complete stack uses 19 settings instead of the previous 31.
They are explicitly passed to the node and recorded in each run's `run.json`:

```text
TON_SIM_FILTER_ANCESTOR_EXTERNALS=1
TON_SIM_ED25519_CHKSIG_CACHE=1
TON_SIM_BATCH_MESSAGE_DESCRIPTORS=1
TON_SIM_MESSAGE_DESCRIPTOR_BATCH_SIZE=256
TON_SIM_EARLY_BLOCK_BOC=1
TON_SIM_ANALYTICAL_ACCOUNT_DICT_ESTIMATOR=1
TON_SIM_ASYNC_FINAL_ACCOUNT_DICT=1
TON_SIM_PARALLEL_ACCOUNT_PREPARE=1
TON_SIM_PARALLEL_STORAGE_PREPARE=1
TON_SIM_PARALLEL_EXECUTION=1
TON_SIM_EXECUTION_THREADS=32
TON_SIM_PARALLEL_ACCOUNT_BLOCKS=1
TON_SIM_BATCH_EXT_POOL_DELIVERY=1
TON_SIM_EXT_POOL_BATCH_SIZE=32
TON_SIM_EXT_POOL_BATCH_DELAY_MS=80
TON_SIM_PIPELINED_STATE_FINALIZATION=1
TON_SIM_FINAL_ACCOUNT_REBIND_TASKS=16
TON_SIM_PIPELINED_STATE_PROOF_TASKS=16
TON_SIM_STATE_UPDATE_OLD_PROOF_TASKS=16
```

These are research switches. Their presence makes experiments bisectable and
keeps normal node behavior unchanged, but production work should consolidate
them into reviewed configuration or safe defaults after each feature is accepted.

## 15. Reproduction procedure

### 15.1 Prerequisites

1. Use the `fast-collators` branch based on the current `testnet` branch.
2. Ensure `_local/` is ignored by Git; never checkpoint the 169 GB cell database.
3. Have at least 200 GB free for generation plus headroom for RocksDB temporary
   files and experiment runs.
4. Use the same host power/performance policy or record the difference.
5. Stop unrelated CPU- and disk-intensive jobs.
6. Record the kernel, compiler, CPU topology, and binary hashes.

### 15.2 Build

Use the repository-supported Ubuntu shared build script as the source of build
configuration:

```bash
cd /home/vallas/ton
bash assembly/native/build-ubuntu-shared.sh
cmake --build build --target \
  validator-engine jetton-simulator bench-state-gen \
  test-cells test-smartcont test-vm -j8
```

The recorded final runs used the already configured Release directory
`build-clang-release`. To compare against them without changing paths:

```bash
cmake --build build-clang-release --target \
  validator-engine jetton-simulator bench-state-gen \
  test-cells test-smartcont test-vm -j8
```

After building, record:

```bash
sha256sum \
  build-clang-release/validator-engine/validator-engine \
  build-clang-release/local/jetton-research/simulator/jetton-simulator \
  build-clang-release/tonlib/libtonlibjson.so \
  build-clang-release/benchmark/bench-state-gen
```

If target paths differ in a clean build, obtain the exact paths with:

```bash
find build-clang-release -type f \
  \( -name validator-engine -o -name jetton-simulator \
     -o -name bench-state-gen -o -name 'libtonlibjson.so*' \)
```

### 15.3 Unit and simulator tests

Run before any performance experiment:

```bash
build-clang-release/test-cells
build-clang-release/test-smartcont
build-clang-release/test-vm

build-clang-release/local/jetton-research/simulator/jetton-simulator self-test
```

The accepted research state passed:

- 23 cell/dictionary/Merkle tests;
- 10 smart-contract tests;
- 34 VM tests;
- the jetton simulator exact wallet/body/signature/TL-B/BOC self-test.

Relevant cell tests cover:

- batched `multiset` versus sequential dictionary roots;
- multi-key lookup and proof preservation;
- analytical estimate versus materialized exact proof;
- independent usage-tree accounting;
- `UsageCell` rebind;
- auxiliary paths and Merkle update;
- direct usage node versus legacy path lookup;
- shared old-state proof;
- serial versus parallel proof root and byte-identical BOC;
- concurrent usage-tree child creation and load.

### 15.4 Generate or verify the state

If the state already exists, verify the manifest first:

```bash
sha256sum _local/jetton-research/generated-db-45m/state/manifest.json
jq . _local/jetton-research/generated-db-45m/state/manifest.json
```

Expected manifest SHA-256:

```text
17b701a6193be8169afc43d737804fc1de216b81cbd30b87f061b47aa3916aa5
```

If it must be regenerated, use the exact command in section 5.2. Do not begin a
performance comparison until root and file hashes match the expected values.

### 15.5 Reproduce the baseline behavior

With all research feature flags absent:

```bash
PYTHONPATH=test/tontester/src .venv/bin/python \
  local/jetton-research/experiments/run_fixed.py \
  --name reproduce-baseline \
  --build-dir build-clang-release \
  --rate 380 \
  --block-limit-mul 2 \
  --gas-limit-mul 2 \
  --duration 120 \
  --warmup 10 \
  --drain 30 \
  --base-port 5000
```

Before accepting the run, verify approximately 809-811 transactions per
productive block, the one-empty/three-productive pattern, all four transaction
stages, one shard, and final queue zero.

### 15.6 Reproduce the final configuration

```bash
cd /home/vallas/ton

final_env=(
  TON_SIM_FILTER_ANCESTOR_EXTERNALS=1
  TON_SIM_ED25519_CHKSIG_CACHE=1
  TON_SIM_BATCH_MESSAGE_DESCRIPTORS=1
  TON_SIM_MESSAGE_DESCRIPTOR_BATCH_SIZE=256
  TON_SIM_EARLY_BLOCK_BOC=1
  TON_SIM_ANALYTICAL_ACCOUNT_DICT_ESTIMATOR=1
  TON_SIM_ASYNC_FINAL_ACCOUNT_DICT=1
  TON_SIM_PARALLEL_ACCOUNT_PREPARE=1
  TON_SIM_PARALLEL_STORAGE_PREPARE=1
  TON_SIM_PARALLEL_EXECUTION=1
  TON_SIM_EXECUTION_THREADS=32
  TON_SIM_PARALLEL_ACCOUNT_BLOCKS=1
  TON_SIM_BATCH_EXT_POOL_DELIVERY=1
  TON_SIM_EXT_POOL_BATCH_SIZE=32
  TON_SIM_EXT_POOL_BATCH_DELAY_MS=80
  TON_SIM_PIPELINED_STATE_FINALIZATION=1
  TON_SIM_FINAL_ACCOUNT_REBIND_TASKS=16
  TON_SIM_PIPELINED_STATE_PROOF_TASKS=16
  TON_SIM_STATE_UPDATE_OLD_PROOF_TASKS=16
)

cmd=(
  .venv/bin/python local/jetton-research/experiments/run_fixed.py
  --name reproduce-final
  --build-dir build-clang-release
  --rate 380
  --block-limit-mul 2
  --gas-limit-mul 2
  --duration 20
  --warmup 5
  --drain 12
  --base-port 5100
)

for value in "${final_env[@]}"; do
  cmd+=(--node-env "$value")
done

PYTHONPATH=test/tontester/src "${cmd[@]}"
```

Run at least twice. For a production-quality performance claim, replace the
20-second duration with a 5- to 10-minute minimum and repeat after process and
host restarts.

### 15.7 Compare accepted productive blocks

```bash
BASELINE=local/jetton-research/simulator/runs/\
45m-one-shard-380rps-limits2x-force-empty-first-10k-20260805

FINAL=local/jetton-research/experiments/runs/\
h106-repeat-proof-rebind-tasks16-h105-20260807-130641

.venv/bin/python local/jetton-research/experiments/compare_runs.py \
  "$BASELINE" "$FINAL"
```

Then inspect, at minimum:

```bash
jq '{throughput, collation}' "$FINAL/analysis.json"
jq '{spam_command, node_environment: .topology.node_environment,
     external_state}' "$FINAL/run.json"
tail -n 100 "$FINAL/network/node1/log"
```

Do not accept a lower mean if it is explained by smaller blocks, extra empty
slots, an undrained queue, a shard split, missing TX3/TX4 transactions, or a
different state manifest.

## 16. Source and test map

| Area | Main source locations |
|---|---|
| Feature switches and collation pipeline | `validator/impl/collator.cpp` |
| Worker executor and async state | `validator/impl/collator-impl.h` |
| External delivery batching | `validator/impl/ext-message-pool.cpp` |
| External queue batch API | `validator/interfaces/validator-manager.h` |
| Collation telemetry | `validator/manager.cpp` and validator-manager interfaces |
| Signature cache | `crypto/vm/tonops.cpp` |
| Batched dictionaries and analytical estimator | `crypto/vm/dict.cpp`, `crypto/vm/dict.h` |
| Usage tree and rebind support | `crypto/vm/cells/CellUsageTree.*`, `UsageCell.h` |
| Parallel Merkle proofs | `crypto/vm/cells/MerkleProof.*` |
| Merkle update pipeline | `crypto/vm/cells/MerkleUpdate.*` |
| Early/parallel BOC work | `crypto/vm/boc.*` |
| Cell and proof tests | `crypto/test/test-cells.cpp` |
| Simulator contracts/load | `local/jetton-research/simulator` |
| Fixed runner and comparison | `local/jetton-research/experiments` |

## 17. Production-readiness plan

The following gates should be completed before enabling these changes in a
production validator.

### 17.1 Source provenance and review

1. Commit the exact research diff on a dedicated branch.
2. Split the work into reviewable changes with one correctness property per PR.
3. Preserve a cumulative integration branch for performance comparisons.
4. Rebuild from a clean checkout and store compiler, link, and binary hashes.
5. Remove abandoned experiment code paths and reduce feature-flag combinations.

### 17.2 Determinism and protocol correctness

1. Run multiple validators and prove candidate root, file hash, collated-data
   hash, and serialized BOCs are identical with optimization on and off.
2. Repeat on different CPU counts and at least two architectures.
3. Exercise forks to prove exact-ancestor external filtering never removes a
   message needed by a competing branch.
4. Test generated-message dependencies and heavily contended accounts, not only
   mostly independent wallet pairs.
5. Test default block/gas limits separately from the `2x` research limits.

### 17.3 Concurrency safety

1. Run ASan, UBSan, and TSan where practical.
2. Stress concurrent `CellUsageTree` load and child creation.
3. Inject worker failures and verify errors are joined and propagated.
4. Run long soaks and check thread, queue, and cache memory bounds.
5. Verify shutdown and candidate cancellation join every background task.

### 17.4 Workload coverage

1. Use independent sender keys and realistic key-cardinality distributions.
2. Add invalid signatures, replays, expired seqnos, and rejected externals.
3. Add custom jetton contracts and varying StateInit/code/data sizes.
4. Add already-deployed and first-deployment recipient mixtures.
5. Add hot recipient accounts and dependency-heavy transaction chains.
6. Add real mainnet-state snapshots where operationally permitted.
7. Test same-shard and cross-shard transfers separately.

### 17.5 Operational validation

1. Run 5- to 10-minute minimum repetitions, then hour-scale soaks.
2. Repeat from cold and warm OS/RocksDB caches.
3. Measure CPU utilization, per-thread saturation, memory, I/O, and cache misses.
4. Add multi-validator candidate propagation and validation under network delay.
5. Measure database growth, block propagation cost, and DoS exposure from `2x`
   block limits.
6. Confirm no increase in missed slots while validation, networking, and storage
   compete with the collator.

## 18. Conclusions

The original 332.809 ms collation time was not dominated by one TVM operation.
It was the sum of repeated external work, repeated dictionary path construction,
serial preparation/execution, actor handoff overhead, exact proof-accounting
work, usage-tree transfer, and late serial finalization.

The successful strategy was cumulative:

1. Remove work that should not be repeated.
2. Batch structurally similar dictionary and queue operations.
3. Move exact accounting to workers without approximating protocol limits.
4. Parallelize only account work with explicit independence and deterministic
   commit.
5. Preserve canonical usage evidence when transferring worker-produced state.
6. Start immutable proof, hash, and serialization work as early as dependencies
   permit.
7. Optimize the critical path after every bottleneck shift, not a stale profile.

Under the fixed 45M-account, 10k-wallet, 380 request/s, one-shard, `2x`-limit
workload, those changes reduced productive-block collation from **332.809 ms to
98.216 ms average across the final two runs**, while preserving approximately
812 transactions per productive block, exact four-transaction completion, one
shard, and an empty final queue. The later duration-matched 120-second run
measured 100.373 ms at 809.198 transactions per productive block, confirming a
69.8% long-run reduction with the same final stack.

That is the result to carry forward. The next engineering phase is to turn the
remaining feature-flagged stack into a reviewable production configuration and
prove the same correctness and latency properties under multi-validator
production conditions. The cleanup pass has already removed 2,749 net lines of
superseded research code, reduced the active settings from 31 to 19, and
revalidated equivalent per-transaction collation cost on the same workload.
