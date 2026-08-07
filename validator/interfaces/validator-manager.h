/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include <ton/ton-tl.hpp>

#include "auto/tl/lite_api.h"
#include "block/signature-set.h"
#include "crypto/vm/db/DynamicBagOfCellsDb.h"
#include "impl/out-msg-queue-proof.hpp"
#include "td/actor/BackpressureQueue.h"
#include "td/utils/logging.h"
#include "validator/validator.h"

#include "block.h"
#include "external-message.h"
#include "liteserver.h"
#include "message-queue.h"
#include "proof.h"
#include "shard-block.h"
#include "shard.h"

DECLARE_LOG_CATEGORY(validator)

namespace ton {

namespace validator {

struct CandidateAccept {
  double ok_from_utime = 0.0;
};

struct CandidateReject {
  std::string reason;
  td::BufferSlice proof;
};

struct AsyncSerializerState {
  BlockIdExt last_block_id;
  BlockIdExt last_written_block_id;
  UnixTime last_written_block_ts;
};

struct StorageStatCacheStats {
  std::atomic<td::uint64> small_cnt = 0, small_cells = 0;
  std::atomic<td::uint64> hit_cnt = 0, hit_cells = 0;
  std::atomic<td::uint64> miss_cnt = 0, miss_cells = 0;

  StorageStatCacheStats() {
  }

  StorageStatCacheStats(const StorageStatCacheStats& other)
      : small_cnt(other.small_cnt.load())
      , small_cells(other.small_cells.load())
      , hit_cnt(other.hit_cnt.load())
      , hit_cells(other.hit_cells.load())
      , miss_cnt(other.miss_cnt.load())
      , miss_cells(other.miss_cells.load()) {
  }

  tl_object_ptr<ton_api::validatorStats_storageStatCacheStats> tl() const {
    return create_tl_object<ton_api::validatorStats_storageStatCacheStats>(small_cnt, small_cells, hit_cnt, hit_cells,
                                                                           miss_cnt, miss_cells);
  }
};

struct CollationStats {
  BlockIdExt block_id{workchainInvalid, 0, 0, RootHash::zero(), FileHash::zero()};
  td::Status status = td::Status::OK();

  td::Bits256 collated_data_hash = td::Bits256::zero();
  CatchainSeqno cc_seqno = 0;
  double collated_at = -1.0;
  td::uint32 actual_bytes = 0, actual_collated_data_bytes = 0;
  int attempt = 0;
  PublicKeyHash self = PublicKeyHash::zero();
  bool is_validator = false;
  td::uint32 estimated_bytes = 0, gas = 0, lt_delta = 0, estimated_collated_data_bytes = 0;
  int cat_bytes = 0, cat_gas = 0, cat_lt_delta = 0, cat_collated_data_bytes = 0;
  std::string limits_log;
  // Machine-readable attribution of why this block set its overload bit (feeds shard-split analysis):
  // 0 = none/normal, 1 = load (a block-limit axis hit soft), 2 = out_msg_queue force-split,
  // 3 = long_collation, 4 = dispatch_queue.
  int overload_reason = 0;
  bool want_split = false;  // the collator's final want_split decision for this block
  bool want_merge = false;  // the collator's final want_merge decision for this block
  td::uint64 overload_history = 0;
  td::uint64 underload_history = 0;
  int peak_block_limit_class = 0;  // running-max block_limit_class_ (cat_* only approximates this)
  double total_time = 0.0;
  std::string time_stats;

