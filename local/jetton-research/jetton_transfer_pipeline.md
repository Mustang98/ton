# Jetton transfer pipeline

This note explains a normal user-to-user jetton transfer from the user, TVM,
contract, node, shard, and infrastructure perspectives.

It is written for a reader who already understands the C++ node architecture,
but is less familiar with TON user accounts, wallet contracts, external
messages, TVM transactions, and jetton contracts.

Scope:

- Source material used here is local only: the extracted private chat archive,
  local jetton test contracts, local wallet contracts, and local node code.
- I did not open any links from the chat.
- The jetton contract flow is based on the local legacy jetton wallet/minter
  contracts in `crypto/func/auto-tests/legacy_tests/jetton-wallet` and
  `crypto/func/auto-tests/legacy_tests/jetton-minter`. Modern deployed jettons
  may customize code, but the core pipeline is the same.
- The node-side behavior is based on local files such as `crypto/block/block.tlb`,
  `crypto/block/transaction.cpp`, `validator/impl/external-message.cpp`,
  `validator/impl/ext-message-pool.cpp`, `validator/impl/collator.cpp`,
  `validator/impl/liteserver.cpp`, and `validator/full-node-shard.cpp`.

## Executive summary

A jetton transfer is not one transaction.

From the user perspective it looks like:

1. Press "send jettons".
2. Sign in a wallet app.
3. Wait until sender balance decreases and recipient balance increases.

On-chain it is usually:

1. An external message to the user's TON wallet contract.
2. A transaction on the user's TON wallet contract.
3. An internal message to the sender's jetton wallet contract.
4. A transaction on the sender's jetton wallet contract.
5. An internal message, often with `StateInit`, to the recipient's jetton wallet
   contract.
6. A transaction on the recipient's jetton wallet contract.
7. Optionally, an internal notification to the recipient owner contract.
8. Optionally, an internal excess-refund message to the response address.

So one "jetton transfer" is commonly at least three account transactions:

- sender TON wallet transaction
- sender jetton wallet transaction
- recipient jetton wallet transaction

It can become four or five transactions if notification and excess messages are
sent. If the accounts are in different shards, messages also enter out-message
queues and are imported by other shard blocks.

This is why `jTPS` is not the same as raw account transaction TPS. In the chat,
`jTPS` was clarified as "jetton transfers per second", not "all transactions per
second".

The current performance problem is not a single bottleneck. A jetton transfer
stresses:

- liteserver external-message ingestion
- external-message pre-check and mempool
- broadcast of externals to validators
- collator external-message selection
- wallet external transaction execution
- sender jetton wallet TVM execution
- recipient jetton wallet activation and TVM execution
- message forwarding fees and message size checks
- out-message queue operations for cross-shard transfers
- block byte/gas/lt-delta/collated-data limits
- shard split heuristics
- validator validation and consensus
- liteserver/indexer catch-up

The most important chat-derived numbers:

- Around October 2025, the team considered user operation time around 5 seconds
  improved but insufficient; target discussion moved toward about 0.5 seconds.
- April 2026: 200 TPS jetton load could hurt testnet badly.
- April 7, 2026: 80 external/s jetton spam resulted in about 60/s passing,
  with `do_collate` around 395 ms and no `want_split` observed.
- May 2026: testnet jetton spam reached reported regimes like 720 TPS/144 EPS,
  880 TPS/177 EPS, 1400 TPS/293 EPS, and 1800 TPS/438 EPS, but shards and
  infrastructure were unstable.
- May 23-25, 2026: one shard split around 94 jetton sends/s and roughly 400 TPS.
  Another note says 400 TPS corresponded to about 135 jetton sends/s.
- June 12, 2026: local benchmark on a mainnet-like configuration showed about
  90 jTPS; current megabyte blocks were said to impose a hard cap below about
  600 jTPS in one shard.
- Optimized non-in-memory collator was reported around 315 jTPS; optimized
  in-memory around 480 jTPS; current testnet in-memory branch around 200 jTPS.
- June 13-14, 2026: liteserver ingress was measured separately around 8k
  externals/s per LS before severe degradation; Go LS with two connections got
  about 13k accepts/s.

## Core mental model

### Accounts

TON state is a map from account addresses to account state.

An account has:

- an address
- a TON balance, stored in the account state
- optional code
- optional persistent data
- last transaction hash and logical time
- storage statistics and storage-fee obligations

An account is not necessarily a user. It can be:

- a wallet contract controlled by a user key
- a jetton minter/master contract
- a jetton wallet contract
- a DeFi contract
- an uninitialized address that can later be activated by a message with
  `StateInit`

### TON balance vs jetton balance

TON is native. TON balance is part of the account state.

Jettons are not native balances. A jetton balance is a number stored inside a
jetton wallet contract's data cell.

For a normal fungible jetton:

- the jetton master/minter contract stores global data such as total supply,
  admin, metadata/content, and the jetton wallet code
- each owner has a separate jetton wallet contract address for this jetton
- that jetton wallet stores the owner's jetton balance

The owner does not directly store the jettons in the user's TON wallet contract.
The user's TON wallet owns the jetton wallet because the jetton wallet stores
`owner_address` in its data.

The local legacy jetton wallet storage is:

```text
balance:Coins
owner_address:MsgAddressInt
jetton_master_address:MsgAddressInt
jetton_wallet_code:^Cell
```

The local minter storage is:

```text
total_supply:Coins
admin_address:MsgAddress
content:^Cell
jetton_wallet_code:^Cell
```

### Messages

Almost all activity is driven by messages.

Two message types matter here:

- external inbound messages, from outside the blockchain into an account
- internal messages, from one account to another account

In `crypto/block/block.tlb`:

```text
ext_in_msg_info$10 src:MsgAddressExt dest:MsgAddressInt import_fee:Grams
int_msg_info$0 ihr_disabled:Bool bounce:Bool bounced:Bool
  src:MsgAddressInt dest:MsgAddressInt
  value:CurrencyCollection extra_flags:(VarUInteger 16) fwd_fee:Grams
  created_lt:uint64 created_at:uint32
```

External inbound messages have no attached TON value in the normal way. The
destination account must pay for accepted execution. This is why wallet contracts
call `accept_message()` only after checking signature, `seqno`, and expiration.

Internal messages can carry TON value. Jetton transfer internals must carry
enough TON to pay storage, gas, forwarding, notifications, and excess return.

### Transactions

A transaction is the effect of one inbound message on one account.

The ordinary transaction descriptor in `block.tlb` contains these phases:

- storage phase
- credit phase
- compute phase
- action phase
- optional bounce phase

For internal messages, crediting the incoming value and collecting storage fees
are part of the transaction. For external messages, there is no incoming value
to credit.

The C++ path is centered around:

- `Transaction::unpack_input_msg`
- `Transaction::prepare_storage_phase`
- `Transaction::prepare_credit_phase`
- `Transaction::prepare_compute_phase`
- `Transaction::prepare_action_phase`
- `Transaction::prepare_bounce_phase`
- `Transaction::serialize`
- `Transaction::commit`

### `accept_message()`

For an external message, TVM starts with a limited gas credit. The contract must
validate cheap conditions first. If it decides the external is legitimate, it
calls `accept_message()`. After that, the account pays for gas.

User wallet contracts typically do:

1. parse signature and parameters
2. check `valid_until`
3. load stored `seqno`, subwallet id, public key
4. check message `seqno`
5. check signature
6. call `accept_message()`
7. update stored `seqno`
8. emit outgoing internal messages

The local node pre-check for externals runs TVM with
`stop_on_accept_message=true`. That means the liteserver/validator can cheaply
determine whether an external reaches `accept_message()` without fully executing
all actions in that pre-check.

Full action execution happens when a collator actually includes the external in
a block.

### Shards

An account belongs to a shard by address prefix.

If an internal message's destination is in the same shard being collated, the
collator may process it in the same block after the sender transaction.

If the destination is outside the shard, the message is wrapped into a message
envelope and placed into the out-message queue. A later destination shard block
imports and processes it.

This is why "cross-shard jetton transfer" is the right reference workload:

- the external enters the sender TON wallet shard
- the sender jetton wallet may be in another shard
- the recipient jetton wallet may be in another shard
- notification/excess messages may go to yet other accounts

Each step can add block latency and queue work.

## The contracts involved

### User TON wallet

The user TON wallet is the account that receives the external message.

It owns TON, stores a key and `seqno`, and emits internal messages.

For wallet v4 in the local legacy test contract, `recv_external` reads:

```text
signature:512 bits
subwallet_id:uint32
valid_until:uint32
msg_seqno:uint32
op:uint8
refs to outgoing messages
```

After validation it calls `accept_message()`, stores `seqno + 1`, commits, and
sends raw messages from the refs.

The user's TON wallet is not the jetton wallet. It only authorizes the outgoing
internal message to the sender's jetton wallet.

### Jetton master/minter

The jetton master/minter is not normally involved in a transfer.

It is used to:

- expose `get_jetton_data`
- expose `get_wallet_address(owner_address)`
- mint
- verify burns
- store the canonical jetton wallet code

The wallet address is derived from:

- owner address
- master address
- jetton wallet code
- initial jetton wallet data with balance zero

The local helper builds `StateInit`, hashes it, and constructs a standard
address in the current workchain.

### Sender jetton wallet

The sender jetton wallet stores the sender's jetton balance.

It accepts an internal message with op `transfer` from its owner address.

The local op codes:

```text
transfer              0xf8a7ea5
internal_transfer     0x178d4519
transfer_notification 0x7362d09c
excesses              0xd53276db
burn                  0x595f07bc
burn_notification     0x7bdd97de
```

When receiving `transfer`, it:

1. loads `query_id`
2. loads jetton amount
3. loads destination owner address
4. checks destination is in the same workchain in the local legacy contract
5. loads its own data
6. subtracts jetton amount
7. checks sender is the stored owner
8. checks balance remains non-negative
9. computes recipient jetton wallet `StateInit`
10. computes recipient jetton wallet address
11. loads response address, custom payload, forward TON amount, forward payload
12. builds `internal_transfer`
13. attaches the recipient wallet `StateInit`
14. checks incoming TON value can pay the expected costs
15. sends the internal message
16. saves the reduced jetton balance

### Recipient jetton wallet

The recipient jetton wallet accepts op `internal_transfer`.

It may already exist or may be activated by the `StateInit` attached by the
sender jetton wallet.

When receiving `internal_transfer`, it:

1. loads its data
2. loads `query_id`
3. loads jetton amount
4. increases its jetton balance
5. loads original sender owner address
6. loads response address
7. checks the sender is either the jetton master or the expected jetton wallet
   for the original sender owner
8. loads forward TON amount
9. computes storage/gas reserve
10. optionally sends `transfer_notification` to the recipient owner
11. optionally sends `excesses` to the response address
12. saves the increased jetton balance

### Recipient owner wallet or contract

If `forward_ton_amount > 0`, the recipient jetton wallet sends a notification
to the recipient owner address.

For an ordinary user wallet, this internal message is usually ignored by wallet
contract logic, but it is useful for indexers/wallet apps. For a contract owner,
the notification may be part of application logic.

### Response destination

If a response destination is present and there is leftover TON value after
storage/gas/notification costs, the recipient jetton wallet sends `excesses`
back.

This is usually how leftover TON from the transfer is returned to the sender or
another selected address.

## Message bodies in a normal transfer

### External message to user wallet

This is an `ext_in_msg_info` message.

Destination:

```text
user TON wallet address
```

Body:

```text
wallet signature and parameters
one or more outgoing internal messages as refs
```

One of those outgoing internal messages is addressed to the sender's jetton
wallet and has body op `transfer`.

### Internal `transfer` to sender jetton wallet

Destination:

```text
sender jetton wallet address
```

Body:

```text
op::transfer()
query_id:uint64
amount:Coins
destination:MsgAddress       ; recipient owner address, not recipient jetton wallet address
response_destination:MsgAddress
custom_payload:Maybe ^Cell
forward_ton_amount:Coins
forward_payload:Either Cell ^Cell
```

Important: the destination field is the recipient owner address. The sender
jetton wallet computes the recipient jetton wallet address from that owner
address and the canonical wallet code.

### Internal `internal_transfer` to recipient jetton wallet

Destination:

```text
recipient jetton wallet address
```

Body:

```text
op::internal_transfer()
query_id:uint64
amount:Coins
from:MsgAddress              ; original sender owner address
response_address:MsgAddress
forward_ton_amount:Coins
forward_payload:Either Cell ^Cell
```

The message also carries `StateInit` for the recipient jetton wallet in the
local legacy implementation. This is a major size/cell overhead and was
explicitly suspected in the chat as a reason one-shard jetton throughput hits
block-size limits early.

### Optional `transfer_notification`

Destination:

```text
recipient owner address
```

Body:

```text
op::transfer_notification()
query_id:uint64
amount:Coins
from:MsgAddress
forward_payload:Either Cell ^Cell
```

This exists only if `forward_ton_amount > 0`.

### Optional `excesses`

Destination:

```text
response_destination
```

Body:

```text
op::excesses()
query_id:uint64
```

This exists only if there is leftover message value and the response address is
not `addr_none`.

## Stage-by-stage pipeline

### Stage 0: Application prepares the transfer

The app or wallet UI must know:

- sender owner wallet address
- recipient owner address
- jetton master address
- sender jetton wallet address
- sender jetton balance
- sender TON wallet `seqno`
- sender TON wallet TON balance
- recipient jetton wallet address, if it wants to display or pre-check it
- approximate TON value needed for the transfer
- `forward_ton_amount` and `forward_payload`
- `response_destination`

Common calls:

- run `get_wallet_address(owner)` on the jetton master
- run `get_wallet_data()` on the sender jetton wallet
- run wallet `seqno()` on the user TON wallet
- fetch account state/balance

What can go wrong:

- stale `seqno`
- wrong subwallet id
- wrong recipient owner address
- deriving the wrong jetton wallet address because the wrong master or wallet
  code is used
- sender has jettons but not enough TON to pay transfer costs
- recipient wallet does not exist yet, making the transfer heavier because
  `StateInit` will be attached
- app underestimates forwarding/storage/gas costs
- custom jetton code has extra behavior not captured by a generic estimate
- indexer/wallet state is behind the chain and gives stale balances or seqno

Performance relevance:

- The user-side step is not the chain bottleneck, but stale state causes
  retries, duplicate externals, and user-visible delays.
- Batch senders using one wallet are limited by wallet `seqno` sequencing unless
  they use wallet features designed for batching.

### Stage 1: Wallet constructs the external message

The wallet signs an external message to the user's TON wallet contract.

For wallet v4-like contracts:

```text
signature
subwallet_id
valid_until
seqno
op
mode/ref pairs for outgoing internal messages
```

The outgoing internal message to the sender jetton wallet is placed as a ref
inside the external body.

Node-visible form:

```text
Message Any
  info = ext_in_msg_info$10
  dest = user TON wallet address
  init = optional StateInit
  body = wallet external body
```

What can go wrong:

- BOC is malformed
- external message is too large or too deep
- destination is not a valid internal address
- body format does not match the wallet code
- signature is invalid
- `valid_until` is too close and expires before inclusion
- `seqno` is old, duplicate, or too far in the future
- external deploy of wallet is wrong or invalid

Performance relevance:

- External messages are relatively small compared with jetton internal messages,
  but high-rate senders can overload liteservers, pre-check, mempool, and
  broadcast paths.
- The external is addressed to the user's TON wallet shard, not directly to the
  jetton wallet.

### Stage 2: The app submits to a liteserver

Local tonlib path:

- `raw_createAndSendMessage` creates an external message from destination,
  optional initial state, and body.
- `TonlibClient::do_request(int_api::SendMessage)` serializes it.
- tonlib sends `liteServer_sendMessage`.

Liteserver path:

- `LiteQuery::perform_sendMessage` receives the BOC.
- It calls `ValidatorManager::new_external_message_query`.
- On success it returns `liteServer_sendMsgStatus(1)`.

Important semantic point:

`sendMsgStatus(1)` means the node accepted the external message for its current
processing path. It does not mean:

- the external transaction is in a block
- the user's wallet seqno has changed on-chain
- the sender jetton wallet has run
- the recipient jetton balance has changed
- an indexer has seen the transfer

What can go wrong:

- liteserver not synced
- liteserver overloaded
- duplicate `sendMessage` suppressed by cache
- pre-check fails
- liteserver accepts locally but does not manage to propagate broadly
- client times out and resubmits the same external to another node
- different liteservers have different local mempool states

Performance relevance:

- Chat measurements on June 13-14 separated liteserver ingress from chain
  throughput: both LS variants could accept about 8k externals/s before severe
  degradation; Go LS with two connections reached about 13k accepts/s.
- This does not imply 8k or 13k jTPS. It only says ingress can be high under
  that test. Actual jTPS is limited later by collation, block limits, jetton
  TVM work, message queues, and validation.

### Stage 3: External message validation and mempool admission

The manager calls:

```text
ExtMessagePool::check_add_external_message(data, priority, add_to_mempool)
```

The path does:

1. require last masterchain state to be available
2. parse BOC into an external message
3. check external message size and depth from config limits
4. require `ext_in_msg_info$10`
5. validate `Message Any`
6. extract destination workchain and address
7. compute raw hash and normalized hash
8. rate-limit recent checked messages per destination account
9. fetch destination account state
10. if the destination code is a known wallet, parse wallet `seqno` and
    `valid_until`
11. run TVM pre-check to `accept_message()`
12. add to local mempool if this node is a validator
13. allow broadcast only when wallet seqno ordering permits it

Hard-coded local limits in the current ext-message pool:

```text
MAX_EXT_MSG_PER_ADDR_TIME_WINDOW = 10 seconds
MAX_EXT_MSG_PER_ADDR = 30
PER_ADDRESS_LIMIT = 256
SOFT_MEMPOOL_LIMIT = 1024
MAX_WALLET_SEQNO_DIFF = 16
Mempool message delete TTL = 600 seconds
```

There is also `opts_->max_mempool_num()`, defaulted in this tree to a high value
unless configured otherwise.

Wallet-specific ordering:

- If `msg_seqno < wallet_seqno`, reject as too old.
- If `msg_seqno - wallet_seqno > 16`, reject as too new.
- If the same future `seqno` is already known for the wallet, reject duplicate.
- A future-seqno message is only allowed to broadcast when the pool knows the
  previous sequence of messages up to it.

What can go wrong:

- not synced, no masterchain state
- invalid message structure
- destination account unavailable or cannot be unpacked
- wallet code not recognized, falling back to generic pre-check
- wallet `valid_until` in the past
- old or duplicate `seqno`
- future `seqno` too far ahead
- too many messages to the same account in a 10s window
- per-address mempool limit reached
- mempool full
- TVM pre-check does not reach `accept_message()`
- local node accepts an external but it later becomes stale because another
  external with the same seqno was included elsewhere

Performance relevance:

- This layer is one of the current problem areas from the chat. There were
  reports of externals stopping under load and of externals stuck in mempools.
- The June chat summary says there were at least four bugs causing externals to
  stop being accepted under load: one dying liteserver issue and three inside
  the node. One planned fix was the new mempool.
- Future-seqno support helps batching, but it also creates more state and
  ordering complexity in the mempool.

### Stage 4: External message broadcast

If the query path accepts the external, the full node sends it to the shard
overlay:

```text
FullNodeImpl::send_ext_message
FullNodeShardImpl::send_external_message
tonNode_externalMessageBroadcast
```

Receiving full nodes process:

```text
FullNodeShardImpl::process_external_message_broadcast
ValidatorManagerImpl::new_external_message_broadcast
ExtMessagePool::check_add_external_message
```

There is duplicate suppression by hash at the full-node shard layer.

What can go wrong:

- broadcast disabled
- shard overlay not active
- duplicate broadcast dropped
- recipient node not synced enough to pre-check
- broadcast reaches too few validators before collation
- private/custom overlay and public overlay behavior diverge
- liteserver returns success before propagation is robust

Performance relevance:

- External messages are small, but at high rates broadcast still consumes CPU,
  queue capacity, and duplicate-check resources.
- The chat mentions smart broadcast/rebroadcast techniques as a way to get more
  externals into the network.

### Stage 5: Collator installs an external queue

At collation start, the collator asks the manager for external messages for its
shard. The ext-message pool:

- computes the shard key range
- slices each priority treap by that range
- creates a snapshot
- pushes messages randomly into the collator queue
- keeps a live callback if the collator is allowed to wait for new externals

Local code:

```text
Collator::start_up
ValidatorManager::get_external_messages
ExtMessagePool::install_collator_queue
```

What can go wrong:

- message is in another shard's range
- message expired
- message inactive due to postpone
- collator queue backpressure
- cancellation because collation attempt changes
- priority behavior starves normal messages under special load

Performance relevance:

- Selection is per shard and per priority.
- The queue is not the same thing as final inclusion. A message can be selected
  and still be rejected or delayed by transaction execution or block limits.

### Stage 6: Collator imports external messages

The relevant loop is:

```text
Collator::process_external_and_new_messages
Collator::process_inbound_external_messages
Collator::process_external_message
```

The loop alternates:

1. import inbound external messages while limits allow
2. process newly generated internal messages
3. optionally wait for new external messages

The collator stops importing externals when:

- external processing is disabled
- collation attempt index is at least 2
- block soft limits are reached
- external-message timeout is reached
- block is too full
- out-message queue is too large and the message is not high priority
- the external is invalid, duplicate, or not for this shard

When an external is attempted:

1. `register_external_message` validates the cell, type, libs, destination, and
   duplicate status.
2. `process_external_message` creates an ordinary transaction for the destination
   account.
3. If the transaction succeeds, the external is inserted into `InMsgDescr` as
   `msg_import_ext`.
4. If the transaction is rejected by the account, the external can be delayed.
5. Bad externals are deleted from mempool; delayed externals may be postponed.

What can go wrong:

- block is already near soft byte/gas/lt limits
- external reaches wallet but fails in compute before `accept_message()`
- action phase fails after `accept_message()`
- external becomes old because another seqno was included first
- queue contains many messages that are rejected and waste time
- external is delayed repeatedly, then deleted
- collator stops because medium timeout is reached
- block has too much out-message queue pressure and skips ordinary externals

Performance relevance:

- The April 7 chat observation fits here: with 80 external/s jetton spam, only
  about 60/s passed, `do_collate` was around 395 ms, and no `want_split` was
  observed.
- The collator's overload check uses block limit classification and long
  collation timers. It logs `wait_externals`, `do_collate`, and `total`.
- If time is spent doing collation rather than waiting for externals, the block
  can be classified as overloaded by time.

### Stage 7: User wallet external transaction

The external transaction is created by:

```text
Collator::create_ordinary_transaction
Collator::impl_create_ordinary_transaction
Transaction::prepare_compute_phase
Transaction::prepare_action_phase
```

For a wallet v4-style transfer:

1. input message is unpacked
2. account state is loaded
3. storage phase collects storage fees
4. compute phase runs `recv_external`
5. wallet checks signature, subwallet, valid-until, and seqno
6. wallet calls `accept_message()`
7. wallet increments seqno
8. wallet emits internal messages via `send_raw_message`
9. action phase checks those messages and computes forwarding fees
10. transaction is serialized and committed
11. outgoing messages are registered as `NewOutMsg`

State changes:

- user wallet `seqno` increases
- user wallet TON balance decreases by gas, action fees, and forwarded value
- at least one internal message to the sender jetton wallet is created

What can go wrong:

- external rejected before `accept_message()`: no useful transaction should be
  included
- external accepted but action fails: user pays gas, outgoing transfer may not be
  created
- not enough TON for outgoing message value and fees
- invalid outbound message generated by wallet or app
- too many outgoing refs in a batch
- later messages in a batch skip or fail depending on send mode
- wallet `seqno` increases, causing retries with old seqno to fail

Performance relevance:

- One wallet external can emit many messages, especially with wallet v5/batch
  patterns. This is useful for mass senders but can create large action lists and
  many internal messages.
- The chat mentioned product reports around W5 batch sending and seqno behavior;
  this is part of the same external/mempool/action surface.

### Stage 8: Action phase creates the internal `transfer`

`Transaction::try_action_send_msg` takes the raw message requested by the
contract and rewrites/validates it.

It checks:

- send mode flags
- message structure
- source address
- destination address
- libraries in `StateInit`
- message bits/cells/depth
- available funds
- forwarding fees
- IHR-related fields, depending on global version

It computes forwarding fees from config:

```text
msg_fwd_fees =
  lump_price + ceil((bit_price * msg.bits + cell_price * msg.cells) / 2^16)
```

For internal messages, it:

- computes first-part and remaining forwarding fees
- rewrites `created_lt` and `created_at`
- deducts fees and value from the sender account
- stores the new outbound message in the action phase

What can go wrong:

- message too large
- message too deep
- too many cells
- invalid destination
- not enough grams to attach value plus fees
- send mode pays fees from the wrong source
- `StateInit` or body does not fit and needs moving into refs
- action skip mode hides failed sends

Performance relevance:

- Jetton transfers tend to create relatively large internal messages because
  the recipient jetton wallet `StateInit` may be attached.
- Message size increases forwarding fees and block bytes.
- More cells also increase serialization/deserialization and storage-stat work.

### Stage 9: New internal messages are processed or queued

After the user wallet transaction creates the internal `transfer`, the collator
registers it in the priority queue of new messages.

Then:

```text
Collator::process_new_messages
Collator::process_one_new_message
```

If the destination is in the same shard and limits permit, the message is
processed immediately in the same block.

If the destination is outside the shard, or if the block is full, or if message
deferring applies, it is enqueued.

What can go wrong:

- destination is another shard, so the transfer continues in a later block
- current block becomes full and remaining messages are queued
- out-message queue grows
- dispatch queue/deferred-message logic delays processing
- forwarding fees are insufficient for later routing
- shard split/merge changes queue behavior

Performance relevance:

- For same-shard chains, a single block can execute multiple legs of the jetton
  transfer. For cross-shard chains, each leg can take additional blocks.
- Under heavy jetton load, the block can spend much of its capacity on the
  message cascade rather than on new externals.

### Stage 10: Sender jetton wallet transaction

The sender jetton wallet receives op `transfer`.

Contract-level steps from the local jetton wallet:

1. ignore empty bodies
2. parse sender address from the inbound internal message
3. parse forwarding fee from inbound message header for estimation
4. parse op
5. load `query_id`
6. load jetton amount
7. load recipient owner address
8. enforce same workchain in the local legacy contract
9. load stored jetton wallet data
10. subtract amount from jetton balance
11. require inbound sender address equals stored owner address
12. require jetton balance remains non-negative
13. build recipient jetton wallet `StateInit`
14. compute recipient jetton wallet address
15. load response address, custom payload, forward TON amount, and forward payload
16. build an `internal_transfer` message
17. attach recipient wallet `StateInit`
18. check incoming TON value is enough for forwarding, gas, and storage reserve
19. send `internal_transfer` with mode 64
20. save reduced jetton balance

State changes:

- sender jetton wallet jetton balance decreases
- sender jetton wallet TON balance changes due to fees and inbound value
- internal message to recipient jetton wallet is created

What can go wrong:

- inbound message was not from the stored owner address
- jetton balance insufficient
- recipient owner address is invalid or unsupported by this jetton code
- incoming TON value is too small
- forward-fee estimate is wrong
- action phase fails to send the `internal_transfer`
- custom payload or forward payload makes the message too large
- contract code is custom and does additional checks

Performance relevance:

- This is where the recipient wallet `StateInit` is computed and attached.
- The chat specifically suspected block-size pressure because `StateInit` is
  attached to transfers.
- This transaction performs TVM work, persistent data load/save, address
  derivation, cell hashing, and emits a large message.

### Stage 11: Routing to recipient jetton wallet

If the recipient jetton wallet is not in the same shard, the internal transfer
is queued and routed.

Node-level structures:

- `MsgEnvelope`
- `OutMsgDescr`
- `OutMsgQueue`
- `InMsgDescr`
- processed-up-to information
- dispatch queue for deferred messages

The collator uses hypercube routing to choose next hop and forwarding-fee
partitioning.

What can go wrong:

- out-message queue becomes large
- queue operations become expensive
- destination shard is busy or not importing quickly
- forwarding fee remaining is insufficient
- message waits behind other messages
- split/merge changes the shard path
- block limits prevent importing all available messages

Performance relevance:

- Cross-shard jetton transfers are a better real-world benchmark than same-shard
  toy transactions because they stress queue import/export and shard references.
- Out-message queue growth can feed back into external processing. The collator
  can skip low-priority externals if the out-message queue is too large.

### Stage 12: Recipient jetton wallet transaction

The recipient jetton wallet receives op `internal_transfer`.

If the account is uninitialized and valid `StateInit` is attached, the account
can be activated and then execute.

Contract-level steps:

1. load current recipient jetton wallet data
2. load `query_id`
3. load jetton amount
4. add amount to jetton balance
5. load original sender owner address
6. load response address
7. verify inbound sender is either the master or the expected sender jetton
   wallet address for the original sender owner
8. load forward TON amount
9. compute TON balance before the message
10. reserve storage and gas
11. if `forward_ton_amount > 0`, build and send `transfer_notification`
12. if response address exists and leftover value remains, build and send
    `excesses`
13. save increased jetton balance

State changes:

- recipient jetton wallet may become active
- recipient jetton wallet jetton balance increases
- optional notification and excess messages are created

What can go wrong:

- `StateInit` does not match the destination address
- incoming sender is not the valid sender jetton wallet
- incoming TON value is insufficient for storage/gas/notification
- notification message cannot be sent
- excess message cannot be sent
- forward payload is too large
- recipient owner is a contract that rejects notification
- recipient owner is uninitialized and notification is nobounce

Performance relevance:

- Account activation is heavier than updating an already active account.
- The transfer can fan out into two more messages.
- Notifications and excesses add transaction count and queue pressure.

### Stage 13: Optional notification transaction

If `forward_ton_amount > 0`, recipient owner receives
`transfer_notification`.

For a regular wallet contract:

- internal messages are often ignored unless they implement plugin behavior
- indexers and wallet apps still use the transaction/message to display the
  incoming transfer

For a smart contract owner:

- this notification may trigger application logic

What can go wrong:

- recipient contract rejects or ignores the notification
- the notification is nobounce, so failures may not return value
- extra work causes additional block load
- UI waits for notification even though recipient jetton balance changed earlier

Performance relevance:

- If benchmarks set `forward_ton_amount > 0`, they measure a heavier workload
  than a pure balance update.
- User-visible wallet behavior may depend on notification/indexer handling, not
  only on recipient jetton wallet state.

### Stage 14: Optional excess transaction

If the recipient jetton wallet has leftover TON value and a response address is
set, it sends `excesses`.

What can go wrong:

- response address invalid or unavailable
- message delayed across shards
- excess transaction adds load

Performance relevance:

- Excesses are part of realistic transfers because senders usually attach more
  TON than exactly required.
- They increase messages per jetton transfer.

### Stage 15: Bounce and failure handling

The local jetton wallet has `on_bounce`.

If an `internal_transfer` or `burn_notification` bounces back to the sender
jetton wallet, it:

1. checks the bounced op
2. loads `query_id`
3. loads jetton amount
4. adds the jetton amount back to balance
5. saves data

Important cases:

- If the user wallet's internal `transfer` to sender jetton wallet never causes
  sender jetton wallet compute to succeed, sender jetton balance is unchanged.
- If sender jetton wallet debits balance but its outgoing `internal_transfer`
  later bounces, `on_bounce` restores balance.
- The contract comments explicitly try to avoid relying on action-phase failure
  for important checks, because a failure in action phase may not produce the
  bounce behavior the token accounting needs.

What can go wrong:

- bounce path itself fails
- messages are nobounce
- action skip modes hide failures
- indexers show temporary state before bounce restore
- delayed bounce creates confusing UX

Performance relevance:

- Failed transfers still consume gas, block space, and possibly multiple
  transactions.

### Stage 16: Block candidate, validation, and consensus

After collation:

- the block contains account blocks and transactions
- `InMsgDescr` records imported external/internal messages
- `OutMsgDescr` records exported/enqueued messages
- account state updates are represented as Merkle updates
- value flow accounts for imported/exported value, fees, burned, created, minted
- block candidate data and collated data are sent through validator consensus
- validators validate by replaying/checking the block

Block limit categories from `block.tlb`:

```text
bytes
gas
lt_delta
collated_data
imported_msg_queue
```

Each has underload, soft, and hard limits.

There are also consensus-config maximum block and collated-data sizes checked by
the collator before returning the candidate.

What can go wrong:

- block exceeds bytes limit
- block exceeds gas limit
- block exceeds lt-delta limit
- collated data too large
- imported message queue limit reached
- validation is slower than collation target
- candidate broadcast/consensus is slow
- shard block not referenced/finalized quickly enough for user UX

Performance relevance:

- The chat says current megabyte blocks impose a hard cap below about 600 jTPS
  in one shard.
- The same chat summary says increasing limits or making blocks more frequent
  eventually becomes necessary.
- Optimizing TVM/collator alone is not enough if block bytes are the first hard
  limiter.

### Stage 17: Shard split/merge behavior

The collator updates overload/underload history and sets `want_split` or
`want_merge` based on:

- block limit class
- dispatch queue pressure
- long collation time
- out-message queue constraints

Relevant local logic:

```text
Collator::check_block_overload
```

It logs:

```text
gas
lt_delta
size_estimate
collated_data_size_estimate
wait_externals
do_collate
total
```

What can go wrong:

- heavy external load makes collation slow but blocks do not hit the expected
  split signal
- blocks are "empty enough" by size but expensive by time
- split signal is delayed relative to short benchmark bursts
- out-message queue is too large to split safely
- shard count oscillates under unstable load

Performance relevance:

- April chat: 80 external/s jetton spam, about 60/s passed, no `want_split`, and
  `do_collate` around 395 ms.
- May chat: shard count and load were unstable at higher jetton spam rates.
- Reference workload should record shard count, `want_split`, `before_split`,
  and actual split timing, not only TPS.

### Stage 18: Liteserver, indexer, and wallet visibility

After blocks are produced, users see the transfer through:

- wallet polling account state
- wallet polling transactions
- liteserver account proofs
- indexer ingestion
- API serving the wallet app
- non-final interfaces if used

Different UIs may consider different events as "done":

- external accepted by liteserver
- sender wallet seqno changed
- sender jetton balance decreased
- recipient jetton balance increased
- transfer notification observed
- shard block finalized by masterchain
- indexer API shows the transfer

What can go wrong:

- liteserver accepts but transaction is not included yet
- transaction included but liteserver/indexer is behind
- sender seqno changes but jetton leg is delayed
- recipient balance changes but notification/excess is delayed
- wallet app waits for a signal that is not the earliest true signal
- second message from the same wallet fails because app does not cache or update
  seqno correctly

Performance relevance:

- May chat: under higher testnet loads, the network could still work while
  infrastructure did not. TON transfers from a wallet became visible only after
  long liteserver/indexer catch-up.
- For user-facing performance, "chain accepted the transfer" and "wallet UI
  displays the transfer" must be measured separately.

## Per-step problem map

| Step | Main object | Performance limit | Correctness/UX failure mode |
| --- | --- | --- | --- |
| App preparation | off-chain state | stale APIs, runmethod latency | wrong seqno, wrong jetton wallet, bad fee estimate |
| External construction | BOC, wallet body | size/depth, signing throughput | invalid signature, expired `valid_until` |
| Liteserver submit | `liteServer_sendMessage` | LS CPU, duplicate cache, network | status 1 mistaken for finality |
| Pre-check | ext-message pool | account fetch, TVM to accept, per-address limits | old/future/duplicate seqno |
| Mempool | validator local pool | memory, per-address limit, TTL, ordering | stuck external, inconsistent local pool |
| Broadcast | shard overlay | broadcast bandwidth/CPU | too few validators see the external |
| Collator external import | external queue | soft block limits, timeout | external delayed or filtered |
| User wallet transaction | wallet account | TVM, action list, TON fees | seqno increments but send fails |
| Internal message action | action phase | message size, fwd fee, cells | outbound message invalid or underfunded |
| Sender jetton wallet | jetton wallet account | TVM, state load/save, StateInit build | insufficient jettons or TON |
| Cross-shard routing | out-message queue | queue size, proofs, import limits | delayed recipient leg |
| Recipient jetton wallet | activation/update | StateInit size, storage/gas, notifications | no notification, bounce, underfunded msg |
| Notification/excess | optional messages | extra transactions/messages | UI waits for delayed optional leg |
| Validation/consensus | candidate block | block/collated data, validation, broadcast | block rejected or slow finality |
| Indexing/UI | LS/indexer/API | catch-up lag | user sees transfer late |

## Why jetton transfer load is heavy

A minimal native TON transfer from one wallet to another is often:

- one external message
- one wallet transaction
- one internal message
- one recipient transaction, if recipient is a contract that does anything

A jetton transfer is heavier:

- external to owner wallet
- internal to sender jetton wallet
- sender jetton wallet computes recipient jetton wallet address
- sender jetton wallet often attaches recipient wallet `StateInit`
- internal to recipient jetton wallet
- recipient jetton wallet may activate
- optional notification
- optional excess
- each leg has fees, cells, bits, transaction phases, and queue entries

The local chat specifically identifies the attached jetton wallet `StateInit` as
a suspected reason that blocks hit size limits early.

## Current known performance state from the chat

This is a synthesis of direct local chat mentions, not an external-source claim.

### Targets

The team wants a user-visible transfer that feels subsecond or close to it, and
also wants high sustained per-shard jetton throughput.

Numbers mentioned:

- 0.5s operation target in strategic discussion
- first practical target of about 1k jTPS
- longer-term recollection of 1e4 jTPS per shard, roughly 300 Mbps goodput

### Benchmark definitions were initially mixed

The chat uses several terms that must not be mixed:

- `external/s`: external messages accepted or sent per second
- `EPS`: external messages per second in some test reports
- `TPS`: raw account transactions per second or block transaction rate
- `jTPS`: user-level jetton transfers per second
- "attempts/s": off-chain spam attempts per second
- "accepted by liteserver": not the same as "included in a block"
- "visible in wallet": not the same as "included in a block"

For future work, every measurement should state exactly which one it is.

### Load generators

The chat mentions:

- `testtpsbot /jetton <freq>`
- devnet `jetton spam`
- external spam without transfer
- internal self-spam contracts
- 16-contract shard spam
- smart broadcast/rebroadcast techniques
- local `bench-jetton-tps` branch/workflow

Reference workload converged toward:

```text
externals -> jetton transfers between shards
```

with many accounts, because that resembles user load better than a single
contract pinging itself.

### One-shard numbers

Reported:

- around 94 jetton sends/s caused split, roughly 400 TPS
- one local benchmark around 90 jTPS in mainnet-like configuration
- testnet one-shard number around 94 jTPS, matching local benchmark
- current testnet in-memory around 200 jTPS in one summary
- optimized non-in-memory collator around 315 jTPS
- optimized in-memory around 480 jTPS
- hard block-size cap below about 600 jTPS in one shard

Interpretation:

- There is a real one-shard execution/size bottleneck well below the desired
  1k jTPS target.
- In-memory database helps, but does not by itself solve the block-size and TVM
  pipeline problem.

### Multi-shard/testnet numbers

Reported on May 9:

- 720 TPS, 144 EPS, 150 attempts/s, 2 shards stable
- 880 TPS, 177 EPS, 200 attempts/s, 2 shards stable, tried to split to 3-4
- 1400 TPS, 293 EPS, 800 attempts/s, about 4 shards, unstable
- 1800 TPS, 438 EPS, 1600 attempts/s, about 4 shards, unstable

Interpretation:

- More shards improve total throughput, but instability and infra lag become
  major factors.
- TPS/EPS/jTPS were not always separated, so these numbers need careful
  reproduction with clearer metrics.

### External acceptance problems

Reported:

- 200 TPS jetton spam hurt testnet in April.
- Stressing with 2000 TPS externals could make the network stop accepting new
  externals.
- At least four bugs were suspected around external acceptance under load.
- One fix area was the new mempool.
- There were reports of external messages stuck in one node's mempool and not
  propagated while other nodes behaved differently.

Interpretation:

- External ingress is a separate subsystem from jetton contract execution.
- A high liteserver accepts/s number does not prove reliable block inclusion.
- Mempool correctness and propagation are first-class performance work.

### Block-size problem

Reported:

- Current megabyte blocks give a hard cap below about 600 jTPS in one shard.
- A sender remembered that all transfers attach jetton wallet `StateInit`.

Interpretation:

- If the block byte limit is binding, optimizing TVM alone has limited upside.
- Options include smaller messages, avoiding repeated `StateInit` where safe,
  raising block limits, or producing blocks more often.
- Raising limits can move the bottleneck to validation, broadcast, collated data,
  database IO, or indexers.

### Infra problem

Reported:

- Under some high-load tests, network kept producing, but liteservers/indexers
  lagged badly.
- Wallet-visible results could lag far behind chain progress.
- Tonkeeper reportedly behaved acceptably in one lower-load test, while other
  paths lagged.

Interpretation:

- Chain throughput and product UX are separate measurements.
- Indexer/liteserver catch-up must be part of any end-to-end benchmark.

## Limits to know by layer

### Wallet/user limits

- wallet `seqno` sequencing
- `valid_until` expiration
- wallet version/body format
- batch size/action count
- available TON balance
- app's fee estimate

### External-message limits

From local code and configs:

- BOC must deserialize to exactly one root
- external must be `ext_in_msg_info$10`
- cell level must be zero
- size and depth are checked against config external-message limits
- destination must be a valid internal address
- normalized hash is used for applied-message cleanup

Ext-message pool constants:

- 30 checked messages per address per 10s window
- 256 mempool messages per address per priority
- future wallet seqno at most 16 ahead
- 600s mempool TTL
- soft mempool behavior changes around 1024 messages

### Transaction limits

- storage fees and due payment
- gas limit and gas credit
- action list size
- message size in bits/cells
- message Merkle depth
- available account balance
- forwarding fee pricing
- account state limits

### Block limits

From `block.tlb`:

- bytes
- gas
- lt delta
- collated data
- imported message queue

Each has underload, soft, and hard classes.

Consensus also has max block size and max collated-data size checks.

### Queue/routing limits

- out-message queue size
- dispatch queue behavior
- per-initiator/per-account dispatch limits
- import queue limits
- forwarding-fee remaining
- split/merge timing and prefix behavior

### Infrastructure limits

- liteserver `sendMessage` accepts/s
- liteserver account-state and transaction-query latency
- full-node broadcast capacity
- validator database IO
- indexer ingestion rate
- API cache and freshness
- wallet polling logic

## What to instrument in future benchmarks

A useful jetton benchmark should record every layer separately.

### Input/load generator

Record:

- requested attempts/s
- actual submitted externals/s
- number of funding wallets
- wallet version
- accounts count
- jetton wallets predeployed or not
- recipient distribution
- forward TON amount
- forward payload size
- response destination policy
- same-shard or cross-shard distribution

### Liteserver ingress

Record:

- `sendMessage` requests/s
- success/error/timeout counts
- latency percentiles
- duplicate-cache drops
- pre-check errors by type
- raw hash and normalized hash if debugging duplicates

### Mempool

Record:

- `total.ext_msg_check ok/error`
- mempool size by priority
- per-address limit hits
- old seqno errors
- future seqno errors
- duplicate seqno errors
- expired valid-until errors
- delayed/postponed/deleted externals
- applied cleanup requested/deleted

### Collator

Record from `validatorStats.collatedBlock` and logs:

- `ext_msgs_total`
- `ext_msgs_filtered`
- `ext_msgs_accepted`
- `ext_msgs_rejected`
- `wait_externals_time`
- `total_time`
- `work_time`
- `cpu_work_time`
- `check_load_do_collate_time`
- `check_load_total_time`
- estimated block bytes
- estimated collated data bytes
- gas
- lt_delta
- out-message queue size
- load fraction for externals
- load fraction for new messages
- limit logs

### Transactions

For sampled transfers, trace:

- user wallet external transaction LT/hash
- sender jetton wallet transaction LT/hash
- recipient jetton wallet transaction LT/hash
- notification transaction if any
- excess transaction if any
- gas used per transaction
- action fees and forward fees
- message sizes/cell counts
- whether recipient wallet was activated
- bounce path if failed

### Shards

Record:

- shard count over time
- shard of user wallets
- shard of sender jetton wallets
- shard of recipient jetton wallets
- `want_split`
- `before_split`
- actual split time
- merge events
- out-message queue size by shard

### Validation and consensus

Record:

- block candidate size
- collated data size
- validation time
- candidate broadcast time
- consensus/finalization latency
- masterchain reference latency for shard blocks

### Indexer and wallet UX

Record:

- time until liteserver sees sender wallet seqno changed
- time until sender jetton balance decreases
- time until recipient jetton balance increases
- time until notification appears
- time until indexer/API exposes transfer
- wallet UI completion time
- lag under sustained load

## A precise timeline for one successful transfer

This is the ideal happy path with no bounces.

```text
T0  App has current sender wallet seqno and sender jetton balance.

T1  User signs external to sender TON wallet.

T2  App sends BOC via liteserver sendMessage.

T3  Liteserver pre-checks external:
    - parse BOC
    - check external message type
    - fetch sender TON wallet account
    - parse wallet seqno/valid_until
    - run TVM until accept_message
    - accept into local path

T4  Full node broadcasts external on sender wallet shard overlay.

T5  Validators receive and pre-check external, add to local mempool.

T6  Collator for sender wallet shard selects external.

T7  User wallet transaction executes:
    - wallet accepts external
    - seqno increments
    - internal transfer message to sender jetton wallet is emitted

T8  If sender jetton wallet is same shard and limits allow:
    sender jetton wallet transaction executes in same block.
    Otherwise message is queued and imported by its shard later.

T9  Sender jetton wallet transaction executes:
    - checks owner
    - subtracts jettons
    - computes recipient jetton wallet address
    - attaches recipient wallet StateInit
    - sends internal_transfer

T10 If recipient jetton wallet is same shard and limits allow:
    recipient jetton wallet transaction executes in same block.
    Otherwise internal_transfer is queued and imported by recipient shard later.

T11 Recipient jetton wallet transaction executes:
    - activates if needed
    - verifies sender jetton wallet
    - adds jettons
    - optionally sends notification
    - optionally sends excess

T12 Notification and excess messages execute or queue.

T13 Shard blocks are validated and finalized/referenced.

T14 Liteservers/indexers observe new transactions and account states.

T15 Wallet UI shows final state.
```

