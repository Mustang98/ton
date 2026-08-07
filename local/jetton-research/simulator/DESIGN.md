# Jetton-spam collation simulator

## Objective

Measure the sustainable one-shard jetton-transfer completion rate and identify the
limiting collation stage in the real C++ TON node. Consensus, validation, overlays,
and liteserver work are not the subject of the primary number, but the first
implementation uses a one-validator Simplex network as a correctness oracle and a
practical state-transition driver.

The simulator is a new workload. It must not reuse the legacy benchmark's wallet-v5
or standard jetton-wallet transaction shape.

## Frozen workload

The contracts, pool, and message fields are pinned to the read-only devnet
sources inspected on 2026-08-04. Recipient init mode is an experiment parameter:
mode 1 matches the current devnet CLI, while the default mode 0 matches the
classic StateInit shape recorded for the 2026-07-21 testnet run.

| File | SHA-256 |
| --- | --- |
| `go-spam/pkg/walletspam/code.go` | `3a88326de8ec5c98079138b55156f6baa5de0d4aab00f37fd7c1f0d26cb4ac08` |
| `go-spam/pkg/walletspam/walletspam.go` | `9d6f9851432eb14b43ab48c311797edc1f27ca503c1d2348f245b8d58027e23b` |
| `go-spam/pkg/jetton/transfer.go` | `660ec0373a1d6b7e9f149bd5b696fdabb341120220129a768efa593edb1ed904` |
| `go-spam/pkg/pool/pool.go` | `09a4dfd5e3dcadd9f52d536c570a2fe8dab08ac334d82870fbea02af61036a2d` |
| `go-spam/cmd/spam/main.go` | `148a4ea64ddd046e2184930299674f0e0590bf5974f3aee3413e20b870c1e55f` |
| `contracts/WalletSpam.tolk` | `00f42742819306eff0ff7b88eb4639ef8e3727dc9c9d452cbda9482aa6d1aad9` |
| `contracts/JettonWallet.tolk` | `22ed58d37c57edf05772519d6031fdb80c0bc99b0a3b34ff9755d1b71f3503d3` |

The simulator embeds the compiled WalletSpam and prepaid JettonWallet BoCs from
`code.go`. Contract root hashes and deterministic address/body golden vectors are
checked by the C++ `self-test` command before a run.

### Pool semantics

For `--pool-size N`:

- Active sender and recipient IDs are `[0, N)`.
- Deployment creates `[0, N]`; ID `N` is the readiness sentinel.
- All WalletSpam contracts share one Ed25519 key and differ by `subwalletId`.
- Every WalletSpam owns one predeployed prepaid JettonWallet.
- Sender order is a random permutation. Each ID is used once per epoch, then the
  permutation is reshuffled.
- Recipient IDs are independent uniform draws from `[0, N)`, including self.
- The load generator optimistically increments its cached seqno after each fire, as
  the devnet generator does. A rejected earlier message can therefore invalidate a
  later message from the same sender; this is measured rather than hidden.

### Transfer parameters

- External destination: sender WalletSpam.
- External validity: construction time plus 120 seconds.
- Wallet send mode: `PAY_FEES_SEPARATELY | IGNORE_ERRORS` (`3`).
- Internal value to sender JettonWallet: 0.05 TON.
- Jetton amount: 1.
- `responseAddress`: original sender WalletSpam.
- `forwardTonAmount`: 0.
- `internal_init_mode`: configurable; default 0 attaches full recipient
  JettonWallet StateInit. Mode 1 sends to the bare predeployed address.
- Query ID: 0.
- Forward payload: random decimal comment in a referenced cell.

### Successful transaction chain

1. TX1, WalletSpam, external: signature/expiry/seqno checks, seqno increment, M1.
2. TX2, sender JettonWallet, `0x0f8a7ea5`: debit one jetton, M2.
3. TX3, recipient JettonWallet, `0x178d4519`: credit one jetton, M3.
4. TX4, original WalletSpam, `0xd53276db`: receive excess TON and ignore opcode.

TX3 is semantic jetton completion. TX4 is refund completion. Offered externals,
send acknowledgements, TX1 inclusions, TX3 completions, TX4 completions, and raw
transactions per second are separate metrics.

## Architecture

### C++ workload client

`jetton-simulator` has three commands:

- `self-test`: validate contract roots, address derivation, signatures, TL-B, and
  the upstream fixed vectors.