  td::uint32 transactions = 0;
  td::uint32 ordinary_external_transactions = 0;
  td::uint32 ordinary_internal_transactions = 0;
  td::uint32 new_msgs_generated = 0;
  td::uint32 new_msgs_immediate = 0;
  td::uint32 new_msgs_enqueued = 0;
  td::uint32 new_msgs_deferred = 0;
  td::uint32 new_msgs_external = 0;
  td::uint32 peak_new_msgs = 0;
  std::vector<BlockIdExt> shard_configuration;
  td::uint32 ext_msgs_total = 0;
  td::uint32 ext_msgs_filtered = 0;
  td::uint32 ext_msgs_ancestor_filtered = 0;
  td::uint32 ext_msgs_pool_filtered = 0;
  td::uint32 ext_msgs_accepted = 0;
  td::uint32 ext_msgs_rejected = 0;
  td::uint32 ext_waves = 0;
  td::uint32 ext_wave_tasks = 0;
  td::uint32 int_waves = 0;
  td::uint32 int_wave_tasks = 0;
  td::uint32 account_dict_estimator_updates = 0;
  td::uint32 account_dict_estimator_proofs = 0;
  td::uint32 account_dict_estimator_batches = 0;
  td::uint64 account_dict_estimator_estimated_bytes = 0;
  td::uint64 account_dict_estimator_proof_cells = 0;
  td::uint64 account_dict_estimator_proof_bits = 0;
  td::uint64 account_dict_estimator_proof_internal_refs = 0;
  td::uint64 account_dict_estimator_proof_external_refs = 0;
  td::uint32 account_dict_estimator_async_batches = 0;
  td::uint32 account_dict_estimator_async_queue_max = 0;
  td::uint32 account_dict_estimator_reused = 0;
  td::uint32 account_dict_estimator_corrections = 0;
  td::uint32 final_account_dict_async_updates = 0;
  td::uint32 final_account_dict_async_batches = 0;
  td::uint32 final_account_dict_async_queue_max = 0;
  td::uint32 account_lookup_batches = 0;
  td::uint32 account_lookup_prefetched = 0;
  td::uint64 old_out_msg_queue_size = 0;
  td::uint64 new_out_msg_queue_size = 0;
  td::uint32 msg_queue_cleaned = 0;
  struct NeighborStats {
    ShardIdFull shard;
    bool is_trivial = false;
    bool is_local = false;
    int msg_limit = -1;
    td::uint32 processed_msgs = 0;
    td::uint32 skipped_msgs = 0;
    bool limit_reached = false;

    tl_object_ptr<ton_api::validatorStats_blockStats_neighborStats> tl() const {
      return create_tl_object<ton_api::validatorStats_blockStats_neighborStats>(
          create_tl_shard_id(shard), is_trivial, is_local, msg_limit, processed_msgs, skipped_msgs, limit_reached);
    }
  };
  std::vector<NeighborStats> neighbors;
  BlockIdExt mc_block_id;

  double load_fraction_queue_cleanup = -1.0;
  double load_fraction_dispatch = -1.0;
  double load_fraction_internals = -1.0;
  double load_fraction_externals = -1.0;
  double load_fraction_new_msgs = -1.0;

