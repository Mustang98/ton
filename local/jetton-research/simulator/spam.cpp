#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "auto/tl/lite_api.h"
#include "auto/tl/lite_api.hpp"
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/mc-config.h"
#include "keys/keys.hpp"
#include "lite-client/ext-client.h"
#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include "td/utils/base64.h"
#include "td/utils/filesystem.h"
#include "td/utils/misc.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/signals.h"
#include "tl-utils/lite-utils.hpp"
#include "ton/lite-tl.hpp"
#include "ton/ton-types.h"
#include "vm/boc.h"
#include "vm/dict.h"
#include "vm/vm.h"

#include "spam.h"
#include "workload.h"

namespace jetton_sim {
namespace {

constexpr td::uint32 kOpTransfer = 0x0f8a7ea5;
constexpr td::uint32 kOpInternalTransfer = 0x178d4519;
constexpr td::uint32 kOpExcess = 0xd53276db;

std::atomic<int> g_interrupts{0};
std::atomic<int> g_exit_code{0};

struct Bits256Hash {
  std::size_t operator()(const td::Bits256& value) const {
    std::size_t result;
    std::memcpy(&result, value.data(), sizeof(result));
    return result;
  }
};

td::BufferSlice envelope(td::BufferSlice query) {
  return ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_query>(std::move(query)), true);
}

td::Result<td::BufferSlice> unwrap_answer(td::Result<td::BufferSlice> result) {
  TRY_RESULT(data, std::move(result));
  auto error = ton::fetch_tl_object<ton::lite_api::liteServer_error>(data.clone(), true);
  if (error.is_ok()) {
    auto value = error.move_as_ok();
    return td::Status::Error(value->code_, value->message_);
  }
  return std::move(data);
}

std::string json_escape(td::Slice value) {
  std::string result;
  result.reserve(value.size());
  for (char c : value) {
    if (c == '"' || c == '\\') {
      result.push_back('\\');
      result.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
      result += buffer;
    } else {
      result.push_back(c);
    }
  }
  return result;
}

std::string shard_hex(ton::ShardId shard) {
  char buffer[19];
  std::snprintf(buffer, sizeof(buffer), "0x%016llX", static_cast<unsigned long long>(shard));
  return buffer;
}

double percentile(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) {
    return 0;
  }
  double position = p * static_cast<double>(sorted.size() - 1);
  auto low = static_cast<std::size_t>(position);
  auto high = std::min(low + 1, sorted.size() - 1);
  auto fraction = position - static_cast<double>(low);
  return sorted[low] * (1.0 - fraction) + sorted[high] * fraction;
}

struct LatencyStats {
  double p50{0};
  double p90{0};
  double p99{0};
  double mean{0};
  std::size_t samples{0};
};

struct RatePhase {
  double rate{0};
  double duration{0};
};

td::Result<std::vector<RatePhase>> parse_rate_phases(const SpamOptions& options) {
  if (options.rate_schedule.empty()) {
    return std::vector<RatePhase>{{options.rate, options.duration}};
  }
  std::vector<RatePhase> phases;
  std::stringstream input(options.rate_schedule);
  std::string token;
  bool has_positive_rate = false;
  while (std::getline(input, token, ',')) {
    auto separator = token.find(':');
    if (separator == std::string::npos) {
      return td::Status::Error("invalid --rate-schedule phase; expected rate:seconds");
    }
    auto rate = td::to_double(token.substr(0, separator));
    auto duration = td::to_double(token.substr(separator + 1));
    if (rate < 0 || duration <= 0) {
      return td::Status::Error("--rate-schedule rates must be non-negative and durations positive");
    }
    has_positive_rate |= rate > 0;
    phases.push_back(RatePhase{rate, duration});
  }
  if (phases.empty() || !has_positive_rate) {
    return td::Status::Error("--rate-schedule must contain at least one positive-rate phase");
  }
  return phases;
}

LatencyStats latency_stats(std::vector<double> values) {
  LatencyStats result;
  result.samples = values.size();
  if (values.empty()) {
    return result;
  }
  std::sort(values.begin(), values.end());
  result.p50 = percentile(values, 0.50);
  result.p90 = percentile(values, 0.90);
  result.p99 = percentile(values, 0.99);
  result.mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
  return result;
}

void append_latency_json(std::ostringstream& out, td::Slice name, const LatencyStats& stats) {
  out << '"' << name.str() << "\":{\"p50\":" << stats.p50 << ",\"p90\":" << stats.p90 << ",\"p99\":" << stats.p99
      << ",\"mean\":" << stats.mean << ",\"samples\":" << stats.samples << '}';
}

struct PresignedMessage {
  td::uint64 ordinal{0};
  td::uint32 sender_id{0};
  td::uint32 recipient_id{0};
  td::uint32 seqno{0};
  td::Bits256 external_hash{};
  td::BufferSlice query;
  bool sampled{false};
};

class SignerPool {
 public:
  SignerPool(td::Bits256 seed, td::Bits256 minter, std::vector<PoolAccount> accounts, td::uint64 target_total,
             td::uint64 target_buffer, td::uint64 rng_seed, double sample_rate, td::uint32 init_mode)
      : seed_(seed)
      , minter_(minter)
      , accounts_(std::move(accounts))
      , target_total_(target_total)
      , target_buffer_(std::max<td::uint64>(target_buffer, 1))
      , rng_(rng_seed)
      , sample_rng_(rng_seed ^ 0x9e3779b97f4a7c15ULL)
      , sample_rate_(sample_rate)
      , init_mode_(init_mode) {
    auto active_count = accounts_.size() - 1;
    permutation_.resize(active_count);
    std::iota(permutation_.begin(), permutation_.end(), 0);
    std::shuffle(permutation_.begin(), permutation_.end(), rng_);
    next_seqno_.resize(active_count, 0);
    next_seqno_[0] = 1;  // The deployment kickoff is WalletSpam #0's seqno zero external.
  }

  ~SignerPool() {
    stop();
  }

  void start(int thread_count) {
    for (int i = 0; i < thread_count; ++i) {
      threads_.emplace_back([this] { worker(); });
    }
  }

  void stop() {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      stop_ = true;
    }
    condition_.notify_all();
    for (auto& thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    threads_.clear();
  }

  std::vector<PresignedMessage> take(std::size_t max_count) {
    std::vector<PresignedMessage> result;
    std::lock_guard<std::mutex> guard(mutex_);
    while (result.size() < max_count) {
      auto it = ready_.find(next_take_);
      if (it == ready_.end()) {
        break;
      }
      result.push_back(std::move(it->second));
      ready_.erase(it);
      ++next_take_;
    }
    condition_.notify_all();
    return result;
  }