- `prepare`: produce the exact signed deployment kickoff BoC and a run manifest.
- `spam`: build/sign exact transfer externals, send them over dedicated ADNL
  liteserver connections, watch workchain-0 blocks, classify all four transaction
  stages, and write JSON/CSV results.

The liteserver is transport for the oracle configuration, not part of the reported
collator timing. Generator construction and acknowledgement rates prove that input
is not the bottleneck.

### Python one-validator orchestrator

`run.py` creates one validator and one unsplit basechain shard with Simplex consensus
and a configurable fixed block period (400 ms by default). It:

1. Starts the node and waits for workchain-0 production.
2. Asks the C++ tool to prepare the pool.
3. Funds uninitialized WalletSpam #0 from the zerostate main wallet.
4. Sends the kickoff external with WalletSpam #0 StateInit.
5. Waits for sentinel ID `N`'s JettonWallet to become active.
6. Runs the C++ load client.
7. Collects block summaries, collation log records, node logs, and a run manifest.

The deployment path is intentionally real. It guarantees byte-exact account state,
including the initial WalletSpam #0 seqno and deployment-created balances, without
duplicating contract state logic in an offline generator.

### State choice

The baseline starts from a fresh tontester zerostate and deploys the complete pool
through real transactions. A copied mainnet database is not the initial baseline:
it would bring unrelated account history and message queues into the one-shard
measurement, would need a validator/config epoch consistent with that snapshot,
and would not contain these deterministic WalletSpam accounts. Snapshot-state and
cache-temperature sensitivity should be measured later as a separate axis after
the exact synthetic workload has a stable saturation point.

### Future post-admission mode

After oracle parity, add a benchmark-only feeder at the checked-message boundary of
`ExtMessagePool`. It will place valid `ExtMessage` objects into the real collator
queue without liteserver or checker work. Both cold and checker-prewarmed variants
are required because the checker currently executes TX1 and warms the account path.

A stripped block driver is justified only if non-collation work measurably interferes
with the oracle. It must still use the real `Collator`, create the complete candidate,
apply its Merkle update, retain persistent message queues, and advance parent state.
Discarding candidates is not a valid repeated-block simulation.

## Node instrumentation

The first patch adds generic, low-overhead per-candidate counters rather than
jetton-specific branches:

- external and internal ordinary transaction counts;
- generated, immediately processed, enqueued, and deferred new-message counts;
- peak in-memory `new_msgs` size;
- real/CPU transaction time split by external versus internal input;
- real/CPU time for every existing major collation phase;
- candidate root hash, queue movement, block-limit class, and overload decision;
- existing old/new OutMsgQueue sizes and limit-stop logs.

The C++ block parser performs workload-specific TX1/TX2/TX3/TX4 classification.
High-volume per-transaction node logging is forbidden in measurement runs.
`analyze.py` joins accepted blocks to successful candidates by `(seqno, root_hash)`
and aggregates only the candidates in the C++ sender's marked steady window.

## Result model

Each run records:

- exact source revision, binary hashes, contract hashes, seed, pool size, rate,
  duration, block period, and node arguments;
- offered, constructed, acknowledged, failed, TX1, TX2, TX3, and TX4 rates;
- raw TPS and transactions per candidate;
- external-to-TX1 and sampled TX1-to-TX2-to-TX3-to-TX4 latency;
- candidate actual/estimated bytes, collated bytes, gas, LT delta, and limit class;
- in-memory and persistent queue growth;
- collation real/CPU phase timings;
- optional `perf stat`, `perf record`, RocksDB, and device counters.

The maximum sustainable jTPS is the highest offered rate for which, over the steady
window:

- TX3 rate is within 1% of accepted TX1 rate;
- TX3 and TX4 backlog slopes are not positive;
- OutMsgQueue and completion latency do not grow without bound;
- the configured block cadence is maintained without repeated/cancelled candidates.

## Experiment order

1. Deterministic self-test and two-wallet contract-emulator checks.
2. Small pool deployment and 5-20 jTPS end-to-end smoke run.
3. Exact 10,000-active-wallet deployment and low-rate correctness baseline.
4. Rate ramp at fixed 2.5 BPS, followed by binary search around saturation.
5. Warm/cold runs and active-pool cardinality sweep.
6. Mainnet block limits versus raised diagnostic limits.
7. Post-admission feed comparison.
8. `perf`/CellDB profiling at the first stable saturated point.

Every optimization must preserve the four-stage counts, aggregate jetton supply,
per-owner seqno progression, queue drain, and candidate validity before its throughput
result is accepted.
