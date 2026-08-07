# Collation optimization research

This document records the code-level model used to choose experiments. Numerical
claims come from ordinary Release runs of the controlled `x2/380` workload in
`README.md`; profiler runs are used only for attribution.

## Current sub-100 ms result

The fixed 45M-state, 10k-wallet, one-shard `x2/380` workload now has two
identical-configuration runs below 100 ms mean productive-block collation:

| Run | Mean/P95 collation | Mean/P95 validation | Transactions/block | Mean/P95 block size |
|---|---:|---:|---:|---:|
| H103, proof tasks 8 | 104.162 / 128.601 ms | 96.650 / 128.167 ms | 809.037 | 1.564 / 1.937 MB |
| H104, parallel proof-backed rebind | 102.072 / 125.520 ms | 97.120 / 136.283 ms | 816.000 | 1.560 / 1.882 MB |
| H105, proof and rebind tasks 16 | 98.660 / 123.138 ms | 93.890 / 129.717 ms | 810.815 | 1.551 / 1.896 MB |
| H106, identical H105 repeat | 97.771 / 125.221 ms | 94.666 / 126.231 ms | 813.037 | 1.565 / 1.929 MB |

H104 makes the proof-backed final-account rebind reuse the complete
source-to-target node map produced while importing worker usage paths. It then
rebuilds independent trie branches with a bounded, deterministic result-order
parallel traversal. On blocks with more than 500 transactions, final-account
rebind fell from 6.45 to 3.82 ms and `combine_account_transactions` fell from
9.45 to 6.99 ms.

H105 and H106 use 16 bounded tasks for final-account rebind, the pipelined
collated-state proof, and the old-state side of the Merkle update. Rebind had
already reached its scaling limit, but collated-state proof wall time fell from
8.57 ms in H104 to 6.63 and 7.10 ms. The state Merkle update remained stable at
17.67-17.75 ms.

Both sub-100 runs offered and acknowledged 7,600 requests with no send errors.
Each produced exactly 7,600 successful TX1, TX2, TX3, and TX4 transactions
(30,400 total), no aborts, no missing or ambiguous collation joins, no deadline
overloads, no `want_split`, and a zero final queue. Every complete steady leader
window contained one empty block followed by three productive blocks. Both used
the same validator binary and node environment. `test-cells` passes all 23 tests,
including exact serial/parallel Merkle proof root and BOC comparisons.

Authoritative run directories:

- `runs/h105-proof-rebind-tasks16-h104-20260807-130104`
- `runs/h106-repeat-proof-rebind-tasks16-h105-20260807-130641`

## Productive-block budget

The table compares steady, non-empty basechain blocks. H10 includes H3b. H11
includes H10, bulk final updates, and the asynchronous exact estimator.

| Cost | H10 repeat | H5b | H11 run 1 | H11 run 2 |
|---|---:|---:|---:|---:|
| Transactions/block | 809.686 | 809.681 | 809.252 | 809.643 |
| Collation mean | 281.077 ms | 258.823 ms | 217.516 ms | 228.820 ms |
| Collation P95 | 384.457 ms | 346.938 ms | 286.634 ms | 305.796 ms |
| Estimator worker CPU | n/a | n/a | 84.619 ms | 89.907 ms |
| Collator estimator wait | n/a | n/a | 14.992 ms | 17.965 ms |
| Canonical final dictionary update | n/a | 4.521 ms | 18.223 ms | 18.563 ms |
| Validation mean/P95 | 100.608 / 136.509 ms | 93.640 / 136.805 ms | 98.836 / 136.234 ms | 96.078 / 133.061 ms |
| Mean block size | 1.594 MB | 1.592 MB | 1.591 MB | 1.592 MB |

Against the repeated H10 control, H11 reduces productive collation mean by
18.6-22.6% and P95 by 20.5-25.5%. The worker still performs the full exact trie
calculation. The gain comes from overlapping 85-90 ms of worker CPU with
transaction execution; only 15-18 ms remains on the collator's critical path.
The two H11 runs had concurrent background experiments on this 32-core host, so
host isolation is imperfect, but the direction and size of the repeated result
are consistent.

## Hot path from an external to a candidate

1. `ExtMessagePool::get_messages` creates the collator's private persistent queue.
   H3b removes exact-ancestor messages here while retaining them globally for forks.
2. `Collator::process_inbound_external_messages` pops an external, calls
   `register_external_message`, executes its destination account, and creates the
   `InMsg` descriptor.
3. `Collator::create_ordinary_transaction` obtains the account and delegates to
   `impl_create_ordinary_transaction`.
4. `Transaction` parses the input, performs storage/credit/compute/action/bounce
   phases, serializes the transaction, and computes the new account state.
5. `Transaction::update_limits` adds transaction cells, new-state proof cells,
   gas, logical time, and accounting constants to `BlockLimitStatus`.
6. `Transaction::commit` advances the in-memory `Account` and records the
   transaction root.
7. `Collator::update_account_dict_estimation` sends the first change to each
   account to H11's private estimator worker. The worker owns both its
   `ShardAccounts` clone and a separate `CellUsageTree`. Every 16 transactions the
   collator joins the submitted sequence and adds that exact root and usage proof
   to block-size accounting.