  td::Status error() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return error_.clone();
  }

  td::uint64 constructed() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return constructed_;
  }

 private:
  struct Job {
    td::uint64 ordinal{0};
    td::uint32 sender_id{0};
    td::uint32 recipient_id{0};
    td::uint32 seqno{0};
    td::uint32 comment{0};
    bool sampled{false};
  };

  Job next_job() {
    if (permutation_cursor_ == permutation_.size()) {
      std::shuffle(permutation_.begin(), permutation_.end(), rng_);
      permutation_cursor_ = 0;
    }
    Job job;
    job.ordinal = next_plan_++;
    job.sender_id = permutation_[permutation_cursor_++];
    job.recipient_id = static_cast<td::uint32>(rng_() % permutation_.size());
    job.seqno = next_seqno_[job.sender_id]++;
    job.comment = static_cast<td::uint32>(rng_());
    if (sample_rate_ > 0) {
      job.sampled = std::generate_canonical<double, 53>(sample_rng_) < sample_rate_;
    }
    return job;
  }

  void worker() {
    auto workload_result = Workload::create(seed_, minter_);
    if (workload_result.is_error()) {
      fail(workload_result.move_as_error());
      return;
    }
    auto workload = workload_result.move_as_ok();
    while (true) {
      Job job;
      {
        std::unique_lock<std::mutex> guard(mutex_);
        condition_.wait(
            guard, [&] { return stop_ || next_plan_ >= target_total_ || next_plan_ - next_take_ < target_buffer_; });
        if (stop_ || next_plan_ >= target_total_) {
          return;
        }
        job = next_job();
      }

      auto valid_until = static_cast<td::uint32>(td::Clocks::system()) + 120;
      auto message =
          workload.build_transfer(job.sender_id, accounts_[job.sender_id], job.recipient_id,
                                  accounts_[job.recipient_id], job.seqno, valid_until, job.comment, init_mode_);
      if (message.is_error()) {
        fail(message.move_as_error());
        return;
      }
      auto value = message.move_as_ok();
      auto boc = vm::std_boc_serialize(value.external, 31);
      if (boc.is_error()) {
        fail(boc.move_as_error());
        return;
      }
      PresignedMessage result;
      result.ordinal = job.ordinal;
      result.sender_id = job.sender_id;
      result.recipient_id = job.recipient_id;
      result.seqno = job.seqno;
      result.external_hash = value.external->get_hash().bits();
      result.sampled = job.sampled;
      result.query = envelope(ton::create_serialize_tl_object<ton::lite_api::liteServer_sendMessage>(boc.move_as_ok()));
      {
        std::lock_guard<std::mutex> guard(mutex_);
        ready_.emplace(result.ordinal, std::move(result));
        ++constructed_;
      }
      condition_.notify_all();
    }
  }

  void fail(td::Status status) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (error_.is_ok()) {
        error_ = std::move(status);
      }
      stop_ = true;
    }
    condition_.notify_all();
  }

  td::Bits256 seed_;
  td::Bits256 minter_;
  const std::vector<PoolAccount> accounts_;
  const td::uint64 target_total_;
  const td::uint64 target_buffer_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::map<td::uint64, PresignedMessage> ready_;
  std::vector<td::uint32> permutation_;
  std::vector<td::uint32> next_seqno_;
  std::mt19937_64 rng_;
  std::mt19937_64 sample_rng_;
  const double sample_rate_;
  const td::uint32 init_mode_;
  std::size_t permutation_cursor_{0};
  td::uint64 next_plan_{0};
  td::uint64 next_take_{0};
  td::uint64 constructed_{0};
  bool stop_{false};
  td::Status error_;
  std::vector<std::thread> threads_;
};

struct ParsedOutput {
  td::Bits256 hash{};
  std::optional<td::uint32> opcode;
};

struct ParsedTransaction {
  td::Bits256 account{};
  td::Bits256 in_hash{};
  int input_tag{-1};
  std::optional<td::uint32> opcode;
  bool success{false};
  std::vector<ParsedOutput> outputs;
};

std::optional<td::uint32> body_opcode(Ref<vm::Cell> message) {
  block::gen::Message::Record record;
  if (!tlb::type_unpack_cell(std::move(message), block::gen::t_Message_Any, record)) {
    return std::nullopt;
  }
  auto body = record.body.write();
  if (!body.fetch_ulong(1)) {
    if (body.size() < 32) {
      return std::nullopt;
    }
    return static_cast<td::uint32>(body.prefetch_ulong(32));
  }
  auto body_ref = body.fetch_ref();
  if (body_ref.is_null()) {
    return std::nullopt;
  }
  auto body_slice = vm::load_cell_slice(body_ref);
  if (body_slice.size() < 32) {
    return std::nullopt;
  }
  return static_cast<td::uint32>(body_slice.prefetch_ulong(32));
}

class BlockParser final : public td::actor::Actor {
 public:
  struct Sample {
    td::uint64 ordinal{0};
    td::uint32 sender_id{0};
    td::uint32 recipient_id{0};
    td::uint32 wallet_seqno{0};
    td::Bits256 external_hash{};
    double sent_at{0};
  };

  struct Trace {
    td::uint64 ordinal{0};
    td::uint32 sender_id{0};
    td::uint32 recipient_id{0};
    td::uint32 wallet_seqno{0};
    td::Bits256 external_hash{};
    std::string status;
    double sent_at{0};
    double tx1_at{0};
    double tx2_at{0};
    double tx3_at{0};
    double tx4_at{0};
    td::uint32 tx1_block{0};
    td::uint32 tx2_block{0};
    td::uint32 tx3_block{0};
    td::uint32 tx4_block{0};
    ton::ShardId tx1_shard{0};
    ton::ShardId tx2_shard{0};
    ton::ShardId tx3_shard{0};
    ton::ShardId tx4_shard{0};
  };

  struct Summary {
    td::uint32 utime{0};
    td::uint64 transactions{0};
    td::uint64 external_transactions{0};
    td::uint64 internal_transactions{0};
    td::uint64 aborted_transactions{0};
    td::uint64 tx2_attempts{0};
    td::uint64 tx2_success{0};
    td::uint64 tx3_attempts{0};
    td::uint64 tx3_success{0};
    td::uint64 tx4_attempts{0};
    td::uint64 tx4_success{0};
    bool after_merge{false};
    bool before_split{false};
    bool after_split{false};
    bool want_split{false};
    bool want_merge{false};
    std::vector<td::Bits256> external_in_hashes;
  };

  struct CompletionStats {
    td::uint64 registered{0};
    td::uint64 tx3_completed{0};
    td::uint64 tx4_completed{0};
    td::uint64 tracking_failures{0};
    std::vector<double> external_to_tx1_ms;
    std::vector<double> tx1_to_tx2_ms;
    std::vector<double> tx2_to_tx3_ms;
    std::vector<double> tx3_to_tx4_ms;
    std::vector<double> external_to_tx3_ms;
    std::vector<double> external_to_tx4_ms;
    std::vector<Trace> traces;
  };

  BlockParser(std::vector<PoolAccount> accounts) {
    owners_.reserve(accounts.size());
    jetton_wallets_.reserve(accounts.size());
    for (const auto& account : accounts) {
      owners_.insert(account.owner);
      jetton_wallets_.insert(account.jetton_wallet);
    }
  }

  void add_samples(std::vector<Sample> samples) {
    for (auto& sample : samples) {
      ++stats_.registered;
      Trace trace;
      trace.ordinal = sample.ordinal;
      trace.sender_id = sample.sender_id;
      trace.recipient_id = sample.recipient_id;
      trace.wallet_seqno = sample.wallet_seqno;
      trace.external_hash = sample.external_hash;
      trace.sent_at = sample.sent_at;
      stage1_.emplace(sample.external_hash, std::move(trace));
    }
  }

  void parse_block(ton::BlockIdExt block_id, td::BufferSlice data, double observed_at, td::Promise<Summary> promise) {
    promise.set_result([&]() -> td::Result<Summary> {
      try {
        return do_parse(block_id, std::move(data), observed_at);
      } catch (vm::VmError& error) {
        return error.as_status("block parse failed: ");
      } catch (vm::VmVirtError& error) {
        return error.as_status("block parse failed: ");
      }
    }());
  }

  void get_completion_stats(td::Promise<CompletionStats> promise) {
    append_incomplete(stage1_, "awaiting_tx1");
    append_incomplete(stage2_, "awaiting_tx2");
    append_incomplete(stage3_, "awaiting_tx3");
    append_incomplete(stage4_, "awaiting_tx4");
    promise.set_value(std::move(stats_));
  }