## Why "subsecond jetton transfer" is ambiguous

There are several possible definitions:

1. liteserver accepted the external
2. sender wallet seqno incremented in a block
3. sender jetton wallet balance decreased
4. recipient jetton wallet balance increased
5. notification reached recipient owner
6. shard block is referenced/finalized enough
7. wallet app displays the transfer

These can differ by milliseconds, seconds, or under overload much longer.

For engineering, measure all of them and choose which one is the product SLA.

## Main optimization hypotheses

### Reduce bytes per jetton transfer

Reason:

- current megabyte blocks cap one-shard jTPS below target
- attached recipient wallet `StateInit` is likely a major byte/cell contributor

Ideas to evaluate:

- predeploy recipient jetton wallets in benchmark to isolate StateInit cost
- compare transfer with and without wallet activation
- measure exact cells/bits of each message
- evaluate compressed/shared code/library strategies
- evaluate protocol or contract changes that avoid attaching full code each time

Tradeoff:

- Removing `StateInit` makes first receipt harder or requires prior deployment.
- Shared libraries or protocol changes may add compatibility risk.

### Improve external-message mempool

Reason:

- chat identified external acceptance bugs and mempool issues
- future-seqno/batching is important for mass senders

Ideas to evaluate:

- reproduce stuck external reports
- compare old and new mempool under same load
- track normalized hash cleanup after applied blocks
- track per-address sequencing behavior under W5 batches
- stress with many accounts vs one account

Tradeoff:

- More permissive future-seqno handling improves batching but can increase
  memory, ordering, and DoS surface.

### Improve collator transaction throughput

Reason:

- local optimized collator numbers are below target
- jetton transfer path does real TVM/account/cell work

Ideas to evaluate:

- profile sender jetton wallet transaction separately from recipient activation
- profile storage-stat/account-dict updates
- profile StateInit hashing and message serialization
- profile non-in-memory vs in-memory database paths
- isolate block-size limit by raising bytes limit in local benchmark

Tradeoff:

- If bytes are binding, CPU optimization may not increase final jTPS much.

### Tune block limits and block frequency

Reason:

- current block byte limit appears to bind below desired jTPS
- faster blocks improve latency but may worsen overhead and infra load

Ideas to evaluate:

- one-shard benchmark with varying block byte limit
- one-shard benchmark with varying block interval
- measure validation and broadcast cost after raising limits
- measure collated-data growth

Tradeoff:

- Larger/faster blocks can break liteservers, indexers, weaker validators, or
  network broadcast assumptions.

### Stabilize sharding behavior under jetton load

Reason:

- chat observed missing or delayed split signals and unstable shard counts

Ideas to evaluate:

- record `want_split`, overload history, block limit class, and out queue size
  during jetton load
- run long enough for split timings to matter
- compare external-only spam vs full jetton spam
- separate size overload from time overload

Tradeoff:

- Aggressive splitting can improve throughput but increases cross-shard traffic
  and operational complexity.

### Separate chain success from infra visibility

Reason:

- chat saw network work while liteservers/indexers lagged

Ideas to evaluate:

- run benchmarks with direct validator stats and independent indexer stats
- measure liteserver catch-up lag
- measure wallet UI completion separately from block inclusion
- use non-final candidate indexing where relevant

Tradeoff:

- Product UX may need infra changes even if core chain throughput improves.

## Minimal glossary

`external message`

Message from outside the blockchain to a contract. In this pipeline, it goes to
the user's TON wallet.

`internal message`

Message from one on-chain account to another. Jetton transfer legs are internal
messages after the initial wallet external.

`TON wallet`

User-controlled wallet contract that stores TON and emits internal messages
after signature and seqno checks.

`jetton master`

Contract that stores jetton metadata, total supply, and wallet code. Used to
derive per-owner jetton wallet addresses.

`jetton wallet`

Per-owner, per-jetton contract that stores that owner's jetton balance.

`seqno`

Wallet sequence number. Prevents replay and orders external messages.

`valid_until`

Timestamp after which an external wallet message is invalid.

`accept_message`

TVM instruction used by a contract to accept an external message and agree to
pay gas from account balance.

`StateInit`

Initial code/data package that can activate an uninitialized account if it
matches the destination address.

`forward_ton_amount`

TON amount forwarded by recipient jetton wallet to recipient owner in the
optional notification.

`forward_payload`

Payload included in the notification.

`response_destination`

Address to receive leftover TON as `excesses`.

`jTPS`

Jetton transfers per second. A user-level metric. Not the same as raw
transactions per second.

`EPS`

External messages per second, in the chat context. Must be defined per
benchmark.

`collated data`

Additional data shipped with a block candidate so validators can validate it.

`out-message queue`

Shard queue holding messages that must be imported by another shard or later
block.

## Local source map

Contract-side references:

- `crypto/func/auto-tests/legacy_tests/jetton-wallet/jetton-wallet.fc`
- `crypto/func/auto-tests/legacy_tests/jetton-wallet/imports/op-codes.fc`
- `crypto/func/auto-tests/legacy_tests/jetton-wallet/imports/jetton-utils.fc`
- `crypto/func/auto-tests/legacy_tests/jetton-minter/jetton-minter.fc`
- `crypto/func/auto-tests/legacy_tests/wallet-v4/wallet-v4-code.fc`
- `crypto/smartcont/wallet3-code.fc`

Node-side references:

- `crypto/block/block.tlb`
- `crypto/block/transaction.cpp`
- `crypto/block/transaction.h`
- `validator/impl/external-message.cpp`
- `validator/impl/ext-message-pool.cpp`
- `validator/impl/ext-message-pool.hpp`
- `validator/impl/collator.cpp`
- `validator/impl/collator-impl.h`
- `validator/impl/liteserver.cpp`
- `validator/full-node.cpp`
- `validator/full-node-shard.cpp`
- `tonlib/tonlib/TonlibClient.cpp`

Private chat references used for performance state:

- direct `jetton`, Russian jetton-token wording, and `jTPS` mentions in
  `jetton-research/chat_2years`
- especially clusters around April 5-9, May 5-25, and June 12-14, 2026