8. `register_new_msgs` puts generated messages into the deterministic priority
   queue. The three internal jetton stages repeat steps 3-7 in
   `process_new_messages`.
9. `combine_account_transactions` serializes one `AccountBlock` per touched
   account and bulk-applies the final `ShardAccounts` changes.
10. `create_shard_state` builds the state and Merkle update.
11. `create_collated_data` scans dictionary differences and generates state,
    queue, and account-storage proofs.
12. `create_block_candidate` independently serializes the block and collated roots
    as BOCs, hashes both byte strings, and constructs `BlockCandidate`.

## Confirmed native costs

The process-wide `perf` profile attributes the largest flat samples to SHA-256
(6.69%), `DataCell::create` (4.75%), Ed25519 field arithmetic (7.61%),
`CellSlice::prefetch_ref` (2.85%), `memmove` (1.95%), and `DataCell` destruction
(1.86%). These support three distinct directions:

- avoid repeated pure cryptography (H10);
- create fewer transient trie/proof cells (H4/H5/H8);
- overlap independent proof and BOC work (H2 and later proof parallelism).

Heap operations are not a leading flat cost, so replacing `new_msgs`' priority
queue is not currently justified.

## Correctness boundaries

- The candidate must remain deterministic for the same branch and inputs.
- External filtering may use only exact ancestors of the candidate branch. Global
  deletion still occurs only after an applied block is known.
- A cryptographic cache hit must compare the complete pure-function input. H10
  compares all 128 bytes and caches only successful `CHKSIGNU` results.
- Transaction chains cannot be naively parallelized. Start/end logical times,
  generated-message ordering, account state, block limits, and value flow are
  sequentially observable by contracts.
- Account-dictionary estimation is consensus-adjacent safety logic. Reducing proof
  refresh frequency without a conservative bound can admit an oversized block.
- `CellUsageTree` is mutable and not thread-safe. The estimator worker must own a
  separate tree, and proof accounting must explicitly consume that tree only after
  a sequence barrier.
- Mutating the canonical account dictionary during transaction execution can alter
  state-usage/proof tracking. Reusing the estimator at finalization is safer than
  replacing the original dictionary early.
- Parallel cell/BOC work must use read-only roots and preserve deterministic root
  and byte ordering. Errors must be joined before returning the candidate.

## Estimator experiments

The H10 probes found that first-change trie mutation costs 89.4 ms/block, while
proof refresh traversal costs only 2.7 ms/block. Changing the proof interval is
therefore low value and unsafe without a conservative pending-size bound.

H5 added `AugmentedDictionary::multiset` and root-equivalence tests. Batched
estimator and final updates are correct, but the apparent H5b run-level gain was
larger than its measured 4.5 ms final-update timer and was not accepted as an
independent result.

H5c measured the exact estimator proof contribution: 181,418,810 bytes across
205 productive blocks and 142,613 updates, or 884,970 bytes/block and 1,272
bytes/update. H11 independently measured about 796 KB/block and 1,280
bytes/update at its slightly lower updates/block. The historical 200-byte/account
heuristic would undercount by hundreds of kilobytes per block, so replacing exact
proof accounting with a flat reserve is rejected.

H11 moves the exact estimator to a worker and waits at the unchanged 16-operation
proof barriers. A regression test constructs identical augmented dictionaries
under independent usage trees and compares root hashes plus proof cells, bits,
internal refs, and external refs. Both full runs completed with identical block
size and transaction semantics.

Directly reusing the worker root for final state is not yet valid. Its usage tree
does not populate the canonical collated-data proof. H4a demonstrated the danger:
mean block size rose from about 1.59 MB to 2.12 MB, the queue ended at 698, and
collation regressed to 341.7/469.2 ms. A future root-reuse design must transfer or
merge exact usage evidence, not merely compare content hashes.

## Next experiments

### H12: transfer worker usage evidence

Design a deterministic union of worker and canonical state usage so the final
dictionary can reuse content-equivalent worker branches without omitting collated
cells. This could remove H11's remaining 18.2-18.6 ms canonical rebuild, but it is
consensus-adjacent and requires proof-equivalence tests at candidate level.

### H13: reduce estimator barrier wait

H11 averages only 1.25-1.32 updates per worker batch and waits 15-18 ms/block.
Submit small producer-side batches or use a single-producer queue, preserving the
same sequence barrier and exact root. The maximum possible gain is now modest.

### H2: independent final serialization

The block BOC and collated-data BOC are independent after proof construction.
Parallel serialization can save at most the smaller side of their current 18 ms
combined stage. It is lower risk but lower potential than H12.

### H6: transaction parallelism

The workload has many independent accounts, but the current loop exposes logical
time and generated-message order to TVM. A correct design needs a deterministic
compute/commit protocol or protocol-level LT allocation; a thread pool around the
existing loop is invalid.

## Acceptance protocol

Every accepted optimization must pass all of the following:

1. Release build and relevant unit tests.
2. Identical workload settings and forced empty slot schedule.
3. Valid TX1-TX4 counts, no aborted acknowledged requests, and full drain.
4. No shard split, extra empty-slot pattern, persistent queue growth, or material
   serialized-size regression.
5. At least two consistent full timing runs for a claimed win.
6. Productive-block mean and P95 comparison, plus per-transaction phase evidence.
