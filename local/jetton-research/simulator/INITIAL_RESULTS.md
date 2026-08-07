# Initial simulator results

These historical runs used init mode 1. The simulator now defaults to mode 0;
the mode 0 comparison is recorded in `SHARDING_RESULTS.md`.

These measurements validate the workload, accounting, and collation probes. They
do not establish maximum sustainable jTPS yet; both 1,000 jTPS runs deliberately
overload the one-shard pipeline, and their steady windows include pipeline fill.

## Configuration

- Release build from `assembly/native/build-ubuntu-shared.sh`.
- One DHT node and one full validator, Simplex v2, one unsplit workchain-0 shard.
- 400 ms target block period (2.5 BPS), eight validator-engine worker threads.
- Fresh zerostate and real on-chain WalletSpam/JettonWallet deployment.
- Exact deterministic workload copied from the frozen devnet `ton jetton-spam`
  implementation, including contract code, signed externals, values, and four
  transaction stages.

## Production-shaped overload probe

Command parameters: 10,000 active wallets, 1,000 offers/s, 2 s warmup, 10 s
measurement, and 60 s drain. The run is stored as
`runs/10k-1000jtps-10s-initial-20260804`.

- All 10,000 sends were acknowledged and eventually produced exactly 10,000
  TX1, TX2, TX3, and TX4 transactions; no transaction aborted.
- All 175 observed accepted workchain blocks joined their successful candidate
  record by exact `(seqno, root_hash)`; no block was skipped or failed parsing.
- In the 7.998 s marked window, TX1 was 576.507/s, TX3 was 370.210/s, and raw
  throughput was 1,703.139 transactions/s. These unequal stage rates describe
  an overloaded pipeline filling, not a sustainable completion rate.
- The new OutMsgQueue grew by 282.7 messages/s in that window, reached 5,062
  after it, and returned to zero during drain.
- Twenty steady-window candidates took 189.189 ms mean real time and 189.170 ms
  mean CPU time. Summed candidate CPU was 0.473 core over wall time.
- Every steady candidate was overloaded by block load. The highest stage load
  was after new-message processing, at 1.736 times its applicable threshold on
  average.

This run predates the final non-overlapping phase and per-axis category probes,
so its old phase fields are diagnostic but cannot be added into a CPU breakdown.

## Instrumented overload probe

Command parameters: 2,000 active wallets, 1,000 offers/s, 2 s measurement, and
25 s drain. The run is stored as
`runs/saturated-instrumentation-2k-20260804`.

- All 2,000 sends were acknowledged and eventually produced exactly 2,000 TX1,
  TX2, TX3, and TX4 transactions, with no abort, parse failure, or missing
  accepted-block/candidate join.
- Four of five measurement-window candidates were overloaded. The per-axis
  maximum categories were block bytes 3 (medium), gas 1 (normal), LT delta 0
  (underload), and collated-data bytes 1 (normal). Block bytes were therefore
  the binding protocol axis.
- For this zerostate, workchain block-byte thresholds are 262,144 underload,
  1,048,576 soft, 1,572,864 computed medium, and 2,097,152 hard. Category 3 means
  the estimated block size reached 1,572,864 bytes but remained below hard.
- The measurement-window queue grew by 404.5 messages/s to 809, peaked at 1,112
  after the window, and drained to zero.
- Mean candidate work was 134.850 ms real and 134.829 ms CPU, or 0.337 core over
  the two-second window. CPU time was 46.0% inbound externals, 17.5% inbound
  internals, 12.3% newly generated messages, 7.6% collated-data construction,
  and 5.7% account-transaction combination. The top-level timer model accounts
  for 99.8% of candidate CPU without summing nested transaction timers.

## Initial conclusion

At 1,000 offered jTPS and 2.5 BPS, this workload is block-size-limited before it
is CPU-limited. The node has substantial CPU headroom while the block byte
estimate crosses the medium threshold, external admission stops, and internal
messages accumulate for later blocks. This identifies the first optimization
question: bytes contributed by each transaction/message/cell and the policy by
which new messages are executed versus enqueued.

It does not identify the maximum sustainable rate. The next experiment is a set
of long 10,000-wallet runs below saturation (initially 250, 350, 450, 550, and
650 offered jTPS), followed by a narrower search around the first rate where
TX1/TX3 diverge, latency slopes upward, or OutMsgQueue fails to remain bounded.
Only then should profiling compare CPU optimizations at a stable operating point.
