# Jetton collation optimization experiments

This directory is the reproducible ledger for one-shard collation optimization
work. The simulator remains in `../simulator`; this directory records hypotheses,
patches, profiles, and comparable results.

## Controlled baseline

The baseline is the completed `2x` run from 2026-08-05:

- generated 45M-account basechain state;
- 10,000 WalletSpam senders, JettonWallet `init_mode=0`;
- one shard, one validator, Simplex v2, 400 ms target slots;
- doubled block and gas limits;
- first slot in every four-slot leader window forced empty;
- 380 requests/s for 120 seconds, 10-second warmup, 30-second drain.

Authoritative result:
`../simulator/runs/45m-one-shard-380rps-limits2x-force-empty-first-10k-20260805/analysis.json`

| Metric | Baseline |
|---|---:|
| Jetton transfers/s | 378.976 |
| Transactions/s | 1515.303 |
| Transactions/non-empty block | 809.117 |
| Mean/P95 non-empty collation | 332.809 / 444.851 ms |
| Mean/P95 non-empty validation | 125.979 / 174.473 ms |
| Mean/P95 non-empty block bytes | 1,626,335 / 2,017,707 |
| Productive blocks/s | 1.873 |
| Deadline-limited candidates | 65 |

The forced schedule is part of the benchmark invariant: slot 0 must be empty and
slots 1-3 must be non-empty. Throughput comparisons are invalid if this changes.

## Acceptance gates

An optimization is successful only when a no-profiler run shows:

1. unchanged transaction semantics and all four jetton stages draining;
2. no extra empty slots, shard split, or persistent message-queue growth;
3. at least two consistent measurements against the same workload;
4. lower non-empty collation mean and tail, or a higher stable request-rate ceiling;
5. no material validation-time or serialized-size regression unless explicitly justified.

## Hypothesis ledger