 private:
  td::Result<Summary> do_parse(const ton::BlockIdExt& block_id, td::BufferSlice data, double observed_at) {
    TRY_RESULT(root, vm::std_boc_deserialize(std::move(data)));
    if (block_id.root_hash != td::Bits256{root->get_hash().bits()}) {
      return td::Status::Error(PSLICE() << "block root hash mismatch for " << block_id.to_str());
    }
    block::gen::Block::Record block;
    block::gen::BlockInfo::Record info;
    block::gen::BlockExtra::Record extra;
    if (!(tlb::unpack_cell(root, block) && tlb::unpack_cell(block.info, info) &&
          tlb::unpack_cell(block.extra, extra))) {
      return td::Status::Error(PSLICE() << "cannot unpack block " << block_id.to_str());
    }

    Summary summary;
    summary.utime = info.gen_utime;
    summary.after_merge = info.after_merge;
    summary.before_split = info.before_split;
    summary.after_split = info.after_split;
    summary.want_split = info.want_split;
    summary.want_merge = info.want_merge;
    std::vector<ParsedTransaction> transactions;
    vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(extra.account_blocks), 256,
                                     block::tlb::aug_ShardAccountBlocks};
    bool valid = accounts.check_for_each_extra(
        [&](Ref<vm::CellSlice> value, Ref<vm::CellSlice>, td::ConstBitPtr account_key, int) {
          td::Bits256 account;
          account.bits().copy_from(account_key, 256);
          block::gen::AccountBlock::Record account_block;
          if (!tlb::csr_unpack(std::move(value), account_block)) {
            return false;
          }
          vm::AugmentedDictionary tx_dict{vm::DictNonEmpty(), std::move(account_block.transactions), 64,
                                          block::tlb::aug_AccountTransactions};
          return tx_dict.check_for_each([&](Ref<vm::CellSlice> tx_value, td::ConstBitPtr, int) {
            auto tx_cell = tx_value->prefetch_ref();
            block::gen::Transaction::Record transaction;
            if (tx_cell.is_null() || !tlb::unpack_cell(tx_cell, transaction)) {
              return false;
            }
            ParsedTransaction parsed;
            parsed.account = account;
            auto input = transaction.r1.in_msg->prefetch_ref();
            if (input.not_null()) {
              parsed.in_hash = input->get_hash().bits();
              block::gen::Message::Record message;
              if (tlb::type_unpack_cell(input, block::gen::t_Message_Any, message)) {
                parsed.input_tag = block::gen::t_CommonMsgInfo.get_tag(*message.info);
                parsed.opcode = body_opcode(input);
              }
            }
            block::gen::TransactionDescr::Record_trans_ord description;
            parsed.success = tlb::unpack_cell(transaction.description, description) && !description.aborted;

            vm::Dictionary outputs{transaction.r1.out_msgs, 15};
            if (!outputs.check_for_each([&](Ref<vm::CellSlice> output_value, td::ConstBitPtr, int) {
                  auto output = output_value->prefetch_ref();
                  if (output.is_null()) {
                    return false;
                  }
                  parsed.outputs.push_back(ParsedOutput{td::Bits256{output->get_hash().bits()}, body_opcode(output)});
                  return true;
                })) {
              return false;
            }
            transactions.push_back(std::move(parsed));
            return true;
          });
        });
    if (!valid) {
      return td::Status::Error(PSLICE() << "cannot enumerate transactions in " << block_id.to_str());
    }

    for (const auto& transaction : transactions) {
      ++summary.transactions;
      if (transaction.input_tag == block::gen::CommonMsgInfo::ext_in_msg_info) {
        ++summary.external_transactions;
        summary.external_in_hashes.push_back(transaction.in_hash);
      } else if (transaction.input_tag == block::gen::CommonMsgInfo::int_msg_info) {
        ++summary.internal_transactions;
      }
      if (!transaction.success) {
        ++summary.aborted_transactions;
      }
      if (!transaction.opcode) {
        continue;
      }
      if (*transaction.opcode == kOpTransfer && jetton_wallets_.count(transaction.account)) {
        ++summary.tx2_attempts;
        summary.tx2_success += transaction.success;
      } else if (*transaction.opcode == kOpInternalTransfer && jetton_wallets_.count(transaction.account)) {
        ++summary.tx3_attempts;
        summary.tx3_success += transaction.success;
      } else if (*transaction.opcode == kOpExcess && owners_.count(transaction.account)) {
        ++summary.tx4_attempts;
        summary.tx4_success += transaction.success;
      }
    }
    promote_samples(transactions, block_id.id.shard, block_id.id.seqno, observed_at);
    return summary;
  }

  static const ParsedOutput* output_with_opcode(const ParsedTransaction& transaction, td::uint32 opcode) {
    for (const auto& output : transaction.outputs) {
      if (output.opcode && *output.opcode == opcode) {
        return &output;
      }
    }
    return nullptr;
  }

  void promote_samples(const std::vector<ParsedTransaction>& transactions, ton::ShardId block_shard,
                       td::uint32 block_seqno, double observed_at) {
    std::unordered_map<td::Bits256, const ParsedTransaction*, Bits256Hash> by_input;
    by_input.reserve(transactions.size());
    for (const auto& transaction : transactions) {
      if (!transaction.in_hash.is_zero()) {
        by_input.emplace(transaction.in_hash, &transaction);
      }
    }

    promote(stage1_, stage2_, by_input, kOpTransfer, "tx1", [&](Trace& trace) {
      trace.tx1_at = observed_at;
      trace.tx1_shard = block_shard;
      trace.tx1_block = block_seqno;
      stats_.external_to_tx1_ms.push_back((observed_at - trace.sent_at) * 1e3);
    });
    promote(stage2_, stage3_, by_input, kOpInternalTransfer, "tx2", [&](Trace& trace) {
      trace.tx2_at = observed_at;
      trace.tx2_shard = block_shard;
      trace.tx2_block = block_seqno;
      stats_.tx1_to_tx2_ms.push_back((observed_at - trace.tx1_at) * 1e3);
    });
    promote(stage3_, stage4_, by_input, kOpExcess, "tx3", [&](Trace& trace) {
      trace.tx3_at = observed_at;
      trace.tx3_shard = block_shard;
      trace.tx3_block = block_seqno;
      ++stats_.tx3_completed;
      stats_.tx2_to_tx3_ms.push_back((observed_at - trace.tx2_at) * 1e3);
      stats_.external_to_tx3_ms.push_back((observed_at - trace.sent_at) * 1e3);
    });

    std::vector<td::Bits256> completed;
    for (auto& [hash, trace] : stage4_) {
      auto it = by_input.find(hash);
      if (it == by_input.end()) {
        continue;
      }
      if (!it->second->success || it->second->opcode != kOpExcess) {
        ++stats_.tracking_failures;
        trace.status = "tx4_failed";
      } else {
        trace.tx4_at = observed_at;
        trace.tx4_shard = block_shard;
        trace.tx4_block = block_seqno;
        trace.status = "complete";
        ++stats_.tx4_completed;
        stats_.tx3_to_tx4_ms.push_back((observed_at - trace.tx3_at) * 1e3);
        stats_.external_to_tx4_ms.push_back((observed_at - trace.sent_at) * 1e3);
      }
      stats_.traces.push_back(std::move(trace));
      completed.push_back(hash);
    }
    for (const auto& hash : completed) {
      stage4_.erase(hash);
    }
  }

  template <class Callback>
  void promote(std::unordered_map<td::Bits256, Trace, Bits256Hash>& source,
               std::unordered_map<td::Bits256, Trace, Bits256Hash>& destination,
               const std::unordered_map<td::Bits256, const ParsedTransaction*, Bits256Hash>& by_input,
               td::uint32 output_opcode, td::Slice stage, Callback callback) {
    std::vector<td::Bits256> consumed;
    std::vector<std::pair<td::Bits256, Trace>> promoted;
    for (auto& [hash, trace] : source) {
      auto it = by_input.find(hash);
      if (it == by_input.end()) {
        continue;
      }
      auto* transaction = it->second;
      consumed.push_back(hash);
      if (!transaction->success) {
        ++stats_.tracking_failures;
        trace.status = PSTRING() << stage << "_failed";
        stats_.traces.push_back(std::move(trace));
        continue;
      }
      auto* output = output_with_opcode(*transaction, output_opcode);
      if (output == nullptr) {
        ++stats_.tracking_failures;
        trace.status = PSTRING() << stage << "_missing_output";
        stats_.traces.push_back(std::move(trace));
        continue;
      }
      callback(trace);
      promoted.emplace_back(output->hash, std::move(trace));
    }
    for (const auto& hash : consumed) {
      source.erase(hash);
    }
    for (auto& item : promoted) {
      auto inserted = destination.try_emplace(item.first, std::move(item.second));
      if (!inserted.second) {
        ++stats_.tracking_failures;
        item.second.status = PSTRING() << stage << "_hash_collision";
        stats_.traces.push_back(std::move(item.second));
      }
    }
  }

  void append_incomplete(std::unordered_map<td::Bits256, Trace, Bits256Hash>& source, td::Slice status) {
    for (auto& [_, trace] : source) {
      trace.status = status.str();
      stats_.traces.push_back(std::move(trace));
    }
    source.clear();
  }

  std::unordered_set<td::Bits256, Bits256Hash> owners_;
  std::unordered_set<td::Bits256, Bits256Hash> jetton_wallets_;
  std::unordered_map<td::Bits256, Trace, Bits256Hash> stage1_;
  std::unordered_map<td::Bits256, Trace, Bits256Hash> stage2_;
  std::unordered_map<td::Bits256, Trace, Bits256Hash> stage3_;
  std::unordered_map<td::Bits256, Trace, Bits256Hash> stage4_;
  CompletionStats stats_;
};