  struct WorkTimeStats {
    td::RealCpuTimer::Time total;
    td::RealCpuTimer::Time preinit;
    td::RealCpuTimer::Time queue_cleanup;
    td::RealCpuTimer::Time dispatch;
    td::RealCpuTimer::Time ticktock;
    td::RealCpuTimer::Time inbound_internal;
    td::RealCpuTimer::Time inbound_external;
    td::RealCpuTimer::Time new_messages;
    td::RealCpuTimer::Time new_messages_route;
    td::RealCpuTimer::Time new_messages_prepare;
    td::RealCpuTimer::Time new_messages_execute;
    td::RealCpuTimer::Time new_messages_commit;
    td::RealCpuTimer::Time prelim_storage_stat;
    td::RealCpuTimer::Time trx_tvm;
    td::RealCpuTimer::Time trx_storage_stat;
    td::RealCpuTimer::Time trx_other;
    td::RealCpuTimer::Time trx_unpack;
    td::RealCpuTimer::Time trx_storage_credit;
    td::RealCpuTimer::Time trx_compute;
    td::RealCpuTimer::Time trx_action_bounce;
    td::RealCpuTimer::Time trx_serialize;
    td::RealCpuTimer::Time trx_external;
    td::RealCpuTimer::Time trx_internal;
    td::RealCpuTimer::Time msg_parse;
    td::RealCpuTimer::Time account_lookup;
    td::RealCpuTimer::Time account_dict_lookup;
    td::RealCpuTimer::Time account_unpack;
    td::RealCpuTimer::Time account_lookup_batch;
    td::RealCpuTimer::Time trx_limits;
    td::RealCpuTimer::Time trx_commit;
    td::RealCpuTimer::Time trx_postprocess;
    td::RealCpuTimer::Time message_descriptor;
    td::RealCpuTimer::Time account_dict_estimator_update;
    td::RealCpuTimer::Time account_dict_estimator_proof;
    td::RealCpuTimer::Time account_dict_estimator_wait;
    td::RealCpuTimer::Time account_block_build;
    td::RealCpuTimer::Time final_account_dict_update;
    td::RealCpuTimer::Time final_account_dict_async_work;
    td::RealCpuTimer::Time final_account_dict_async_wait;
    td::RealCpuTimer::Time final_account_dict_rebind;
    td::RealCpuTimer::Time account_storage_dict;
    td::RealCpuTimer::Time final_storage_stat;
    td::RealCpuTimer::Time enqueue_new_messages;
    td::RealCpuTimer::Time combine_account_transactions;
    td::RealCpuTimer::Time create_shard_state;
    td::RealCpuTimer::Time state_build_root;
    td::RealCpuTimer::Time state_merkle_update;
    td::RealCpuTimer::Time state_limit_proof;
    td::RealCpuTimer::Time create_block;
    td::RealCpuTimer::Time create_collated_data;
    td::RealCpuTimer::Time collated_prepare_proofs;
    td::RealCpuTimer::Time collated_state_proof;
    td::RealCpuTimer::Time collated_neighbor_proofs;
    td::RealCpuTimer::Time collated_storage_proofs;
    td::RealCpuTimer::Time create_block_candidate;
    td::RealCpuTimer::Time candidate_block_boc;
    td::RealCpuTimer::Time candidate_collated_boc;
    td::RealCpuTimer::Time candidate_hashes;
    td::RealCpuTimer::Time candidate_construct;

