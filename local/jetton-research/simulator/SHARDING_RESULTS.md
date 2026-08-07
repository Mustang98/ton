# Jetton one-to-two-shard experiment

Date: 2026-08-04

## Result

At the default workchain limits, this exact jetton workload has a reproducible
one-shard split boundary between 390 and 400 requests/s at 2.5 blocks/s:

- 390 requests/s ran for 45 seconds with TX1 equal to TX3, no persistent queue,
  no overloaded candidates, and no `want_split`.
- 400 requests/s produced a split twice. The topology changed after 31.5 seconds
  on the first attempt and 36.6 seconds on the repeat.
- The decisive default-limit block was overloaded only by estimated block bytes.
  Gas and collated-data bytes were normal, collation work was 146 ms, and the
  source-level long-collation predicate was false.

Doubling the workchain byte-family limits moved the measured boundary to between
700 and 750 requests/s:

- 700 requests/s ran in one shard for 30 seconds, delivered 696.2 TX3/s, ended
  with no persistent queue, and never set `want_split`.
- 750 requests/s did not remain stable. TX3 fell behind TX1, the persistent queue
  grew from 0 to 976, and the shard split 35.7 seconds after that rate began.
- Near 750, byte pressure and collation time were both active. Raising block
  bytes further by itself therefore does not establish a safe higher rate.

The practical conclusion for this local one-validator oracle is:

| Configuration | Demonstrated one-shard point | Observed split point |
| --- | ---: | ---: |
| Default limits, mode 1 | 390 requests/s | 400 requests/s, repeated |
| Default limits, mode 0 | 390 requests/s, with intermittent overload | 400 requests/s |
| 2x byte-family limits, mode 1 | 700 requests/s | 750-800 requests/s |

The 700 point is demonstrated locally, not certified for production. A longer
650/700 soak and multi-validator candidate propagation/validation measurements
are required before proposing a network configuration change. Of the two, 650
has materially more timing margin.

## Mode 0 rerun

After changing the simulator default to mode 0, two fresh 10,000-wallet runs
used the same one-validator topology, 400 ms target block period, default limits,
and `min_split=0,max_split=1`.

The fixed 300 requests/s run is:

```text
runs/mode0-10k-300jtps-split-20260804
```

It sustained 301.1 TX3/s and 1,204.3 raw TPS for the measured window. All 17,999
acknowledged requests completed TX1-TX4, the queue drained to zero, no candidate
was overloaded, and no `want_split` or topology change occurred. Mean candidate
work was 107.8 ms. Compared with the prior mode 1 300-jTPS run at the same mean
480 transactions/block, mode 0 added 10.6 kB final estimated bytes and 13.3 kB
actual bytes per block. Repeated StateInit code cells are shared by the block
cell DAG, so full standalone StateInit size does not multiply directly by the
number of transfers in the serialized block.

The boundary sweep is:

```text
runs/mode0-split-threshold-10k-20260804
380:45,100:45,390:45,100:45,400:45
```

| Target | TX3/s | Mean/p95 work | Block-limit overloads | `want_split` | Topology |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 380 | 374.2 | 133/153 ms | 0 | 0 | 1 -> 1 |
| 390 | 385.0 | 141/164 ms | 48 intermittent | 0 | 1 -> 1 |
| 400 | 387.5 | 113/153 ms across 1-2 shards | 75 | 52 | 1 -> 2 |

At 400, the first `want_split` was parent block 1007, 10.372 seconds after the
phase began. It had 643 transactions, 1,180,064 final estimated bytes, 838,368
actual bytes, 2,355,798 gas, 144.5 ms collation work, an OutMsgQueue of 20, and
`overload_reason=1` (block limits). The parent emitted its `before_split` block
at seqno 1058, and child seqno 1059 was observed 31.152 seconds after phase
start. All 61,624 acknowledged requests in the sweep eventually completed all
four stages with no abort, and the drain ended with an empty queue.

Mode 0 therefore increases byte pressure at the margin but does not explain a
split at 80-88 jTPS in this local workload. Its observed one-to-two-shard
boundary remains between 390 and 400 requests/s. The 390 point is less robust
than mode 1 because it already generates intermittent overload bits, so it
should not be called a zero-headroom operating point.

## Configuration

All measured runs used:

- 10,000 active WalletSpam accounts and their prepaid JettonWallet contracts;
- the `ton jetton-spam` transfer shape with init mode 1 (bare recipient address,
  no StateInit); these historical results predate the simulator's mode 0 default;
- one DHT node and one validator/full node;
- Simplex v2 with a 400 ms target block period, empirically 2.5 workchain BPS;
- workchain `min_split=0`, `max_split=1`;
- one initial workchain shard and at most two child shards;
- eight validator-engine worker threads;
- real candidate construction, validation, consensus, accepted blocks, state,
  external admission, internal messages, and OutMsgQueue processing;