| ID | Hypothesis | Evidence and constraint | Status |
|---|---|---|---|
| P0 | Native sampling will identify costs hidden inside broad phases | `perf` shows SHA-256 6.69%, `DataCell::create` 4.75%, Ed25519 field arithmetic 7.61%, `CellSlice::prefetch_ref` 2.85%, `memmove` 1.95%, and cell destruction 1.86%. The profile covers the whole validator process; wallet Ed25519 verification occurs in admission, collation, and validation. | Complete |
| H1 | Avoid redundant candidate-buffer copies | `td::BufferSlice::clone()` is refcounted and shallow; only `copy()` copies bytes. The original premise was false. | Rejected before benchmark |
| H2 | Serialize block and collated-data BOCs concurrently | The two BOCs are independent after proof construction, but candidate serialization is only about 6% of productive collation CPU. Scheduler overhead and deterministic error handling must be measured. | Candidate, medium priority |
| H3a | Do not execute externals already applied on the candidate's exact branch | Release smoke: 12,094 branch-local stale externals skipped, zero rejected transactions, 374.945 jTPS / 1499.579 TPS, 803.3 tx/productive block, 310.9/409.0 ms mean/P95 collation. The filter is exact-ancestor-only and therefore fork-safe. | Successful semantics; one timing run |
| H3b | Prune exact-ancestor externals at pool delivery instead of in the collator loop | Full Release run: 59,498 stale externals removed from private pool snapshots, zero transaction rejects in productive blocks, 377.500 jTPS / 1510.000 TPS, 814.2 tx/productive block, and 306.6/421.5 ms mean/P95 collation. Versus the shorter H3a smoke, mean fell 4.3 ms and inbound-external cost fell 9.5% per transaction, but the tail did not improve. | Successful semantics; modest result, one full timing run |
| H4 | Reuse the estimator root for final `ShardAccounts` | Content roots can match, but usage evidence belongs to a particular `CellUsageTree`. H4a reused the root without transferring canonical usage: mean block size became 2.118 MB, the final queue was 698, and collation regressed to 341.7/469.2 ms. | Rejected implementation; valid only with exact usage-proof transfer |
| H5 | Replace repeated dictionary updates with a bulk sorted mutation | Added `AugmentedDictionary::multiset` with sequential-root equivalence tests. H5a produced 267.7/366.8 ms and H5b 258.8/346.9 ms, but H5b's measured final update was only 4.5 ms and its larger run-level delta was not independently attributable. The primitive is retained for H11 and final batching. | Correct primitive; standalone timing not accepted |
| H5c | Replace exact account proof accounting with a flat conservative reserve | Exact telemetry measured 181,418,810 estimator-proof bytes over 142,613 updates: 1,272 bytes/update and 885 KB/productive block. H11 repeated about 1,280 bytes/update. The historical 200-byte/account estimate would undercount by hundreds of KB/block. | Rejected; exact accounting required |
| H6 | Parallelize independent account transaction chains | The 10k-wallet load has substantial account independence, but global LT ordering, generated-message dependencies, block limits, value flow, and deterministic dictionary commit are shared. Requires compute/commit separation, not a loop-level thread pool. | Architectural candidate |
| H7 | Cache reusable transaction inputs across blocks | Account objects are collator-local; wallet code and immutable contract structures repeat heavily. Cache keys must include content hashes and never reuse mutable account state. RocksDB/cell caches may already capture most benefit. | Candidate, profile first |
| H8 | Reduce transient cell allocation and hashing | `DataCell` creation/destruction plus SHA-256 are the largest confirmed collator-adjacent native costs. A per-collation arena or fewer intermediate cells may help; global cell semantics must remain unchanged. | Candidate, source attribution needed |
| H9 | Optimize the `new_msgs` heap | The heap processes roughly three internal jetton messages per accepted request, but native profiling did not identify heap operations as a top cost. | Deprioritized |
| H10 | Reuse successful `CHKSIGNU` verification across admission, collation, and validation | The opcode result is a pure function of an exact 32-byte hash, 64-byte signature, and 32-byte key. A bounded sharded cache stores only successful results and checks all 128 bytes on a hit; invalid signatures and `CHKSIGNS` are not cached. Two full runs produced 283.7/382.7 and 281.1/384.5 ms mean/P95 collation versus H3b's 306.6/421.5 ms. Productive TVM fell 35.4 -> 12.8-12.9 ms/block; validation fell 119.8/167.5 -> 100.6-101.1/136.5-141.4 ms. Cache telemetry was 65.8% hits. Release validator, `test-smartcont` (10 tests), and `test-vm` (34 tests) pass with the flag enabled. | Accepted, repeated full-run improvement |
| H11 | Run exact `ShardAccounts` size estimation concurrently with transaction execution | A dedicated worker owns an estimator dictionary and separate usage tree. The collator joins it at the unchanged 16-operation proof barriers, then accounts the exact proof. Two full runs produced 217.5/286.6 and 228.8/305.8 ms mean/P95 versus H10's 281.1/384.5 ms: 18.6-22.6% lower mean and 20.5-25.5% lower P95. Both sustained 809 tx/productive block and 1.591-1.592 MB mean blocks; validation stayed at 96.1-98.8 ms. Worker CPU was 84.6-89.9 ms/block, but critical-path wait was only 15.0-18.0 ms. Exact independent-tree proof tests and full TX1-TX4/drain checks pass. | Accepted, repeated significant improvement |

## Current H3 evidence

The first H3 implementation filters in `Collator::process_inbound_external_messages`.
It fixes the expensive correctness/performance failure where an external included in
an unfinalized ancestor is re-executed by the next collator, but it still receives
that external from `ExtMessagePool`.

H3b passes the sorted exact-ancestor normalized hashes through `ExtMsgCallback`.
`ExtMessagePool` uses `ext_messages_hashes_norm_` to erase matching raw variants
from the callback's private persistent-treap snapshot and filters matching live
arrivals. It does not erase or postpone the global entry. Finalized-block cleanup
remains the only global deletion path, so a competing fork can still receive the
message. `ext_msgs_pool_filtered` records the work avoided at this boundary.

Rejected hypotheses and correctness failures stay in this table with their measured
result; they are not removed from the history.

## Profiling

`profile_fixed.py` launches a shortened copy of the controlled baseline and attaches
`perf` only when the spam phase starts. Profiling results locate native hot paths;
throughput claims always come from ordinary runs without `perf`.

```bash
PYTHONPATH=test/tontester/src .venv/bin/python \
  local/jetton-research/experiments/profile_fixed.py \
  --run-dir local/jetton-research/experiments/runs/p0-native-profile
```

Comparable no-profiler experiments use `run_fixed.py`; validator-only feature
switches are passed explicitly and recorded in `run.json`:

```bash
PYTHONPATH=test/tontester/src .venv/bin/python \
  local/jetton-research/experiments/run_fixed.py --name h1-copy-elision \
  --node-env TON_SIM_EXAMPLE=1
```