    std::string to_str(bool is_cpu) const {
      return PSTRING() << "total=" << total.get(is_cpu) << " preinit=" << preinit.get(is_cpu)
                       << " queue_cleanup=" << queue_cleanup.get(is_cpu) << " dispatch=" << dispatch.get(is_cpu)
                       << " ticktock=" << ticktock.get(is_cpu) << " inbound_internal=" << inbound_internal.get(is_cpu)
                       << " inbound_external=" << inbound_external.get(is_cpu)
                       << " new_messages=" << new_messages.get(is_cpu)
                       << " new_messages_route=" << new_messages_route.get(is_cpu)
                       << " new_messages_prepare=" << new_messages_prepare.get(is_cpu)
                       << " new_messages_execute=" << new_messages_execute.get(is_cpu)
                       << " new_messages_commit=" << new_messages_commit.get(is_cpu)
                       << " prelim_storage_stat=" << prelim_storage_stat.get(is_cpu)
                       << " trx_tvm=" << trx_tvm.get(is_cpu) << " trx_storage_stat=" << trx_storage_stat.get(is_cpu)
                       << " trx_other=" << trx_other.get(is_cpu) << " trx_external=" << trx_external.get(is_cpu)
                       << " trx_internal=" << trx_internal.get(is_cpu) << " trx_unpack=" << trx_unpack.get(is_cpu)
                       << " trx_storage_credit=" << trx_storage_credit.get(is_cpu)
                       << " trx_compute=" << trx_compute.get(is_cpu)
                       << " trx_action_bounce=" << trx_action_bounce.get(is_cpu)
                       << " trx_serialize=" << trx_serialize.get(is_cpu) << " msg_parse=" << msg_parse.get(is_cpu)
                       << " account_lookup=" << account_lookup.get(is_cpu) << " trx_limits=" << trx_limits.get(is_cpu)
                       << " account_dict_lookup=" << account_dict_lookup.get(is_cpu)
                       << " account_unpack=" << account_unpack.get(is_cpu)
                       << " account_lookup_batch=" << account_lookup_batch.get(is_cpu)
                       << " trx_commit=" << trx_commit.get(is_cpu) << " trx_postprocess=" << trx_postprocess.get(is_cpu)
                       << " message_descriptor=" << message_descriptor.get(is_cpu)
                       << " account_dict_estimator_update=" << account_dict_estimator_update.get(is_cpu)
                       << " account_dict_estimator_proof=" << account_dict_estimator_proof.get(is_cpu)
                       << " account_dict_estimator_wait=" << account_dict_estimator_wait.get(is_cpu)
                       << " account_block_build=" << account_block_build.get(is_cpu)
                       << " final_account_dict_update=" << final_account_dict_update.get(is_cpu)
                       << " final_account_dict_async_work=" << final_account_dict_async_work.get(is_cpu)
                       << " final_account_dict_async_wait=" << final_account_dict_async_wait.get(is_cpu)
                       << " final_account_dict_rebind=" << final_account_dict_rebind.get(is_cpu)
                       << " account_storage_dict=" << account_storage_dict.get(is_cpu)
                       << " final_storage_stat=" << final_storage_stat.get(is_cpu)
                       << " enqueue_new_messages=" << enqueue_new_messages.get(is_cpu)
                       << " combine_account_transactions=" << combine_account_transactions.get(is_cpu)
                       << " create_shard_state=" << create_shard_state.get(is_cpu)
                       << " state_build_root=" << state_build_root.get(is_cpu)
                       << " state_merkle_update=" << state_merkle_update.get(is_cpu)
                       << " state_limit_proof=" << state_limit_proof.get(is_cpu)
                       << " create_block=" << create_block.get(is_cpu)
                       << " create_collated_data=" << create_collated_data.get(is_cpu)
                       << " collated_prepare_proofs=" << collated_prepare_proofs.get(is_cpu)
                       << " collated_state_proof=" << collated_state_proof.get(is_cpu)
                       << " collated_neighbor_proofs=" << collated_neighbor_proofs.get(is_cpu)
                       << " collated_storage_proofs=" << collated_storage_proofs.get(is_cpu)
                       << " create_block_candidate=" << create_block_candidate.get(is_cpu)
                       << " candidate_block_boc=" << candidate_block_boc.get(is_cpu)
                       << " candidate_collated_boc=" << candidate_collated_boc.get(is_cpu)
                       << " candidate_hashes=" << candidate_hashes.get(is_cpu)
                       << " candidate_construct=" << candidate_construct.get(is_cpu);
    }
  };
  WorkTimeStats work_time;
  double wait_externals_time = 0.0;
  double check_load_do_collate_time = -1.0;
  double check_load_total_time = -1.0;
  StorageStatCacheStats storage_stat_cache;

  tl_object_ptr<ton_api::validatorStats_collatedBlock> tl() const {
    std::vector<tl_object_ptr<ton_api::tonNode_blockIdExt>> shards_obj;
    for (const BlockIdExt& block_id : shard_configuration) {
      shards_obj.push_back(create_tl_block_id(block_id));
    }
    std::vector<tl_object_ptr<ton_api::validatorStats_blockStats_neighborStats>> neighbors_obj;
    for (const NeighborStats& neighbor : neighbors) {
      neighbors_obj.push_back(neighbor.tl());
    }
    auto block_stats = create_tl_object<ton_api::validatorStats_blockStats>(
        create_tl_object<ton_api::validatorStats_blockStats_extMsgsStats>(ext_msgs_total, ext_msgs_filtered,
                                                                          ext_msgs_accepted, ext_msgs_rejected),
        transactions, std::move(shards_obj), old_out_msg_queue_size, new_out_msg_queue_size, msg_queue_cleaned,
        std::move(neighbors_obj), create_tl_block_id(mc_block_id));
    return create_tl_object<ton_api::validatorStats_collatedBlock>(
        create_tl_block_id(block_id), collated_data_hash, cc_seqno, collated_at, actual_bytes,
        actual_collated_data_bytes, attempt, self.bits256_value(), is_validator, total_time, work_time.total.real,
        work_time.total.cpu, time_stats, work_time.to_str(false), work_time.to_str(true), wait_externals_time,
        check_load_do_collate_time, check_load_total_time,
        create_tl_object<ton_api::validatorStats_blockLimitsStatus>(
            estimated_bytes, gas, lt_delta, estimated_collated_data_bytes, cat_bytes, cat_gas, cat_lt_delta,
            cat_collated_data_bytes, load_fraction_queue_cleanup, load_fraction_dispatch, load_fraction_internals,
            load_fraction_externals, load_fraction_new_msgs, limits_log, overload_reason, want_split,
            peak_block_limit_class),
        std::move(block_stats), storage_stat_cache.tl());
  }
};

struct ValidationStats {
  BlockIdExt block_id;
  td::Bits256 collated_data_hash = td::Bits256::zero();
  double validated_at = -1.0;
  PublicKeyHash self = PublicKeyHash::zero();
  bool valid = false;
  std::string comment;
  td::uint32 actual_bytes = 0, actual_collated_data_bytes = 0;
  double total_time = 0.0;
  std::string time_stats;
  double actual_time = 0.0;
  bool parallel_accounts_validation = false;