class SpamRunner final : public td::actor::Actor {
 public:
  SpamRunner(SpamOptions options, td::Bits256 seed, td::Bits256 minter, std::vector<PoolAccount> accounts,
             td::Bits256 wallet_code_hash, td::Bits256 jetton_code_hash, std::vector<RatePhase> rate_phases,
             ton::adnl::AdnlNodeIdFull server_id, td::IPAddress server_address)
      : options_(std::move(options))
      , rate_phases_(std::move(rate_phases))
      , seed_(seed)
      , minter_(minter)
      , accounts_(std::move(accounts))
      , wallet_code_hash_(wallet_code_hash)
      , jetton_code_hash_(jetton_code_hash)
      , server_id_(std::move(server_id))
      , server_address_(server_address) {
  }

  void start_up() override {
    started_at_ = td::Time::now();
    unix_offset_ = td::Clocks::system() - started_at_;
    last_send_tick_ = started_at_;
    next_progress_ = started_at_ + 5.0;
    next_timeline_ = started_at_;
    scheduled_duration_ = 0;
    max_rate_ = 0;
    target_total_ = 0;
    for (const auto& phase : rate_phases_) {
      scheduled_duration_ += phase.duration;
      max_rate_ = std::max(max_rate_, phase.rate);
      target_total_ += static_cast<td::uint64>(phase.rate * phase.duration + 0.5);
    }
    max_inflight_ =
        options_.max_inflight == 0 ? static_cast<td::uint64>(max_rate_ * 2.0) + 10 : options_.max_inflight;
    auto presign = options_.presign == 0
                       ? std::max<td::uint64>(static_cast<td::uint64>(std::ceil(max_rate_ * 2.0)), 1)
                       : options_.presign;
    int signer_threads = options_.signer_threads == 0 ? td::clamp(static_cast<int>(max_rate_ / 2'500.0) + 1, 1, 8)
                                                      : options_.signer_threads;

    for (int i = 0; i < options_.connections + 1; ++i) {
      clients_.push_back(liteclient::ExtClient::create(server_id_, server_address_, nullptr));
    }
    parser_ = td::actor::create_actor<BlockParser>("jetton-block-parser", accounts_);
    signer_ = std::make_unique<SignerPool>(seed_, minter_, accounts_, target_total_, presign, options_.rng_seed,
                                           options_.track_sample, options_.init_mode);
    signer_->start(signer_threads);
    LOG(INFO) << "exact jetton-spam: pool=" << options_.pool_size << " target=" << target_total_
              << " init_mode=" << options_.init_mode << " phases=" << rate_phases_.size() << " max_rate=" << max_rate_
              << "/s max_inflight=" << max_inflight_ << " presign=" << presign << " signer_threads=" << signer_threads;
    alarm_timestamp() = td::Timestamp::in(0.005);
  }

  void alarm() override {
    auto now = td::Time::now();
    if (g_interrupts.load(std::memory_order_relaxed) > 0 && !draining_) {
      LOG(WARNING) << "interrupt received; stopping offers and draining";
      begin_drain(now);
    }
    if (g_interrupts.load(std::memory_order_relaxed) > 1 && !finishing_) {
      finish();
      return;
    }
    tick_watch();
    tick_send(now);
    tick_timeline(now);
    tick_progress(now);
    if (draining_ && !finishing_ && now >= drain_until_) {
      finish();
    }
    if (!finishing_) {
      alarm_timestamp() = td::Timestamp::in(0.005);
    }
  }

 private:
  struct SentRecord {
    double sent_at{0};
    td::uint32 sender_id{0};
    td::uint32 recipient_id{0};
    td::uint32 seqno{0};
  };

  struct BlockRecord {
    ton::ShardId shard{0};
    td::uint32 seqno{0};
    td::Bits256 root_hash{};
    td::uint32 utime{0};
    double observed_at{0};
    td::uint64 transactions{0};
    td::uint64 tx1{0};
    td::uint64 tx2_attempts{0};
    td::uint64 tx2_success{0};
    td::uint64 tx3_attempts{0};
    td::uint64 tx3_success{0};
    td::uint64 tx4_attempts{0};
    td::uint64 tx4_success{0};
    td::uint64 aborted{0};
    bool after_merge{false};
    bool before_split{false};
    bool after_split{false};
    bool want_split{false};
    bool want_merge{false};
  };

  struct TopologyRecord {
    double observed_at{0};
    std::vector<ton::BlockId> shards;
  };

  struct TimelineRecord {
    double time{0};
    std::size_t phase{0};
    double target_rate{0};
    td::uint64 constructed{0};
    td::uint64 offered{0};
    td::uint64 acknowledged{0};
    td::uint64 send_errors{0};
    td::uint64 tx1{0};
    td::uint64 tx3{0};
  };

  void tick_send(double now) {
    if (sending_done_ || !watcher_ready_) {
      last_send_tick_ = now;
      return;
    }
    auto error = signer_->error();
    if (error.is_error()) {
      LOG(ERROR) << "signer failed: " << error;
      g_exit_code.store(2);
      begin_drain(now);
      return;
    }
    auto elapsed = now - last_send_tick_;
    last_send_tick_ = now;
    auto [phase_index, rate] = current_rate(now);
    if (phase_index != current_phase_index_) {
      current_phase_index_ = phase_index;
      LOG(INFO) << "rate phase " << phase_index << " started at " << rate << " requests/s";
    }
    auto burst_limit = std::max(rate * 0.02, 8.0);
    tokens_ = std::min(tokens_ + rate * elapsed, burst_limit);
    auto room = max_inflight_ > inflight_ ? max_inflight_ - inflight_ : 0;
    auto count = std::min<td::uint64>(static_cast<td::uint64>(tokens_), room);
    count = std::min(count, target_total_ - offered_);
    auto batch = signer_->take(static_cast<std::size_t>(count));
    tokens_ -= static_cast<double>(batch.size());
    std::vector<BlockParser::Sample> samples;
    for (auto& message : batch) {
      if (first_offer_at_ == 0) {
        first_offer_at_ = now;
      }
      last_offer_at_ = now;
      pending_.emplace(message.external_hash, SentRecord{now, message.sender_id, message.recipient_id, message.seqno});
      if (message.sampled) {
        samples.push_back(BlockParser::Sample{message.ordinal, message.sender_id, message.recipient_id, message.seqno,
                                              message.external_hash, now});
      }
      auto promise = td::PromiseCreator::lambda([self = actor_id(this)](td::Result<td::BufferSlice> result) {
        td::actor::send_closure(self, &SpamRunner::on_send_result, std::move(result));
      });
      auto& client = clients_[1 + (sender_round_robin_++ % options_.connections)];
      td::actor::send_closure(client, &liteclient::ExtClient::send_query, "sendMessage", std::move(message.query),
                              td::Timestamp::in(10.0), std::move(promise));
      ++offered_;
      ++inflight_;
    }
    if (!samples.empty()) {
      td::actor::send_closure(parser_, &BlockParser::add_samples, std::move(samples));
    }
    if (first_offer_at_ != 0 && now - first_offer_at_ >= scheduled_duration_) {
      if (offered_ < target_total_) {
        LOG(WARNING) << "offer window ended after " << offered_ << '/' << target_total_
                     << " messages; generator/transport did not sustain target";
      }
      begin_drain(now);
    }
  }

  std::pair<std::size_t, double> current_rate(double now) const {
    auto elapsed = first_offer_at_ == 0 ? 0.0 : now - first_offer_at_;
    double boundary = 0;
    for (std::size_t i = 0; i < rate_phases_.size(); ++i) {
      boundary += rate_phases_[i].duration;
      if (elapsed < boundary || i + 1 == rate_phases_.size()) {
        return {i, rate_phases_[i].rate};
      }
    }
    return {rate_phases_.size() - 1, rate_phases_.back().rate};
  }

  void on_send_result(td::Result<td::BufferSlice> result) {
    if (inflight_ > 0) {
      --inflight_;
    }
    auto data = unwrap_answer(std::move(result));
    if (data.is_error()) {
      ++send_errors_;
      auto message = data.error().message().str();
      send_error_categories_[message.substr(0, 100)]++;
      return;
    }
    auto status = ton::fetch_tl_object<ton::lite_api::liteServer_sendMsgStatus>(data.move_as_ok(), true);
    if (status.is_error()) {
      ++send_errors_;
      send_error_categories_["invalid sendMsgStatus"]++;
    } else if (status.ok()->status_ == 1) {
      ++acknowledged_;
    } else {
      ++send_errors_;
      send_error_categories_[PSTRING() << "sendMsgStatus=" << status.ok()->status_]++;
    }
  }

  void tick_watch() {
    if (watcher_busy_ || !watcher_retry_.is_in_past()) {
      return;
    }
    watcher_busy_ = true;
    if (watcher_ready_ && !pending_blocks_.empty()) {
      request_next_block();
    } else {
      request_masterchain_info();
    }
  }

  void watcher_retry(double delay) {
    watcher_busy_ = false;
    watcher_retry_ = td::Timestamp::in(delay);
  }

  void lite_query(td::BufferSlice query, double timeout, void (SpamRunner::*handler)(td::Result<td::BufferSlice>)) {
    auto promise = td::PromiseCreator::lambda([self = actor_id(this), handler](td::Result<td::BufferSlice> result) {
      td::actor::send_closure(self, handler, unwrap_answer(std::move(result)));
    });
    td::actor::send_closure(clients_[0], &liteclient::ExtClient::send_query, "watch", envelope(std::move(query)),
                            td::Timestamp::in(timeout), std::move(promise));
  }

  void request_masterchain_info() {
    lite_query(ton::create_serialize_tl_object<ton::lite_api::liteServer_getMasterchainInfo>(), 5.0,
               &SpamRunner::on_masterchain_info);
  }

  void on_masterchain_info(td::Result<td::BufferSlice> result) {
    auto status = [&]() -> td::Status {
      TRY_RESULT(data, std::move(result));
      TRY_RESULT(info, ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfo>(std::move(data), true));
      auto last = ton::create_block_id(info->last_);
      lite_query(ton::create_serialize_tl_object<ton::lite_api::liteServer_getAllShardsInfo>(
                     ton::create_tl_lite_block_id(last)),
                 5.0, &SpamRunner::on_shards);
      return td::Status::OK();
    }();
    if (status.is_error()) {
      LOG(INFO) << "waiting for masterchain/liteserver: " << status;
      watcher_retry(0.2);
    }
  }

  void on_shards(td::Result<td::BufferSlice> result) {
    auto status = [&]() -> td::Status {
      TRY_RESULT(data, std::move(result));
      TRY_RESULT(info, ton::fetch_tl_object<ton::lite_api::liteServer_allShardsInfo>(std::move(data), true));
      TRY_RESULT(root, vm::std_boc_deserialize(std::move(info->data_)));
      block::ShardConfig config;
      if (!config.unpack(vm::load_cell_slice_ref(root))) {
        return td::Status::Error("cannot unpack ShardHashes");
      }
      auto tops = config.get_shard_hash_ids(std::function<bool(ton::ShardIdFull, bool)>{
          [](ton::ShardIdFull shard, bool) { return shard.workchain == ton::basechainId; }});
      if (tops.empty()) {
        return td::Status::Error("workchain-0 shards are not available");
      }
      std::sort(tops.begin(), tops.end(), [](const ton::BlockId& lhs, const ton::BlockId& rhs) {
        return std::tie(lhs.seqno, lhs.shard) < std::tie(rhs.seqno, rhs.shard);
      });

      std::set<ton::ShardId> new_active;
      for (const auto& top : tops) {
        new_active.insert(top.shard);
      }
      if (!watcher_ready_) {
        for (const auto& top : tops) {
          shard_cursors_[top.shard] = ShardCursor{top.seqno, true};
        }
        active_shards_ = new_active;
        topology_.push_back(TopologyRecord{td::Time::now(), tops});
        watcher_ready_ = true;
        LOG(INFO) << "workchain watcher ready with " << tops.size() << " shard(s)";
        watcher_retry(0);
        return td::Status::OK();
      }

      auto old_active = active_shards_;
      for (auto& [_, cursor] : shard_cursors_) {
        cursor.active = false;
      }
      for (const auto& top : tops) {
        td::uint32 base = top.seqno;
        auto cursor = shard_cursors_.find(top.shard);
        if (old_active.count(top.shard) && cursor != shard_cursors_.end()) {
          base = std::min(cursor->second.scheduled_through, top.seqno);
        } else {
          bool inherited = false;
          if (top.shard != ton::shardIdAll) {
            auto parent = shard_cursors_.find(ton::shard_parent(top.shard));
            if (parent != shard_cursors_.end()) {
              base = std::min(parent->second.scheduled_through, top.seqno);
              inherited = true;
            }
          }
          auto left = shard_cursors_.find(ton::shard_child(top.shard, true));
          auto right = shard_cursors_.find(ton::shard_child(top.shard, false));
          if (left != shard_cursors_.end() && right != shard_cursors_.end()) {
            base = std::min(std::max(left->second.scheduled_through, right->second.scheduled_through), top.seqno);
            inherited = true;
          }
          if (!inherited && cursor != shard_cursors_.end()) {
            base = std::min(cursor->second.scheduled_through, top.seqno);
          } else if (!inherited && top.seqno > 0) {
            base = top.seqno - 1;
          }
        }
        for (td::uint32 seqno = base + 1; seqno <= top.seqno; ++seqno) {
          pending_blocks_.emplace_back(ton::basechainId, top.shard, seqno);
        }
        shard_cursors_[top.shard] = ShardCursor{top.seqno, true};
      }
      active_shards_ = std::move(new_active);
      if (active_shards_ != old_active) {
        topology_.push_back(TopologyRecord{td::Time::now(), tops});
        LOG(INFO) << "workchain topology changed from " << old_active.size() << " to " << active_shards_.size()
                  << " shard(s)";
      }
      std::sort(pending_blocks_.begin(), pending_blocks_.end(), [](const ton::BlockId& lhs, const ton::BlockId& rhs) {
        return std::tie(lhs.seqno, lhs.shard) < std::tie(rhs.seqno, rhs.shard);
      });
      watcher_retry(pending_blocks_.empty() ? 0.04 : 0.0);
      return td::Status::OK();
    }();
    if (status.is_error()) {
      LOG(INFO) << "waiting for workchain shard: " << status;
      watcher_retry(0.2);
    }
  }

  void request_next_block() {
    CHECK(!pending_blocks_.empty());
    current_simple_block_ = pending_blocks_.front();
    auto query = ton::create_serialize_tl_object<ton::lite_api::liteServer_lookupBlock>(
        1, ton::create_tl_lite_block_id_simple(*current_simple_block_), 0, 0);
    lite_query(std::move(query), 5.0, &SpamRunner::on_lookup);
  }

  void on_lookup(td::Result<td::BufferSlice> result) {
    auto status = [&]() -> td::Status {
      TRY_RESULT(data, std::move(result));
      TRY_RESULT(header, ton::fetch_tl_object<ton::lite_api::liteServer_blockHeader>(std::move(data), true));
      auto block_id = ton::create_block_id(header->id_);
      if (!current_simple_block_ || block_id.id != *current_simple_block_) {
        return td::Status::Error("lookupBlock returned a different seqno");
      }
      current_observed_at_ = td::Time::now();
      request_block(block_id);
      return td::Status::OK();
    }();
    if (status.is_error()) {
      watcher_retry(0.04);
    }
  }

  void request_block(ton::BlockIdExt block_id) {
    auto query =
        ton::create_serialize_tl_object<ton::lite_api::liteServer_getBlock>(ton::create_tl_lite_block_id(block_id));
    auto promise = td::PromiseCreator::lambda([self = actor_id(this), block_id](td::Result<td::BufferSlice> result) {
      td::actor::send_closure(self, &SpamRunner::on_block, block_id, unwrap_answer(std::move(result)));
    });
    td::actor::send_closure(clients_[0], &liteclient::ExtClient::send_query, "getBlock", envelope(std::move(query)),
                            td::Timestamp::in(15.0), std::move(promise));
  }

  static bool transient_block_error(const td::Status& status) {
    auto message = status.message().str();
    return message.find("not found") != std::string::npos || message.find("out of sync") != std::string::npos ||
           message.find("notready") != std::string::npos;
  }

  void on_block(ton::BlockIdExt block_id, td::Result<td::BufferSlice> result) {
    if (result.is_error()) {
      if (transient_block_error(result.error()) && ++transient_block_retries_ < 200) {
        watcher_retry(0.08);
        return;
      }
      if (++block_fetch_failures_ < 5) {
        LOG(WARNING) << "getBlock(" << block_id.id.seqno << ") failed: " << result.error();
        watcher_retry(0.1);
        return;
      }
      LOG(ERROR) << "skipping unavailable block (" << block_id.id.workchain << ',' << shard_hex(block_id.id.shard)
                 << ',' << block_id.id.seqno << "): stage counts are incomplete";
      ++blocks_skipped_;
      advance_watcher();
      return;
    }
    auto response = ton::fetch_tl_object<ton::lite_api::liteServer_blockData>(result.move_as_ok(), true);
    if (response.is_error()) {
      ++parse_failures_;
      advance_watcher();
      return;
    }
    bytes_fetched_ += response.ok()->data_.size();
    auto promise = td::PromiseCreator::lambda(
        [self = actor_id(this), block_id, observed_at = current_observed_at_](td::Result<BlockParser::Summary> parsed) {
          td::actor::send_closure(self, &SpamRunner::on_block_parsed, block_id, observed_at, std::move(parsed));
        });
    td::actor::send_closure(parser_, &BlockParser::parse_block, block_id, std::move(response.move_as_ok()->data_),
                            current_observed_at_, std::move(promise));
  }

  void on_block_parsed(ton::BlockIdExt block_id, double observed_at, td::Result<BlockParser::Summary> result) {
    if (result.is_error()) {
      LOG(ERROR) << "block " << block_id.id.seqno << " parse failed: " << result.error();
      ++parse_failures_;
      ++blocks_skipped_;
      advance_watcher();
      return;
    }
    auto summary = result.move_as_ok();
    BlockRecord record;
    record.shard = block_id.id.shard;
    record.seqno = block_id.id.seqno;
    record.root_hash = block_id.root_hash;
    record.utime = summary.utime;
    record.observed_at = observed_at;
    record.transactions = summary.transactions;
    record.tx2_attempts = summary.tx2_attempts;
    record.tx2_success = summary.tx2_success;
    record.tx3_attempts = summary.tx3_attempts;
    record.tx3_success = summary.tx3_success;
    record.tx4_attempts = summary.tx4_attempts;
    record.tx4_success = summary.tx4_success;
    record.aborted = summary.aborted_transactions;
    record.after_merge = summary.after_merge;
    record.before_split = summary.before_split;
    record.after_split = summary.after_split;
    record.want_split = summary.want_split;
    record.want_merge = summary.want_merge;
    for (const auto& hash : summary.external_in_hashes) {
      auto it = pending_.find(hash);
      if (it == pending_.end()) {
        continue;
      }
      inclusion_latency_ms_.push_back((observed_at - it->second.sent_at) * 1e3);
      pending_.erase(it);
      ++record.tx1;
      ++tx1_total_;
    }
    tx3_total_ += record.tx3_success;
    blocks_.push_back(record);
    advance_watcher();
  }

  void advance_watcher() {
    if (!pending_blocks_.empty()) {
      pending_blocks_.pop_front();
    }
    current_simple_block_.reset();
    block_fetch_failures_ = 0;
    transient_block_retries_ = 0;
    watcher_retry(0);
  }

  void begin_drain(double now) {
    if (draining_) {
      return;
    }
    sending_done_ = true;
    draining_ = true;
    drain_until_ = now + options_.drain;
    LOG(INFO) << "draining for " << options_.drain << "s after " << offered_ << " offered messages";
  }

  void tick_timeline(double now) {
    if (now < next_timeline_) {
      return;
    }
    next_timeline_ = now + 1.0;
    auto [phase, rate] = current_rate(now);
    timeline_.push_back(TimelineRecord{now, phase, rate, signer_ ? signer_->constructed() : 0, offered_, acknowledged_,
                                       send_errors_, tx1_total_, tx3_total_});
  }

  void tick_progress(double now) {
    if (now < next_progress_) {
      return;
    }
    next_progress_ = now + 5.0;
    td::uint64 tx3 = 0;
    td::uint64 transactions = 0;
    auto cutoff = now - 5.0;
    double lower = now;
    for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it) {
      lower = it->observed_at;
      if (it->observed_at < cutoff) {
        break;
      }
      tx3 += it->tx3_success;
      transactions += it->transactions;
    }
    auto span = now - lower;
    LOG(INFO) << "progress offered=" << offered_ << '/' << target_total_ << " ack=" << acknowledged_
              << " errors=" << send_errors_ << " inflight=" << inflight_ << " included=" << tx1_total_
              << " completed=" << tx3_total_ << " pending=" << pending_.size()
              << " recent=" << (span > 0 ? static_cast<double>(tx3) / span : 0) << " jTPS, "
              << (span > 0 ? static_cast<double>(transactions) / span : 0) << " raw TPS";
  }

  void finish() {
    if (finishing_) {
      return;
    }
    finishing_ = true;
    auto promise = td::PromiseCreator::lambda([self = actor_id(this)](td::Result<BlockParser::CompletionStats> result) {
      td::actor::send_closure(self, &SpamRunner::on_completion_stats, std::move(result));
    });
    td::actor::send_closure(parser_, &BlockParser::get_completion_stats, std::move(promise));
  }

  void on_completion_stats(td::Result<BlockParser::CompletionStats> result) {
    BlockParser::CompletionStats completion;
    if (result.is_ok()) {
      completion = result.move_as_ok();
    } else {
      LOG(ERROR) << "cannot obtain completion statistics: " << result.error();
      g_exit_code.store(2);
    }
    write_outputs(completion);
    signer_->stop();
    if (tx1_total_ == 0 && g_exit_code.load() == 0) {
      LOG(ERROR) << "no offered external was observed in a block";
      g_exit_code.store(1);
    }
    td::actor::SchedulerContext::get().stop();
    stop();
  }

  double unix_ms(double monotonic_time) const {
    return (monotonic_time + unix_offset_) * 1e3;
  }

  void write_outputs(const BlockParser::CompletionStats& completion) {
    auto window_start = first_offer_at_ + options_.warmup;
    auto window_end = last_offer_at_;
    if (first_offer_at_ == 0 || window_end <= window_start) {
      window_start = first_offer_at_;
    }
    auto window = window_end > window_start ? window_end - window_start : 0.0;
    BlockRecord total;
    BlockRecord steady;
    for (const auto& block : blocks_) {
      total.transactions += block.transactions;
      total.tx1 += block.tx1;
      total.tx2_attempts += block.tx2_attempts;
      total.tx2_success += block.tx2_success;
      total.tx3_attempts += block.tx3_attempts;
      total.tx3_success += block.tx3_success;
      total.tx4_attempts += block.tx4_attempts;
      total.tx4_success += block.tx4_success;
      total.aborted += block.aborted;
      if (block.observed_at <= window_start || block.observed_at > window_end) {
        continue;
      }
      steady.transactions += block.transactions;
      steady.tx1 += block.tx1;
      steady.tx2_attempts += block.tx2_attempts;
      steady.tx2_success += block.tx2_success;
      steady.tx3_attempts += block.tx3_attempts;
      steady.tx3_success += block.tx3_success;
      steady.tx4_attempts += block.tx4_attempts;
      steady.tx4_success += block.tx4_success;
      steady.aborted += block.aborted;
    }
    auto inclusion = latency_stats(inclusion_latency_ms_);
    auto to_tx3 = latency_stats(completion.external_to_tx3_ms);
    auto to_tx4 = latency_stats(completion.external_to_tx4_ms);
    bool valid = blocks_skipped_ == 0 && parse_failures_ == 0 && steady.tx1 != 0;

    std::ostringstream json;
    json.setf(std::ios::fixed);
    json.precision(3);
    json << "{\n  \"version\":2,\n  \"workload\":\"devnet-jetton-spam-exact\",\n";
    json << "  \"valid_stage_counts\":" << (valid ? "true" : "false") << ",\n";
    json << "  \"pool_size\":" << options_.pool_size << ",\n  \"rng_seed\":" << options_.rng_seed
         << ",\n  \"init_mode\":" << options_.init_mode << ",\n  \"seed_hex\":\"" << seed_.to_hex()
         << "\",\n  \"minter\":\"0:" << minter_.to_hex() << "\",\n";
    json << "  \"wallet_code_hash\":\"" << wallet_code_hash_.to_hex() << "\",\n"
         << "  \"jetton_wallet_code_hash\":\"" << jetton_code_hash_.to_hex() << "\",\n";
    json << "  \"rate_target\":" << options_.rate << ",\n  \"duration_target_s\":" << scheduled_duration_
         << ",\n  \"warmup_s\":" << options_.warmup << ",\n  \"drain_s\":" << options_.drain
         << ",\n  \"measurement_window_s\":" << window
         << ",\n  \"measurement_start_unix_ms\":" << static_cast<td::int64>(unix_ms(window_start))
         << ",\n  \"measurement_end_unix_ms\":" << static_cast<td::int64>(unix_ms(window_end))
         << ",\n  \"max_inflight\":" << max_inflight_ << ",\n";
    json << "  \"rate_schedule\":[";
    for (std::size_t i = 0; i < rate_phases_.size(); ++i) {
      json << (i == 0 ? "" : ",") << "{\"phase\":" << i << ",\"rate\":" << rate_phases_[i].rate
           << ",\"duration_s\":" << rate_phases_[i].duration << '}';
    }
    json << "],\n";
    json << "  \"constructed\":" << signer_->constructed() << ",\n  \"offered\":" << offered_
         << ",\n  \"acknowledged\":" << acknowledged_ << ",\n  \"send_errors\":" << send_errors_
         << ",\n  \"inflight_at_end\":" << inflight_ << ",\n  \"tx1_total\":" << tx1_total_
         << ",\n  \"unmatched_externals\":" << pending_.size() << ",\n";
    json << "  \"totals\":{\"transactions\":" << total.transactions << ",\"tx1\":" << total.tx1
         << ",\"tx2_attempts\":" << total.tx2_attempts << ",\"tx2_success\":" << total.tx2_success
         << ",\"tx3_attempts\":" << total.tx3_attempts << ",\"tx3_success\":" << total.tx3_success
         << ",\"tx4_attempts\":" << total.tx4_attempts << ",\"tx4_success\":" << total.tx4_success
         << ",\"aborted\":" << total.aborted << "},\n";
    json << "  \"steady\":{\"transactions\":" << steady.transactions << ",\"tx1\":" << steady.tx1
         << ",\"tx2_attempts\":" << steady.tx2_attempts << ",\"tx2_success\":" << steady.tx2_success
         << ",\"tx3_attempts\":" << steady.tx3_attempts << ",\"tx3_success\":" << steady.tx3_success
         << ",\"tx4_attempts\":" << steady.tx4_attempts << ",\"tx4_success\":" << steady.tx4_success
         << ",\"aborted\":" << steady.aborted
         << ",\"raw_tps\":" << (window > 0 ? static_cast<double>(steady.transactions) / window : 0)
         << ",\"tx1_tps\":" << (window > 0 ? static_cast<double>(steady.tx1) / window : 0)
         << ",\"tx3_jtps\":" << (window > 0 ? static_cast<double>(steady.tx3_success) / window : 0) << "},\n";
    json << "  \"sampled\":{\"registered\":" << completion.registered
         << ",\"tx3_completed\":" << completion.tx3_completed << ",\"tx4_completed\":" << completion.tx4_completed
         << ",\"tracking_failures\":" << completion.tracking_failures
         << ",\"trace_rows\":" << completion.traces.size() << ',';
    append_latency_json(json, "external_to_tx1_ms", latency_stats(completion.external_to_tx1_ms));
    json << ',';
    append_latency_json(json, "tx1_to_tx2_ms", latency_stats(completion.tx1_to_tx2_ms));
    json << ',';
    append_latency_json(json, "tx2_to_tx3_ms", latency_stats(completion.tx2_to_tx3_ms));
    json << ',';
    append_latency_json(json, "tx3_to_tx4_ms", latency_stats(completion.tx3_to_tx4_ms));
    json << ',';
    append_latency_json(json, "external_to_tx3_ms", to_tx3);
    json << ',';
    append_latency_json(json, "external_to_tx4_ms", to_tx4);
    json << "},\n  ";
    append_latency_json(json, "inclusion_latency_ms", inclusion);
    json << ",\n  \"blocks_observed\":" << blocks_.size() << ",\n  \"blocks_skipped\":" << blocks_skipped_
         << ",\n  \"parse_failures\":" << parse_failures_ << ",\n  \"block_bytes_fetched\":" << bytes_fetched_
         << ",\n  \"initial_shards\":" << (topology_.empty() ? 0 : topology_.front().shards.size())
         << ",\n  \"final_shards\":" << (topology_.empty() ? 0 : topology_.back().shards.size())
         << ",\n  \"topology_events\":" << topology_.size()
         << ",\n  \"send_error_categories\":{";
    bool first = true;
    for (const auto& [category, count] : send_error_categories_) {
      json << (first ? "" : ",") << '"' << json_escape(category) << "\":" << count;
      first = false;
    }
    json << "}\n}\n";
    auto status = td::write_file(options_.out_path, json.str());
    if (status.is_error()) {
      LOG(ERROR) << "cannot write " << options_.out_path << ": " << status;
      g_exit_code.store(2);
    }

    std::ostringstream csv;
    csv << "steady,shard,seqno,root_hash,utime,observed_at_unix_ms,transactions,tx1,tx2_attempts,tx2_success,"
           "tx3_attempts,tx3_success,tx4_attempts,tx4_success,aborted,after_merge,before_split,after_split,"
           "want_split,want_merge\n";
    for (const auto& block : blocks_) {
      auto in_steady_window = block.observed_at > window_start && block.observed_at <= window_end;
      csv << (in_steady_window ? 1 : 0) << ',' << shard_hex(block.shard) << ',' << block.seqno << ','
          << block.root_hash.to_hex() << ',' << block.utime << ','
          << static_cast<td::int64>(unix_ms(block.observed_at)) << ',' << block.transactions << ',' << block.tx1 << ','
          << block.tx2_attempts << ',' << block.tx2_success << ',' << block.tx3_attempts << ',' << block.tx3_success
          << ',' << block.tx4_attempts << ',' << block.tx4_success << ',' << block.aborted << ',' << block.after_merge
          << ',' << block.before_split << ',' << block.after_split << ',' << block.want_split << ',' << block.want_merge
          << '\n';
    }
    status = td::write_file(options_.blocks_csv_path, csv.str());
    if (status.is_error()) {
      LOG(ERROR) << "cannot write " << options_.blocks_csv_path << ": " << status;
      g_exit_code.store(2);
    }

    if (!options_.traces_csv_path.empty()) {
      auto traces = completion.traces;
      std::sort(traces.begin(), traces.end(), [](const auto& lhs, const auto& rhs) { return lhs.ordinal < rhs.ordinal; });
      std::ostringstream trace_csv;
      trace_csv.setf(std::ios::fixed);
      trace_csv.precision(3);
      trace_csv << "steady_offer,ordinal,sender_id,recipient_id,wallet_seqno,external_hash,status,"
                   "offered_at_unix_ms,"
                   "tx1_shard,tx1_block,tx2_shard,tx2_block,tx3_shard,tx3_block,tx4_shard,tx4_block,"
                   "external_to_tx1_ms,tx1_to_tx2_ms,"
                   "tx2_to_tx3_ms,tx3_to_tx4_ms,external_to_tx3_ms,external_to_tx4_ms\n";
      auto append_block = [&](td::uint32 seqno) {
        if (seqno != 0) {
          trace_csv << seqno;
        }
      };
      auto append_latency = [&](double from, double to) {
        if (from != 0 && to != 0) {
          trace_csv << (to - from) * 1e3;
        }
      };
      for (const auto& trace : traces) {
        auto steady_offer = trace.sent_at > window_start && trace.sent_at <= window_end;
        trace_csv << (steady_offer ? 1 : 0) << ',' << trace.ordinal << ',' << trace.sender_id << ','
                  << trace.recipient_id << ',' << trace.wallet_seqno << ',' << trace.external_hash.to_hex() << ','
                  << trace.status << ',' << static_cast<td::int64>(unix_ms(trace.sent_at)) << ',';
        trace_csv << shard_hex(trace.tx1_shard) << ',';
        append_block(trace.tx1_block);
        trace_csv << ',';
        trace_csv << shard_hex(trace.tx2_shard) << ',';
        append_block(trace.tx2_block);
        trace_csv << ',';
        trace_csv << shard_hex(trace.tx3_shard) << ',';
        append_block(trace.tx3_block);
        trace_csv << ',';
        trace_csv << shard_hex(trace.tx4_shard) << ',';
        append_block(trace.tx4_block);
        trace_csv << ',';
        append_latency(trace.sent_at, trace.tx1_at);
        trace_csv << ',';
        append_latency(trace.tx1_at, trace.tx2_at);
        trace_csv << ',';
        append_latency(trace.tx2_at, trace.tx3_at);
        trace_csv << ',';
        append_latency(trace.tx3_at, trace.tx4_at);
        trace_csv << ',';
        append_latency(trace.sent_at, trace.tx3_at);
        trace_csv << ',';
        append_latency(trace.sent_at, trace.tx4_at);
        trace_csv << '\n';
      }
      status = td::write_file(options_.traces_csv_path, trace_csv.str());
      if (status.is_error()) {
        LOG(ERROR) << "cannot write " << options_.traces_csv_path << ": " << status;
        g_exit_code.store(2);
      }
    }

    if (!options_.timeline_csv_path.empty()) {
      std::ostringstream timeline;
      timeline << "unix_ms,phase,target_rate,constructed,offered,acknowledged,send_errors,tx1,tx3\n";
      for (const auto& item : timeline_) {
        timeline << static_cast<td::int64>(unix_ms(item.time)) << ',' << item.phase << ',' << item.target_rate << ','
                 << item.constructed << ',' << item.offered << ',' << item.acknowledged << ',' << item.send_errors << ','
                 << item.tx1 << ',' << item.tx3 << '\n';
      }
      status = td::write_file(options_.timeline_csv_path, timeline.str());
      if (status.is_error()) {
        LOG(ERROR) << "cannot write " << options_.timeline_csv_path << ": " << status;
      }
    }

    if (!options_.topology_csv_path.empty()) {
      std::ostringstream topology;
      topology << "observed_at_unix_ms,shard_count,shard,top_seqno\n";
      for (const auto& event : topology_) {
        for (const auto& shard : event.shards) {
          topology << static_cast<td::int64>(unix_ms(event.observed_at)) << ',' << event.shards.size() << ','
                   << shard_hex(shard.shard) << ',' << shard.seqno << '\n';
        }
      }
      status = td::write_file(options_.topology_csv_path, topology.str());
      if (status.is_error()) {
        LOG(ERROR) << "cannot write " << options_.topology_csv_path << ": " << status;
        g_exit_code.store(2);
      }
    }

    std::printf("=== exact jetton simulator ===\n");
    std::printf("  offered / acknowledged:  %llu / %llu (%llu errors)\n", (unsigned long long)offered_,
                (unsigned long long)acknowledged_, (unsigned long long)send_errors_);
    std::printf("  steady TX1 / TX2 / TX3 / TX4: %llu / %llu / %llu / %llu\n", (unsigned long long)steady.tx1,
                (unsigned long long)steady.tx2_success, (unsigned long long)steady.tx3_success,
                (unsigned long long)steady.tx4_success);
    std::printf("  steady jTPS / raw TPS:    %.1f / %.1f\n",
                window > 0 ? static_cast<double>(steady.tx3_success) / window : 0,
                window > 0 ? static_cast<double>(steady.transactions) / window : 0);
    std::printf("  stage counts valid:       %s (skipped %llu, parse failures %llu)\n", valid ? "yes" : "no",
                (unsigned long long)blocks_skipped_, (unsigned long long)parse_failures_);
    std::printf("  results:                  %s\n", options_.out_path.c_str());
    std::fflush(stdout);
  }

  SpamOptions options_;
  std::vector<RatePhase> rate_phases_;
  td::Bits256 seed_;
  td::Bits256 minter_;
  std::vector<PoolAccount> accounts_;
  td::Bits256 wallet_code_hash_;
  td::Bits256 jetton_code_hash_;
  ton::adnl::AdnlNodeIdFull server_id_;
  td::IPAddress server_address_;
  std::vector<td::actor::ActorOwn<liteclient::ExtClient>> clients_;
  td::actor::ActorOwn<BlockParser> parser_;
  std::unique_ptr<SignerPool> signer_;

  double started_at_{0};
  double unix_offset_{0};
  double last_send_tick_{0};
  double first_offer_at_{0};
  double last_offer_at_{0};
  double tokens_{0};
  double scheduled_duration_{0};
  double max_rate_{0};
  std::size_t current_phase_index_{std::numeric_limits<std::size_t>::max()};
  td::uint64 target_total_{0};
  td::uint64 max_inflight_{0};
  td::uint64 offered_{0};
  td::uint64 inflight_{0};
  td::uint64 acknowledged_{0};
  td::uint64 send_errors_{0};
  std::size_t sender_round_robin_{0};
  std::map<std::string, td::uint64> send_error_categories_;
  std::unordered_map<td::Bits256, SentRecord, Bits256Hash> pending_;
  std::vector<double> inclusion_latency_ms_;
  bool sending_done_{false};

  bool watcher_ready_{false};
  bool watcher_busy_{false};
  td::Timestamp watcher_retry_{td::Timestamp::now()};
  double current_observed_at_{0};
  int block_fetch_failures_{0};
  int transient_block_retries_{0};
  td::uint64 blocks_skipped_{0};
  td::uint64 parse_failures_{0};
  td::uint64 bytes_fetched_{0};
  td::uint64 tx1_total_{0};
  td::uint64 tx3_total_{0};
  std::vector<BlockRecord> blocks_;

  bool draining_{false};
  bool finishing_{false};
  double drain_until_{0};
  double next_progress_{0};
  double next_timeline_{0};
  std::vector<TimelineRecord> timeline_;

  struct ShardCursor {
    td::uint32 scheduled_through{0};
    bool active{false};
  };
  std::map<ton::ShardId, ShardCursor> shard_cursors_;
  std::set<ton::ShardId> active_shards_;
  std::deque<ton::BlockId> pending_blocks_;
  std::optional<ton::BlockId> current_simple_block_;
  std::vector<TopologyRecord> topology_;
};

}  // namespace