- a fresh synthetic zerostate and real on-chain deployment of the full pool.

The local workchain split/merge timings were a 20 second scheduling delay and a
20 second allowed interval. These are deliberately shorter than the defaults in
`WorkchainInfo` and are generated by tontester.

One successful request creates four workchain transactions: WalletSpam TX1,
sender JettonWallet TX2, recipient JettonWallet TX3, and WalletSpam reply TX4.
Reported jTPS is successful TX3/s; raw TPS counts all accepted workchain
transactions.

## Block limits

The workchain block limits used by the experiments were:

| Axis | Underload | Soft | Medium | Hard |
| --- | ---: | ---: | ---: | ---: |
| Default bytes | 262,144 | 1,048,576 | 1,572,864 | 2,097,152 |
| 2x bytes | 262,144 | 2,097,152 | 3,145,728 | 4,194,304 |
| Gas, both runs | 2,000,000 | 10,000,000 | 15,000,000 | 20,000,000 |

The legacy block-limit config used here assigns the byte limits to collated data
as well. `--block-limit-mul 2` doubles byte, collated-data, and LT soft/hard
limits; underload thresholds stay unchanged. `--gas-limit-mul 1` leaves gas
unchanged.

The final `estimated_bytes` recorded after candidate construction can be larger
than the estimate at overload classification. The per-axis category and
`overload_reason` fields are authoritative for the split decision.

## How a split is requested

For every candidate, `Collator::check_block_overload()` shifts two 64-bit
histories and may add an overload or underload bit. An overload bit is added if:

1. any block-limit axis reaches the soft class;
2. the dispatch queue limit is reached; or
3. collation meets the long-time predicate.

The long-time predicate is:

```text
total_time > 100 ms
wait_externals < 20% of total_time
do_collate_time > 60% of total_time
```

The overload history weight is:

```text
3 * popcount(newest 16 bits)
+ 2 * popcount(previous 16 bits)
+ 1 * popcount(previous 16 bits)
- 64
```

`want_split` becomes true at weight zero or above. Starting from an empty
history, 24 consecutive overloaded blocks are sufficient. At 2.5 BPS this is
about 9.6 seconds. The masterchain shard-configuration FSM then schedules the
split after the configured 20 second delay. This explains the observed 31-37
second rate-to-topology delay.

A merge is analogous, but both sibling shards must request it. After the test
rate dropped to 100 requests/s, observed merge delays were 32.1-33.7 seconds.

Relevant implementation points are `Collator::history_weight()` and
`Collator::check_block_overload()` in `validator/impl/collator.cpp`, and the
split/merge FSM update near `update_one_shard()` in the same file.

## Default-limit evidence

The narrow threshold run is:

```text
runs/split-threshold-10k-refine-20260804
380:45,100:45,390:45,100:45,400:45,100:50,400:45
```

All 84,596 acknowledged requests produced exactly 84,596 successful TX1, TX2,
TX3, and TX4 transactions. No transaction aborted. All 1,036 accepted workchain
blocks joined exactly to their successful candidate record.

| Target | TX1/s | TX3/s | Mean/p95 work | Overloaded | `want_split` | Queue first/last/max |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 380 | 374.0 | 374.0 | 131/155 ms | 11 transient | 0 | 0/0/246 |
| 390 | 384.9 | 384.9 | 142/164 ms | 0 | 0 | 0/0/3 |
| 400, first | 394.7 | 387.3 | 112/150 ms across 1-2 shards | 69 | 51 | 0/489/550 |
| 400, repeat | 396.8 | 389.4 | 119/159 ms across 1-2 shards | 73 | 52 | 0/474/573 |

Phase throughput uses accepted-block observation timestamps, so block-boundary
spill makes short phase rates slightly lower than the exact offered rate. Queue
and equality of stage totals establish whether the one-shard pipeline kept up.
The 400 rows include the child-shard tail after the topology change; they are not
used as one-shard throughput claims.

The first decisive 400-rate `want_split` block was workchain block 1008:

| Field | Value |
| --- | ---: |
| Transactions | 636, exactly 159 in each TX stage |
| Estimated / actual block bytes | 1,128,570 / 791,119 |
| Estimated / actual collated bytes | 833,772 / 461,999 |
| Gas | 2,377,209 |
| Byte / collated-data category | soft / normal |
| Collation work | 145.998 ms |
| External wait / total timer | 249.335 / 358.368 ms |
| Long-collation predicate | false |
| New OutMsgQueue | 2 |
| Overload history | `0x0000000000ffffff` |