  struct WorkTimeStats {
    td::RealCpuTimer::Time total;
    td::RealCpuTimer::Time unpack_block_candidate;
    td::RealCpuTimer::Time process_mc_state;
    td::RealCpuTimer::Time trx_tvm;
    td::RealCpuTimer::Time trx_storage_stat;
    td::RealCpuTimer::Time trx_other;
    td::RealCpuTimer::Time check_transactions_other;
    td::RealCpuTimer::Time unpack_state;
    td::RealCpuTimer::Time validate_block_tlb;
    td::RealCpuTimer::Time unpack_block_data;
    td::RealCpuTimer::Time precheck_account_updates;
    td::RealCpuTimer::Time precheck_account_transactions;
    td::RealCpuTimer::Time precheck_msg_queue;
    td::RealCpuTimer::Time unpack_dispatch_queue;
    td::RealCpuTimer::Time check_in_msg_descr;
    td::RealCpuTimer::Time check_out_msg_descr;
    td::RealCpuTimer::Time check_dispatch_queue;
    td::RealCpuTimer::Time check_processed_upto;
    td::RealCpuTimer::Time check_in_queue;
    td::RealCpuTimer::Time check_new_state;

    std::string to_str(bool is_cpu) const {
      return PSTRING() << "total=" << total.get(is_cpu)
                       << " unpack_block_candidate=" << unpack_block_candidate.get(is_cpu)
                       << " process_mc_state=" << process_mc_state.get(is_cpu) << " trx_tvm=" << trx_tvm.get(is_cpu)
                       << " trx_storage_stat=" << trx_storage_stat.get(is_cpu) << " trx_other=" << trx_other.get(is_cpu)
                       << " check_transactions_other=" << check_transactions_other.get(is_cpu)
                       << " unpack_state=" << unpack_state.get(is_cpu)
                       << " validate_block_tlb=" << validate_block_tlb.get(is_cpu)
                       << " unpack_block_data=" << unpack_block_data.get(is_cpu)
                       << " precheck_account_updates=" << precheck_account_updates.get(is_cpu)
                       << " precheck_account_transactions=" << precheck_account_transactions.get(is_cpu)
                       << " precheck_msg_queue=" << precheck_msg_queue.get(is_cpu)
                       << " unpack_dispatch_queue=" << unpack_dispatch_queue.get(is_cpu)
                       << " check_in_msg_descr=" << check_in_msg_descr.get(is_cpu)
                       << " check_out_msg_descr=" << check_out_msg_descr.get(is_cpu)
                       << " check_dispatch_queue=" << check_dispatch_queue.get(is_cpu)
                       << " check_processed_upto=" << check_processed_upto.get(is_cpu)
                       << " check_in_queue=" << check_in_queue.get(is_cpu)
                       << " check_new_state=" << check_new_state.get(is_cpu);
    }
  };
  WorkTimeStats work_time;
  mutable StorageStatCacheStats storage_stat_cache;

