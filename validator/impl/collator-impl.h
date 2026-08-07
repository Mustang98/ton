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
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <thread>

#include "block/block-db.h"
#include "block/block.h"
#include "block/mc-config.h"
#include "block/output-queue-merger.h"
#include "block/transaction.h"
#include "common/global-version.h"
#include "common/refcnt.hpp"
#include "interfaces/validator-manager.h"
#include "td/utils/port/thread.h"
#include "vm/cells.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cells/MerkleUpdate.h"
#include "vm/dict.h"

#include "block-parse.h"
#include "fabric.h"
#include "shard.hpp"
#include "top-shard-descr.hpp"

namespace ton {

namespace validator {
using td::Ref;

class AsyncAccountDictEstimator;
class AsyncAccountLookupBatch;

// A persistent fork-join pool for deterministic transaction-execution waves. The actor
// thread participates in each wave; workers only execute tasks and never commit collator state.
class WaveExecutor {
 public:
  explicit WaveExecutor(int threads) {
    for (int i = 1; i < threads; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  ~WaveExecutor() {
    {
      std::lock_guard<std::mutex> guard{mutex_};
      stop_ = true;
    }
    start_cv_.notify_all();
    for (auto& worker : workers_) {
      worker.join();
    }
  }

  void run(std::vector<std::function<void()>>& tasks) {
    if (tasks.empty()) {
      return;
    }
    if (tasks.size() == 1) {
      tasks.front()();
      return;
    }
    {
      std::lock_guard<std::mutex> guard{mutex_};
      tasks_ = &tasks;
      next_.store(0, std::memory_order_relaxed);
      done_.store(0, std::memory_order_relaxed);
      ++generation_;
    }
    size_t helpers = std::min(workers_.size(), tasks.size() - 1);
    for (size_t i = 0; i < helpers; ++i) {
      start_cv_.notify_one();
    }
    drain(tasks);
    std::unique_lock<std::mutex> lock{mutex_};
    done_cv_.wait(lock, [&] { return done_.load(std::memory_order_acquire) == tasks.size() && active_ == 0; });
    tasks_ = nullptr;
  }

 private:
  void drain(std::vector<std::function<void()>>& tasks) {
    while (true) {
      size_t index = next_.fetch_add(1, std::memory_order_relaxed);
      if (index >= tasks.size()) {
        return;
      }
      tasks[index]();
      if (done_.fetch_add(1, std::memory_order_acq_rel) + 1 == tasks.size()) {
        std::lock_guard<std::mutex> guard{mutex_};
        done_cv_.notify_all();
      }
    }
  }

  void worker_loop() {
    td::uint64 seen_generation = 0;
    while (true) {
      std::vector<std::function<void()>>* tasks;
      {
        std::unique_lock<std::mutex> lock{mutex_};
        start_cv_.wait(lock, [&] { return stop_ || (tasks_ != nullptr && generation_ != seen_generation); });
        if (stop_) {
          return;
        }
        seen_generation = generation_;
        tasks = tasks_;
        ++active_;
      }
      drain(*tasks);
      {
        std::lock_guard<std::mutex> guard{mutex_};
        --active_;
      }
      done_cv_.notify_all();
    }
  }

  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable start_cv_, done_cv_;
  std::vector<std::function<void()>>* tasks_ = nullptr;
  std::atomic<size_t> next_{0};
  std::atomic<size_t> done_{0};
  size_t active_ = 0;
  td::uint64 generation_ = 0;
  bool stop_ = false;
};

class Collator final : public td::actor::Actor {
 public:
  static constexpr int supported_version() {
    return SUPPORTED_VERSION;
  }
  static constexpr long long supported_capabilities() {
    return ton::capCreateStatsEnabled | ton::capBounceMsgBody | ton::capReportVersion | ton::capShortDequeue |
           ton::capStoreOutMsgQueueSize | ton::capMsgMetadata | ton::capDeferMessages | ton::capFullCollatedData;
  }

 private:
  using LtCellRef = block::LtCellRef;
  using NewOutMsg = block::NewOutMsg;
  CollateParams params_;
  const ShardIdFull shard_;
  ton::BlockId new_id{workchainInvalid, 0, 0};
  bool busy_{false};
  bool before_split_{false};
  bool after_split_{false};
  bool after_merge_{false};
  bool want_split_{false};
  bool want_merge_{false};
  bool right_child_{false};
  bool preinit_complete{false};
  bool is_key_block_{false};
  bool block_full_{false};
  bool inbound_queues_empty_{false};
  bool libraries_changed_{false};
  bool prev_key_block_exists_{false};
  const std::vector<BlockIdExt>& prev_blocks;
  std::vector<Ref<ShardState>> prev_states;
  size_t pending_prev_states_{0};
  std::vector<Ref<BlockData>> prev_block_data;
  td::actor::ActorId<ValidatorManager> manager;
  td::Timestamp timeout_;
  td::Timestamp queue_cleanup_timeout_, external_msg_timeout_, internal_msg_timeout_;
  td::Promise<BlockCandidate> main_promise;
  bool allow_repeat_collation_ = false;
  ton::BlockSeqno last_block_seqno{0};
  ton::BlockSeqno prev_mc_block_seqno{0};
  ton::BlockSeqno new_block_seqno{0};
  ton::BlockSeqno prev_key_block_seqno_{0};
  int step{0};
  int pending{0};
  td::Timestamp collator_started_at_ = td::Timestamp::now();
  td::Timestamp do_collate_started_at_;
  double wait_externals_total_time_ = 0.0;
  static constexpr int max_ihr_msg_size = 65535;   // 64k
  static constexpr int max_ext_msg_size = 65535;   // 64k
  static constexpr int max_blk_sign_size = 65535;  // 64k
  static constexpr bool shard_splitting_enabled = true;

 public:
  Collator(CollateParams params, td::actor::ActorId<ValidatorManager> manager, td::CancellationToken cancellation_token,
           td::Promise<BlockCandidate> promise);
  ~Collator() override = default;
  bool is_busy() const {
    return busy_;
  }
  ShardId get_shard() const {
    return shard_.shard;
  }
  WorkchainId workchain() const {
    return shard_.workchain;
  }
  static constexpr td::uint32 priority() {
    return 2;
  }

  static td::Result<std::unique_ptr<block::transaction::Transaction>> impl_create_ordinary_transaction(
      Ref<vm::Cell> msg_root, block::Account* acc, UnixTime utime, LogicalTime lt,
      block::StoragePhaseConfig* storage_phase_cfg, block::ComputePhaseConfig* compute_phase_cfg,
      block::ActionPhaseConfig* action_phase_cfg, block::SerializeConfig* serialize_cfg, bool external,
      LogicalTime after_lt, CollationStats* stats = nullptr);

 private:
  void start_up() override;
  void load_prev_states_blocks();
  void alarm() override;

  void tear_down() override {
    ext_msg_cancellation_.cancel();
    ext_msg_queue_.close();
  }

  int verbosity{3 * 0};
  bool full_collated_data_ = false;
  ton::LogicalTime start_lt, max_lt;
  ton::UnixTime now_;
  ton::UnixTime prev_now_;
  ton::UnixTime now_upper_limit_{~0U};
  td::uint64 now_ms_;
  unsigned out_msg_queue_ops_{}, in_descr_cnt_{}, out_descr_cnt_{};
  Ref<MasterchainStateQ> mc_state_;
  Ref<BlockData> prev_mc_block;
  BlockIdExt mc_block_id_;
  Ref<vm::Cell> mc_state_root;
  Ref<vm::Cell> mc_block_root;
  td::BitArray<256> rand_seed_ = td::Bits256::zero();
  std::unique_ptr<block::ConfigInfo> config_;
  std::unique_ptr<block::ShardConfig> shard_conf_;
  std::map<BlockSeqno, Ref<MasterchainStateQ>> aux_mc_states_;
  std::map<ShardIdFull, td::int32> neighbor_msg_queues_limits_;
  std::vector<block::McShardDescr> neighbors_;
  std::unique_ptr<block::OutputQueueMerger> nb_out_msgs_;
  std::vector<ton::StdSmcAddress> special_smcs;
  Ref<vm::Cell> prev_block_root;
  Ref<vm::Cell> prev_state_root_, prev_state_root_pure_;
  Ref<vm::Cell> state_root;                              // (new) shardchain state
  Ref<vm::Cell> state_update;                            // Merkle update from prev_state_root to state_root
  std::shared_ptr<vm::CellUsageTree> state_usage_tree_;  // used to construct Merkle update
  Ref<vm::Cell> precomputed_state_proof_;
  td::Result<Ref<vm::Cell>> async_state_proof_result_;
  td::RealCpuTimer::Time async_state_proof_time_;
  td::thread async_state_proof_thread_;
  bool async_state_proof_started_{false};
  bool collated_proofs_prepared_{false};
  Ref<vm::CellSlice> new_config_params_;
  Ref<vm::Cell> old_mparams_;
  ton::LogicalTime prev_state_lt_;
  ton::LogicalTime shards_max_end_lt_{0};
  ton::UnixTime prev_state_utime_;
  int global_id_{0};
  int global_version_{0};
  ton::BlockSeqno min_ref_mc_seqno_{~0U};
  ton::BlockSeqno vert_seqno_{~0U}, prev_vert_seqno_{~0U};
  ton::BlockIdExt prev_key_block_;
  ton::LogicalTime prev_key_block_lt_;
  bool accept_msgs_{true};
  bool shard_conf_adjusted_{false};
  bool ihr_enabled_{false};
  bool create_stats_enabled_{false};
  bool report_version_{false};
  bool skip_topmsgdescr_{false};
  bool skip_extmsg_{false};
  bool short_dequeue_records_{false};
  bool allow_same_timestamp_{false};
  td::uint64 overload_history_{0}, underload_history_{0};
  td::uint64 block_size_estimate_{};
  Ref<block::WorkchainInfo> wc_info_;
  std::vector<Ref<ShardTopBlockDescription>> shard_block_descr_;
  std::vector<Ref<ShardTopBlockDescrQ>> used_shard_block_descr_;
  std::unique_ptr<vm::Dictionary> shard_libraries_;
  Ref<vm::Cell> mc_state_extra_;
  std::unique_ptr<vm::AugmentedDictionary> account_dict, old_account_dict;
  std::map<ton::StdSmcAddress, std::unique_ptr<block::Account>> accounts;
  std::vector<block::StoragePrices> storage_prices_;
  block::StoragePhaseConfig storage_phase_cfg_{&storage_prices_};
  block::ComputePhaseConfig compute_phase_cfg_;
  block::ActionPhaseConfig action_phase_cfg_;
  block::SerializeConfig serialize_cfg_;
  td::RefInt256 masterchain_create_fee_, basechain_create_fee_;
  std::unique_ptr<block::BlockLimits> block_limits_;
  std::unique_ptr<block::BlockLimitStatus> block_limit_status_;
  vm::ProofStorageStat collated_data_stat;
  int block_limit_class_ = 0;
  ton::LogicalTime min_new_msg_lt{std::numeric_limits<td::uint64>::max()};
  block::CurrencyCollection total_balance_, old_total_balance_, total_validator_fees_;
  block::CurrencyCollection global_balance_, old_global_balance_, import_created_{0};
  Ref<vm::Cell> recover_create_msg_, mint_msg_;
  Ref<vm::Cell> new_block;
  block::ValueFlow value_flow_{block::ValueFlow::SetZero()};
  std::unique_ptr<vm::AugmentedDictionary> fees_import_dict_;

  std::set<td::Bits256> registered_ext_msgs_;
  std::vector<ExtMessage::Hash> ancestor_ext_msg_hashes_;
  std::shared_ptr<std::atomic<td::uint32>> pool_filtered_ext_msgs_;
  ExtMsgQueue ext_msg_queue_;
  std::deque<ExtMsgQueueEntry> pending_ext_msgs_;
  td::CancellationTokenSource ext_msg_cancellation_;

  std::priority_queue<NewOutMsg, std::vector<NewOutMsg>, std::greater<NewOutMsg>> new_msgs;
  std::vector<StdSmcAddress> pending_new_msg_accounts_;
  std::shared_ptr<AsyncAccountLookupBatch> async_account_lookup_batch_;
  std::pair<ton::LogicalTime, ton::Bits256> last_proc_int_msg_, first_unproc_int_msg_;
  block::tlb::Aug_InMsgDescr aug_InMsgDescr{0};
  block::tlb::Aug_OutMsgDescr aug_OutMsgDescr{0};
  std::unique_ptr<vm::AugmentedDictionary> in_msg_dict, out_msg_dict, old_out_msg_queue_, out_msg_queue_,
      sibling_out_msg_queue_;
  struct MsgDescrUpdate {
    td::Bits256 key;
    Ref<vm::Cell> value;
  };
  std::vector<MsgDescrUpdate> pending_in_msg_descr_updates_;
  std::vector<MsgDescrUpdate> pending_out_msg_descr_updates_;
  std::map<StdSmcAddress, size_t> unprocessed_deferred_messages_;  // number of messages from dispatch queue in new_msgs
  td::uint64 out_msg_queue_size_ = 0;
  td::uint64 old_out_msg_queue_size_ = 0;
  bool have_out_msg_queue_size_in_state_ = false;
  std::unique_ptr<vm::Dictionary> ihr_pending;
  std::shared_ptr<block::MsgProcessedUptoCollection> processed_upto_, sibling_processed_upto_;
  std::unique_ptr<vm::Dictionary> block_create_stats_;
  std::map<td::Bits256, int> block_create_count_;
  unsigned block_create_total_{0};
  std::vector<ExtMessage::Hash> bad_ext_msgs_, delay_ext_msgs_;
  Ref<vm::Cell> shard_account_blocks_;  // ShardAccountBlocks

  std::map<td::Bits256, Ref<vm::Cell>> block_state_proofs_;
  std::vector<std::pair<BlockIdExt, vm::MerkleProofBuilder>> neighbor_proof_builders_;
  std::vector<Ref<vm::Cell>> collated_roots_;

  struct AccountStorageDict {
    bool inited = false;
    vm::MerkleProofBuilder mpb;
    vm::ProofStorageStat proof_stat;
    bool add_to_collated_data = false;
    std::vector<Ref<vm::Cell>> storage_stat_updates;
  };
  std::map<td::Bits256, AccountStorageDict> account_storage_dicts_;

  struct WaveProofStats {
    vm::ProofStorageStat collated;
    vm::ProofStorageStat storage;
    AccountStorageDict* storage_dict{nullptr};
  };

  std::unique_ptr<ton::BlockCandidate> block_candidate;

  std::unique_ptr<vm::AugmentedDictionary> dispatch_queue_, old_dispatch_queue_;
  std::map<StdSmcAddress, td::uint32> sender_generated_messages_count_;
  unsigned dispatch_queue_ops_{0};
  std::map<StdSmcAddress, LogicalTime> last_dispatch_queue_emitted_lt_;
  bool have_unprocessed_account_dispatch_queue_ = false;
  bool dispatch_queue_total_limit_reached_ = false;
  td::uint64 defer_out_queue_size_limit_;
  td::uint64 hard_defer_out_queue_size_limit_;

  std::unique_ptr<vm::AugmentedDictionary> account_dict_estimator_;
  std::unique_ptr<vm::AugmentedDictionary> analytical_account_dict_estimator_;
  std::vector<td::Bits256> analytical_estimator_previous_keys_;
  std::vector<td::Bits256> analytical_estimator_pending_keys_;
  bool analytical_account_dict_estimator_valid_{true};
  std::shared_ptr<AsyncAccountDictEstimator> async_account_dict_estimator_;
  std::shared_ptr<AsyncAccountDictEstimator> async_final_account_dict_;
  struct EarlyFinalAccountRebind {
    td::Result<Ref<vm::Cell>> result;
    std::shared_ptr<vm::ProofStorageStat> loaded_cells;
    td::RealCpuTimer::Time worker_time;
    td::RealCpuTimer::Time wait_time;
    td::RealCpuTimer::Time rebind_time;
    td::uint32 batches{0};
    td::uint32 max_queue{0};
    td::thread thread;
    bool started{false};
  } early_final_account_rebind_;
  std::shared_ptr<vm::CellUsageTree> state_update_aux_usage_tree_;
  std::shared_ptr<vm::ProofStorageStat> state_update_aux_loaded_cells_;
  vm::CellUsageTree::NodeId state_update_aux_target_root_{0};
  std::set<td::Bits256> account_dict_estimator_added_accounts_;
  struct AccountDictEstimatorState {
    bool exists;
    td::Bits256 total_state_hash;
    td::Bits256 last_trans_hash;
    ton::LogicalTime last_trans_lt;
  };
  struct AccountDictEstimatorUpdate {
    td::Bits256 address;
    Ref<vm::Cell> value;
  };
  std::vector<AccountDictEstimatorUpdate> analytical_estimator_fallback_updates_;
  std::map<td::Bits256, AccountDictEstimatorState> account_dict_estimator_states_;
  std::vector<AccountDictEstimatorUpdate> account_dict_estimator_pending_updates_;
  std::vector<AccountDictEstimatorUpdate> final_account_dict_pending_updates_;
  unsigned account_dict_ops_{0};

  bool msg_metadata_enabled_ = false;
  bool deferring_messages_enabled_ = false;
  bool store_out_msg_queue_size_ = false;

  std::function<td::Ref<vm::Cell>(const td::Bits256&)> storage_stat_cache_;
  std::vector<std::pair<td::Ref<vm::Cell>, td::uint32>> storage_stat_cache_update_;

  td::PerfWarningTimer perf_timer_;
  td::PerfLog perf_log_;
  //
  block::Account* lookup_account(td::ConstBitPtr addr) const;
  std::unique_ptr<block::Account> make_account_from(td::ConstBitPtr addr, Ref<vm::CellSlice> account,
                                                    bool force_create);
  bool init_account_storage_dict(block::Account& account);
  td::Result<block::Account*> make_account(td::ConstBitPtr addr, bool force_create = false);
  bool prepare_accounts_parallel(const std::vector<StdSmcAddress>& addresses);
  td::actor::ActorId<Collator> get_self() {
    return actor_id(this);
  }
  bool init_utime();
  bool init_lt();
  bool fetch_config_params();
  bool fatal_error(td::Status error);
  bool fatal_error(int err_code, std::string err_msg);
  bool fatal_error(std::string err_msg, int err_code = -666);
  void check_pending();
  void after_get_mc_state(td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res, td::PerfLogAction token);
  void after_get_shard_state(int idx, td::Result<Ref<ShardState>> res, td::PerfLogAction token);
  void request_top_masterchain_state(BlockIdExt prev_mc_ref);
  void after_get_block_data(int idx, td::Result<Ref<BlockData>> res, td::PerfLogAction token);
  void after_get_shard_blocks(td::Result<std::vector<Ref<ShardTopBlockDescription>>> res, td::PerfLogAction token);
  void after_get_storage_stat_cache(td::Result<std::function<td::Ref<vm::Cell>(const td::Bits256&)>> res,
                                    td::PerfLogAction token);
  bool preprocess_prev_mc_state();
  bool register_mc_state(Ref<MasterchainStateQ> other_mc_state);
  bool request_aux_mc_state(BlockSeqno seqno, Ref<MasterchainStateQ>& state);
  Ref<MasterchainStateQ> get_aux_mc_state(BlockSeqno seqno) const;
  void after_get_aux_shard_state(ton::BlockIdExt blkid, td::Result<Ref<ShardState>> res, td::PerfLogAction token);
  bool fix_one_processed_upto(block::MsgProcessedUpto& proc, const ton::ShardIdFull& owner);
  bool fix_processed_upto(block::MsgProcessedUptoCollection& upto);
  void got_neighbor_msg_queues(td::Result<std::map<BlockIdExt, Ref<OutMsgQueueProof>>> R, td::PerfLogAction token);
  void got_neighbor_msg_queue(unsigned i, Ref<OutMsgQueueProof> res);
  void got_out_queue_size(size_t i, td::Result<td::uint64> res);
  bool adjust_shard_config();
  bool store_shard_fees(ShardIdFull shard, const block::CurrencyCollection& fees,
                        const block::CurrencyCollection& created);
  bool store_shard_fees(Ref<block::McShardHash> descr);
  bool import_new_shard_top_blocks();
  bool register_shard_block_creators(std::vector<td::Bits256> creator_list);
  bool init_block_limits();
  bool compute_minted_amount(block::CurrencyCollection& to_mint);
  bool create_output_queue_merger();
  bool init_value_create();
  bool try_collate();
  bool do_preinit();

  td::actor::Task<> do_collate();
  td::actor::Task<> do_collate_inner();
  bool create_special_transactions();
  bool create_special_transaction(block::CurrencyCollection amount, Ref<vm::Cell> dest_addr_cell,
                                  Ref<vm::Cell>& in_msg);
  bool create_ticktock_transactions(int mask);
  bool create_ticktock_transaction(const ton::StdSmcAddress& smc_addr, ton::LogicalTime req_start_lt, int mask);
  Ref<vm::Cell> create_ordinary_transaction(Ref<vm::Cell> msg_root, td::optional<block::MsgMetadata> msg_metadata,
                                            LogicalTime after_lt, bool is_special_tx = false);
  Ref<vm::Cell> commit_ordinary_transaction(std::unique_ptr<block::transaction::Transaction> trans,
                                            block::Account* acc, bool external,
                                            td::optional<block::MsgMetadata> msg_metadata, bool is_special_tx);
  LogicalTime adjust_after_lt(bool external, LogicalTime after_lt, const block::Account& acc) const;
  bool check_cur_validator_set();
  bool unpack_last_mc_state();
  bool unpack_last_state();
  bool unpack_merge_last_state();
  bool unpack_one_last_state(block::ShardState& ss, BlockIdExt blkid, Ref<vm::Cell> prev_state_root);
  bool split_last_state(block::ShardState& ss);
  bool import_shard_state_data(block::ShardState& ss);
  bool add_trivial_neighbor();
  bool add_trivial_neighbor_after_merge();
  bool out_msg_queue_cleanup();
  bool dequeue_message(Ref<vm::Cell> msg_envelope, ton::LogicalTime delivered_lt);
  bool check_prev_block(const BlockIdExt& listed, const BlockIdExt& prev, bool chk_chain_len = true);
  bool check_prev_block_exact(const BlockIdExt& listed, const BlockIdExt& prev);
  bool check_this_shard_mc_info();
  bool request_neighbor_msg_queues();
  bool request_out_msg_queue_size();
  void update_max_lt(ton::LogicalTime lt);
  bool is_masterchain() const {
    return shard_.is_masterchain();
  }
  int prev_block_idx(const BlockIdExt& id) const {
    for (size_t i = 0; i < prev_blocks.size(); ++i) {
      if (prev_blocks[i] == id) {
        return (int)i;
      }
    }
    return -1;
  }
  bool is_our_address(Ref<vm::CellSlice> addr_ref) const;
  bool is_our_address(ton::AccountIdPrefixFull addr_prefix) const;
  bool is_our_address(const ton::StdSmcAddress& addr) const;
  td::Status register_external_message(Ref<ExtMessage> ext_msg, int priority);
  td::actor::Task<> wait_for_external_message(td::Timestamp timeout);

  void register_new_msg(block::NewOutMsg msg);
  void track_new_message_account(const Ref<vm::Cell>& msg);
  bool prefetch_new_message_accounts();
  bool start_async_account_lookup_batch();
  bool finish_async_account_lookup_batch();
  bool prepare_async_account_for_message(const Ref<vm::Cell>& msg);
  void register_new_msgs(block::transaction::Transaction& trans, td::optional<block::MsgMetadata> msg_metadata);
  bool process_new_messages(bool& enqueue_only);
  struct NewMsgRoute {
    StdSmcAddress src_addr;
    StdSmcAddress dest_addr;
    td::RefInt256 fwd_fees;
  };
  int route_one_new_message(block::NewOutMsg& msg, bool enqueue_only, Ref<vm::Cell>* is_special, NewMsgRoute& route);
  int finalize_one_new_message(block::NewOutMsg msg, Ref<vm::Cell> trans_root, td::RefInt256 fwd_fees,
                               Ref<vm::Cell>* is_special);
  int process_one_new_message(block::NewOutMsg msg, bool enqueue_only = false, Ref<vm::Cell>* is_special = nullptr);
  bool process_new_messages_parallel(bool& enqueue_only);
  bool process_inbound_internal_messages();
  bool precheck_inbound_message(Ref<vm::CellSlice> msg, ton::LogicalTime lt);
  bool process_inbound_message(Ref<vm::CellSlice> msg, ton::LogicalTime lt, td::ConstBitPtr key, int src_nb_idx);
  td::actor::Task<> process_external_and_new_messages();
  td::actor::Task<bool> process_inbound_external_messages();
  td::actor::Task<bool> process_inbound_external_messages_parallel();
  int process_external_message(Ref<vm::Cell> msg);
  WaveExecutor& wave_executor();
  bool process_dispatch_queue();
  bool process_deferred_message(Ref<vm::CellSlice> enq_msg, StdSmcAddress src_addr, LogicalTime lt,
                                td::optional<block::MsgMetadata>& msg_metadata);
  bool enqueue_message(block::NewOutMsg msg, td::RefInt256 fwd_fees_remaining, StdSmcAddress src_addr,
                       bool defer = false);
  bool enqueue_transit_message(Ref<vm::Cell> msg, Ref<vm::Cell> old_msg_env, ton::AccountIdPrefixFull prev_prefix,
                               ton::AccountIdPrefixFull cur_prefix, ton::AccountIdPrefixFull dest_prefix,
                               td::RefInt256 fwd_fee_remaining, td::optional<block::MsgMetadata> msg_metadata,
                               td::optional<LogicalTime> emitted_lt, bool from_dispatch_queue);
  bool delete_out_msg_queue_msg(td::ConstBitPtr key);
  bool insert_in_msg(Ref<vm::Cell> in_msg);
  bool insert_out_msg(Ref<vm::Cell> out_msg);
  bool insert_out_msg(Ref<vm::Cell> out_msg, td::ConstBitPtr msg_hash);
  bool flush_msg_descr_updates(vm::AugmentedDictionary& dict, std::vector<MsgDescrUpdate>& pending_updates);
  bool flush_in_msg_descr_updates();
  bool flush_out_msg_descr_updates();
  bool flush_message_descriptor_updates();
  bool register_out_msg_queue_op(bool force = false);
  bool register_dispatch_queue_op(bool force = false);
  bool wait_account_dict_estimator(Ref<vm::Cell>* root = nullptr,
                                   std::shared_ptr<vm::CellUsageTree>* usage_tree = nullptr,
                                   std::shared_ptr<vm::ProofStorageStat>* loaded_cells = nullptr);
  bool enqueue_account_dict_estimator_update(td::Bits256 address, Ref<vm::Cell> value);
  bool flush_account_dict_estimator_updates();
  bool enqueue_final_account_dict_update(const block::Account& account);
  bool flush_final_account_dict_updates();
  bool start_early_final_account_rebind();
  td::Result<vm::NewCellStorageStat::Stat> estimate_analytical_account_dict_increment();
  bool update_account_dict_estimation(const block::transaction::Transaction& trans);
  void update_account_storage_dict_info(const block::transaction::Transaction& trans);
  bool update_min_mc_seqno(ton::BlockSeqno some_mc_seqno);
  bool process_account_storage_dict(block::Account& account);
  bool combine_account_transactions();
  bool update_public_libraries();
  bool update_account_public_libraries(Ref<vm::Cell> orig_libs, Ref<vm::Cell> final_libs, const td::Bits256& addr);
  bool add_public_library(td::ConstBitPtr key, td::ConstBitPtr addr, Ref<vm::Cell> library);
  bool remove_public_library(td::ConstBitPtr key, td::ConstBitPtr addr);
  bool check_block_overload();
  bool update_block_creator_count(td::ConstBitPtr key, unsigned shard_incr, unsigned mc_incr);
  int creator_count_outdated(td::ConstBitPtr key, vm::CellSlice& cs);
  bool update_block_creator_stats();
  bool create_mc_state_extra();
  bool create_shard_state();
  td::Result<Ref<vm::Cell>> get_config_data_from_smc(const ton::StdSmcAddress& cfg_addr);
  bool try_fetch_new_config(const ton::StdSmcAddress& cfg_addr, Ref<vm::Cell>& new_config);
  bool update_processed_upto();
  bool compute_out_msg_queue_info(Ref<vm::Cell>& out_msg_queue_info);
  bool compute_total_balance();
  bool store_master_ref(vm::CellBuilder& cb);
  bool store_prev_blk_ref(vm::CellBuilder& cb, bool after_merge);
  bool store_zero_state_ref(vm::CellBuilder& cb);
  bool store_version(vm::CellBuilder& cb) const;
  bool create_block_info(Ref<vm::Cell>& block_info);
  bool check_value_flow();
  bool create_block_extra(Ref<vm::Cell>& block_extra);
  bool update_shard_config(const block::WorkchainSet& wc_set, const block::CatchainValidatorsConfig& ccvc,
                           bool update_cc);
  bool create_mc_block_extra(Ref<vm::Cell>& mc_block_extra);
  bool create_block();

  Ref<vm::Cell> collate_shard_block_descr_set();
  bool prepare_proofs();
  td::Status finish_async_state_proof();
  bool create_collated_data();

  bool create_block_candidate(td::Result<td::BufferSlice>* early_block_boc = nullptr,
                              const td::Bits256* early_block_file_hash = nullptr,
                              td::Result<td::BufferSlice>* early_collated_boc = nullptr);
  void return_block_candidate();
  bool update_last_proc_int_msg(const std::pair<ton::LogicalTime, ton::Bits256>& new_lt_hash);

  td::CancellationToken cancellation_token_;
  bool check_cancelled();

 public:
  static td::uint32 get_skip_externals_queue_size();
  static int history_weight(td::uint64 history);

 private:
  CollationStats stats_;

  void finalize_stats();

  static thread_local AccountStorageDict* current_tx_storage_dict_;
  static thread_local WaveProofStats* current_wave_proof_stats_;
  std::recursive_mutex proof_stat_mutex_;
  std::unique_ptr<WaveExecutor> wave_executor_;

  void on_cell_loaded(const vm::LoadedCell& cell);
  void merge_wave_proof_stats(WaveProofStats& stats);
  void set_current_tx_storage_dict(const block::Account& account);
};

}  // namespace validator

}  // namespace ton