No candidate in either 400 phase met the long-collation overload predicate.
Every split-causing overload was reason 1, block limits.

Timing relative to each phase start:

| Event | First 400 | Repeated 400 |
| --- | ---: | ---: |
| First `want_split` | 11.166 s | 15.746 s |
| Actual topology `1 -> 2` | 31.518 s | 36.558 s |
| Low-rate topology `2 -> 1` | 32.133 s after low began | n/a |

## Doubled-limit evidence

The comparison run is:

```text
runs/split-blocklimit2x-10k-sweep-20260804
400:25,550:25,650:25,700:30,750:35,800:45,100:55,800:45
```

All 163,080 acknowledged requests eventually produced exactly 163,080
successful transactions at every TX stage, with no abort. All 1,062 accepted
workchain blocks joined exactly to candidate records, and the persistent queue
drained to zero.

| Target | TX1/s | TX3/s | Mean/p95 work | Byte/time overloads | `want_split` | Queue first/last/max |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 400 | 388.0 | 388.0 | 139/156 ms | 0 / 0 | 0 | 0/0/3 |
| 550 | 549.3 | 549.3 | 178/202 ms | 0 / 0 | 0 | 0/0/3 |
| 650 | 641.7 | 641.6 | 212/243 ms | 0 / 13 | 0 | 0/4/4 |
| 700 | 696.1 | 696.2 | 214/244 ms | 5 / 16 | 0 | 4/0/257 |
| 750 | 731.6 | 714.4 | 267/364 ms | 67 / 53 predicate | 50 | 0/976/1,292 |

At 650 and 700, time-overload bits were intermittent and their weighted history
never reached the split threshold. At 750, 70 of 87 candidates were overloaded;
53 met the time predicate and byte pressure had priority in 67 reason records.
TX3 lag and persistent queue growth independently show that 750 was not merely a
conservative split-policy event.

The first `want_split` at the raised boundary was block 831, 14.886 seconds after
750 began. The topology split at 35.676 seconds. Its state already reflected the
unstable queue: 1,191 transactions, 3,169,835 estimated bytes, 5,386,905 gas,
13.034 ms external wait, 216.318 ms total timer, 296.772 ms recorded work, and a
new OutMsgQueue of 1,217.

After 55 seconds at 100 requests/s, the children merged 33.711 seconds after the
low phase began. A clean 800-rate repeat then requested and performed another
split after 33.541 seconds.

## Interpretation

The default split at 400 is a policy headroom decision, not an inability to
execute 400 transfers/s. The collator was keeping up, and a single soft byte axis
caused the overload history. Raising that axis therefore has real value for this
workload.

The 2x run sustained about 700 requests/s in one shard, an approximately 79%
increase over the highest default-limit no-split point. The gain is less than 2x
because candidate work and the no-wait timing predicate become important before
or alongside the enlarged byte limit. At 750, the queue and TX-stage divergence
show actual one-shard saturation.

Changing only the split history or split scheduling delay would postpone the
topology change but would not increase collation capacity. It is reasonable at
the default 400 point, where there is no queue growth, but unsafe at the raised
750 point, where work is already accumulating. The correct next optimization
target is candidate work per transfer, especially inbound external/internal
processing, new-message handling, collated-data construction, and account
transaction combination.

## Validity and limitations

- The node ran real consensus and accepted blocks, but there was only one
  validator. Candidate propagation, multi-validator checking, disk variance,
  and adversarial network timing were not stressed.
- The user-requested assumption that consensus and finalization remain healthy
  is valid for collator attribution, but not sufficient to call larger blocks
  production-safe.
- The state is a fresh synthetic 10,000-wallet state, not a mainnet snapshot.
- The 650 and 700 raised-limit phases lasted 25 and 30 seconds. They cover more
  than one 48-block weighted-history horizon, but need a 5-10 minute soak.
- High-rate rejected externals are optimistic pre-signed seqno-chain failures.
  They are counted separately. Every acknowledged message was included and
  completed all four transaction stages.
- Raising hard limits affects candidate serialization, validation, propagation,
  storage, and denial-of-service headroom outside this collator-focused test.

## Next measurements

1. Soak 650 and 700 requests/s for 5-10 minutes with 2x limits and require zero
   persistent queue slope, no `want_split`, and stable p99 collation work.
2. Profile the stable 650/700 point and the unstable 750 point with `perf`, using
   the existing phase counters to attribute CPU and allocation hot paths.
3. Test a smaller limit increase between default and 2x after allowing fractional
   multipliers, so the byte headroom matches measured timing headroom.
4. Repeat the chosen configuration with multiple validators and measure candidate
   encode, propagation, validation, and block cadence before any production claim.