  tl_object_ptr<ton_api::validatorStats_validatedBlock> tl() const {
    return create_tl_object<ton_api::validatorStats_validatedBlock>(
        create_tl_block_id(block_id), collated_data_hash, validated_at, self.bits256_value(), valid, comment,
        actual_bytes, actual_collated_data_bytes, total_time, actual_time, work_time.total.real, work_time.total.cpu,
        time_stats, work_time.to_str(false), work_time.to_str(true), storage_stat_cache.tl(),
        parallel_accounts_validation);
  }
};

using ExtMsgQueueEntry = std::pair<td::Ref<ExtMessage>, int>;
using ExtMsgQueueBatch = std::vector<ExtMsgQueueEntry>;
using ExtMsgQueue = td::actor::BackpressureQueue<ExtMsgQueueBatch>;

struct ExtMsgCallback {
  ShardIdFull shard;
  ExtMsgQueue queue;
  td::CancellationToken cancellation_token;
  td::Timestamp timeout;
  std::shared_ptr<const std::vector<ExtMessage::Hash>> excluded_normalized_hashes;
  std::shared_ptr<std::atomic<td::uint32>> excluded_count;
  ExtMsgQueueBatch pending_batch;
  td::Timestamp batch_flush_at;
  bool sync_only = false;
};

using ValidateCandidateResult = td::Variant<CandidateAccept, CandidateReject>;

class ValidatorManager : public ValidatorManagerInterface {
 public:
  virtual void init_last_masterchain_state(td::Ref<MasterchainState> state) {
  }
  virtual void set_block_state(BlockHandle handle, td::Ref<ShardState> state, vm::StoreCellHint hint,
                               td::Promise<td::Ref<ShardState>> promise) = 0;
  virtual void store_block_state_part(BlockId effective_block, td::Ref<vm::Cell> cell,
                                      td::Promise<td::Ref<vm::DataCell>> promise) = 0;
  virtual void set_block_state_from_data(BlockHandle handle, td::Ref<BlockData> block,
                                         td::Promise<td::Ref<ShardState>> promise) = 0;
  virtual void set_block_state_from_data_bulk(std::vector<td::Ref<BlockData>> blocks,
                                              td::Promise<td::Unit> promise) = 0;
  virtual void get_cell_db_reader(td::Promise<std::shared_ptr<vm::CellDbReader>> promise) = 0;
  virtual void get_cell_from_cell_db(td::Bits256 hash, td::Promise<td::Ref<vm::DataCell>> promise) = 0;
  virtual void store_persistent_state_file(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                           PersistentStateType type, td::BufferSlice state,
                                           td::Promise<td::Unit> promise) = 0;
  virtual void store_persistent_state_file_gen(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                               PersistentStateType type,
                                               std::function<td::Status(td::FileFd&)> write_data,
                                               td::Promise<td::Unit> promise) = 0;
  virtual void store_zero_state_file(BlockIdExt block_id, td::BufferSlice state, td::Promise<td::Unit> promise) = 0;

  virtual void set_block_data(BlockHandle handle, td::Ref<BlockData> data, td::Promise<td::Unit> promise) = 0;
  virtual void wait_block_data(BlockHandle handle, td::uint32 priority, td::Timestamp,
                               td::Promise<td::Ref<BlockData>> promise) = 0;
  virtual void wait_block_data_short(BlockIdExt block_id, td::uint32 priority, td::Timestamp,
                                     td::Promise<td::Ref<BlockData>> promise) = 0;

  virtual void set_block_proof(BlockHandle handle, td::Ref<Proof> proof, td::Promise<td::Unit> promise) = 0;
  virtual void wait_block_proof(BlockHandle handle, td::Timestamp timeout, td::Promise<td::Ref<Proof>> promise) = 0;
  virtual void wait_block_proof_short(BlockIdExt id, td::Timestamp timeout, td::Promise<td::Ref<Proof>> promise) = 0;

  virtual void set_block_proof_link(BlockHandle handle, td::Ref<ProofLink> proof, td::Promise<td::Unit> promise) = 0;
  virtual void wait_block_proof_link(BlockHandle handle, td::Timestamp timeout,
                                     td::Promise<td::Ref<ProofLink>> promise) = 0;
  virtual void wait_block_proof_link_short(BlockIdExt id, td::Timestamp timeout,
                                           td::Promise<td::Ref<ProofLink>> promise) = 0;