int run_spam(const SpamOptions& options) {
  auto input = [&]() -> td::Result<std::tuple<td::Bits256, td::Bits256, Workload>> {
    TRY_RESULT(seed, parse_bits256(options.seed_hex, "--seed-hex"));
    TRY_RESULT(minter, parse_bits256(options.minter_hex, "--minter-hex"));
    TRY_RESULT(workload, Workload::create(seed, minter));
    return std::make_tuple(seed, minter, std::move(workload));
  }();
  if (input.is_error()) {
    LOG(ERROR) << input.error();
    return 2;
  }
  auto [seed, minter, workload] = input.move_as_ok();
  auto phases_result = parse_rate_phases(options);
  if (phases_result.is_error()) {
    LOG(ERROR) << phases_result.error();
    return 2;
  }
  auto rate_phases = phases_result.move_as_ok();
  if (options.pool_size == 0) {
    LOG(ERROR) << "--pool-size must be positive";
    return 2;
  }
  std::vector<PoolAccount> accounts;
  accounts.reserve(static_cast<std::size_t>(options.pool_size) + 1);
  for (td::uint32 id = 0; id <= options.pool_size; ++id) {
    accounts.push_back(workload.account(id));
  }
  if (options.liteserver_addr.empty() || options.liteserver_pubkey_b64.empty()) {
    LOG(ERROR) << "--liteserver and --liteserver-pubkey-b64 are required";
    return 2;
  }
  td::IPAddress server_address;
  auto status = server_address.init_host_port(options.liteserver_addr);
  if (status.is_error()) {
    LOG(ERROR) << "invalid --liteserver: " << status;
    return 2;
  }
  auto decoded_key = td::base64_decode(options.liteserver_pubkey_b64);
  if (decoded_key.is_error() || decoded_key.ok().size() != 32) {
    LOG(ERROR) << "--liteserver-pubkey-b64 must decode to 32 bytes";
    return 2;
  }
  td::Bits256 key;
  key.as_slice().copy_from(decoded_key.ok());
  ton::adnl::AdnlNodeIdFull server_id{ton::PublicKey{ton::pubkeys::Ed25519{key}}};
  auto wallet_code_hash = td::Bits256{workload.contracts().wallet_spam_code->get_hash().bits()};
  auto jetton_code_hash = td::Bits256{workload.contracts().jetton_wallet_code->get_hash().bits()};

  g_interrupts.store(0);
  g_exit_code.store(0);
  td::set_signal_handler(td::SignalType::Quit, [](int) {
    if (g_interrupts.fetch_add(1, std::memory_order_relaxed) >= 2) {
      std::_Exit(130);
    }
  }).ensure();
  td::actor::Scheduler scheduler({4});
  scheduler.run_in_context([&] {
    td::actor::create_actor<SpamRunner>("jetton-spam-runner", options, seed, minter, std::move(accounts),
                                        wallet_code_hash, jetton_code_hash, std::move(rate_phases), std::move(server_id),
                                        server_address)
        .release();
  });
  scheduler.run();
  return g_exit_code.load();
}

}  // namespace jetton_sim