  virtual void set_block_signatures(BlockHandle handle, td::Ref<block::BlockSignatureSet> signatures,
                                    Ref<block::ValidatorSet> vset, td::Promise<td::Unit> promise) = 0;
  virtual void wait_block_signatures(BlockHandle handle, td::Timestamp timeout,
                                     td::Promise<td::Ref<block::BlockSignatureSet>> promise) = 0;
  virtual void wait_block_signatures_short(BlockIdExt id, td::Timestamp timeout,
                                           td::Promise<td::Ref<block::BlockSignatureSet>> promise) = 0;

  virtual void cache_block_candidate(BlockCandidate candidate, td::Promise<td::Unit> promise) {
    promise.set_value(td::Unit{});
  }
  virtual void send_block_candidate_broadcast(BlockIdExt id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                              td::BufferSlice data, int mode) = 0;

  virtual void wait_prev_block_state(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                     td::Promise<td::Ref<ShardState>> promise) = 0;

  virtual void wait_block_message_queue(BlockHandle handle, td::uint32 priority, td::Timestamp timeout,
                                        td::Promise<td::Ref<MessageQueue>> promise) = 0;
  virtual void wait_block_message_queue_short(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                              td::Promise<td::Ref<MessageQueue>> promise) = 0;
  virtual void get_external_messages(ShardIdFull shard, std::unique_ptr<ExtMsgCallback> callback) = 0;
  virtual void get_shard_blocks_for_collator(BlockIdExt masterchain_block_id,
                                             td::Promise<std::vector<td::Ref<ShardTopBlockDescription>>> promise) = 0;
  virtual void complete_external_messages(std::vector<ExtMessage::Hash> to_delay,
                                          std::vector<ExtMessage::Hash> to_delete) = 0;
  virtual void cleanup_applied_external_messages(BlockHandle handle, td::Ref<BlockData> block) = 0;

  //virtual void set_first_block(ZeroStateIdExt state, BlockIdExt block, td::Promise<td::Unit> promise) = 0;
  virtual void set_next_block(BlockIdExt prev, BlockIdExt next, td::Promise<td::Unit> promise) = 0;

  virtual void new_block(BlockHandle handle, td::Ref<ShardState> state, td::Promise<td::Unit> promise) = 0;
  virtual void on_block_accepted(BlockIdExt block_id) {
  }

  virtual td::actor::Task<ReceivedBlock> send_get_block_request(BlockIdExt id, td::uint32 priority) {
    co_return td::Status::Error("not implemented");
  }
  virtual void send_get_zero_state_request(BlockIdExt id, td::uint32 priority,
                                           td::Promise<td::BufferSlice> promise) = 0;
  virtual void send_get_persistent_state_request(BlockIdExt id, BlockIdExt masterchain_block_id,
                                                 PersistentStateType type, td::uint32 priority,
                                                 td::Promise<td::BufferSlice> promise) = 0;
  virtual void send_get_block_proof_request(BlockIdExt block_id, td::uint32 priority,
                                            td::Promise<td::BufferSlice> promise) = 0;
  virtual void send_get_block_proof_link_request(BlockIdExt block_id, td::uint32 priority,
                                                 td::Promise<td::BufferSlice> promise) = 0;
  virtual void send_get_next_key_blocks_request(BlockIdExt block_id, td::uint32 priority,
                                                td::Promise<std::vector<BlockIdExt>> promise) = 0;
  virtual void send_top_shard_block_description(td::Ref<ShardTopBlockDescription> desc) = 0;
  virtual void send_block_broadcast(BlockBroadcast broadcast, int mode) = 0;
  virtual void send_block_finality_broadcast(BlockFinalityBroadcast finality, int mode) = 0;
  virtual void send_get_out_msg_queue_proof_request(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks,
                                                    block::ImportedMsgQueueLimits limits,
                                                    td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise) = 0;
  virtual void send_download_archive_request(BlockSeqno mc_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                                             td::Timestamp timeout, td::Promise<std::string> promise) = 0;

  virtual void get_block_proof_link_from_import(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                                td::Promise<td::BufferSlice> promise) {
    promise.set_error(td::Status::Error("not supported"));
  }

  virtual void update_shard_client_state(BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise) = 0;
  virtual void get_shard_client_state(bool from_db, td::Promise<BlockIdExt> promise) = 0;

  virtual void update_async_serializer_state(AsyncSerializerState state, td::Promise<td::Unit> promise) = 0;
  virtual void get_async_serializer_state(td::Promise<AsyncSerializerState> promise) = 0;

  virtual void try_get_static_file(FileHash file_hash, td::Promise<td::BufferSlice> promise) = 0;

  virtual void allow_block_state_gc(BlockIdExt block_id, td::Promise<bool> promise) = 0;

  virtual void archive(BlockHandle handle, td::Promise<td::Unit> promise) = 0;

  virtual void check_is_hardfork(BlockIdExt block_id, td::Promise<bool> promise) = 0;
  virtual void get_vertical_seqno(BlockSeqno seqno, td::Promise<td::uint32> promise) = 0;

  virtual void update_last_known_key_block(BlockHandle handle, bool send_request) = 0;
  virtual void update_gc_block_handle(BlockHandle handle, td::Promise<td::Unit> promise) = 0;

  virtual void update_shard_client_block_handle(BlockHandle handle, td::Ref<MasterchainState> state,
                                                td::Promise<td::Unit> promise) = 0;

  virtual void truncate(BlockSeqno seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise) = 0;

  virtual void wait_shard_client_state(BlockSeqno seqno, td::Timestamp timeout, td::Promise<td::Unit> promise) = 0;

  virtual void get_block_handle_for_litequery(BlockIdExt block_id, td::Promise<ConstBlockHandle> promise) = 0;
  virtual void get_block_data_for_litequery(BlockIdExt block_id, td::Promise<td::Ref<BlockData>> promise) = 0;
  virtual void get_block_state_for_litequery(BlockIdExt block_id, td::Promise<td::Ref<ShardState>> promise) = 0;
  virtual void get_block_by_lt_for_litequery(AccountIdPrefixFull account, LogicalTime lt,
                                             td::Promise<ConstBlockHandle> promise) = 0;
  virtual void get_block_by_unix_time_for_litequery(AccountIdPrefixFull account, UnixTime ts,
                                                    td::Promise<ConstBlockHandle> promise) = 0;
  virtual void get_block_by_seqno_for_litequery(AccountIdPrefixFull account, BlockSeqno seqno,
                                                td::Promise<ConstBlockHandle> promise) = 0;
  virtual void get_pending_shard_blocks_for_litequery(
      td::optional<ShardIdFull> shard,
      td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_pendingShardBlocks>> promise) {
    promise.set_error(td::Status::Error("not implemented"));
  }

  virtual void add_lite_query_stats(int lite_query_id, bool success) {
  }

  virtual void log_collate_query_stats(CollationStats stats) {
  }
  virtual void log_validate_query_stats(ValidationStats stats) {
  }

  virtual void add_persistent_state_description(td::Ref<PersistentStateDescription> desc) = 0;

  virtual void get_storage_stat_cache(td::Promise<std::function<td::Ref<vm::Cell>(const td::Bits256&)>> promise) {
    promise.set_error(td::Status::Error("not implemented"));
  }
  virtual void update_storage_stat_cache(std::vector<std::pair<td::Ref<vm::Cell>, td::uint32>> data) {
    // not implemented
  }

  virtual void wait_verify_shard_blocks(std::vector<BlockIdExt> blocks, td::Promise<td::Unit> promise) {
    promise.set_result(td::Unit());
  }

  virtual void iterate_temp_block_handles(std::function<void(const BlockHandleInterface&)> f) {
  }

  static bool is_persistent_state(UnixTime ts, UnixTime prev_ts) {
    return ts / (1 << 17) != prev_ts / (1 << 17);
  }
  static UnixTime persistent_state_ttl(UnixTime ts) {
    auto x = ts / (1 << 17);
    CHECK(x > 0);
    auto b = td::count_trailing_zeroes32(x);
    return ts + ((1 << 18) << b);
  }
};

}  // namespace validator

}  // namespace ton
