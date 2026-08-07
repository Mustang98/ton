#include <limits>

#include "td/utils/OptionParser.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/signals.h"
#include "vm/vm.h"

#include "spam.h"
#include "workload.h"

namespace jetton_sim {
namespace {

struct Options {
  std::string seed_hex = kDefaultSeedHex.str();
  std::string minter_hex = kDefaultMinterHex.str();
  td::uint32 pool_size{10'000};
  std::string out_dir = "jetton-simulator-pool";
};

td::Result<Workload> make_workload(const Options& options) {
  TRY_RESULT(seed, parse_bits256(options.seed_hex, "--seed-hex"));
  TRY_RESULT(minter, parse_bits256(options.minter_hex, "--minter-hex"));
  return Workload::create(seed, minter);
}

int prepare(const Options& options) {
  auto result = [&]() -> td::Status {
    TRY_RESULT(workload, make_workload(options));
    TRY_RESULT(pool, workload.prepare_pool(options.pool_size));
    TRY_STATUS(write_prepared_pool(pool, workload, options.out_dir));
    LOG(INFO) << "prepared exact jetton-spam pool: active=" << options.pool_size
              << " deployed=" << static_cast<td::uint64>(options.pool_size) + 1
              << " wallet0=0:" << pool.first.owner.to_hex()
              << " sentinel_jetton=0:" << pool.sentinel.jetton_wallet.to_hex();
    return td::Status::OK();
  }();
  if (result.is_error()) {
    LOG(ERROR) << "prepare failed: " << result;
    return 1;
  }
  return 0;
}

}  // namespace
}  // namespace jetton_sim

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();
  vm::init_vm().ensure();

  jetton_sim::Options options;
  jetton_sim::SpamOptions spam_options;
  spam_options.seed_hex = options.seed_hex;
  spam_options.minter_hex = options.minter_hex;
  td::OptionParser parser;
  parser.set_description("jetton-simulator <self-test|prepare|spam> [options]");
  parser.add_option('\0', "seed-hex", "deterministic shared pool Ed25519 seed", [&](td::Slice value) {
    options.seed_hex = value.str();
    spam_options.seed_hex = value.str();
  });
  parser.add_option('\0', "minter-hex", "raw 32-byte workchain-0 minter account hash", [&](td::Slice value) {
    options.minter_hex = value.str();
    spam_options.minter_hex = value.str();
  });
  parser.add_checked_option('\0', "pool-size", "active WalletSpam IDs; deployment creates one extra sentinel",
                            [&](td::Slice value) {
                              TRY_RESULT_ASSIGN(options.pool_size, td::to_integer_safe<td::uint32>(value));
                              spam_options.pool_size = options.pool_size;
                              return options.pool_size > 0 && options.pool_size < std::numeric_limits<td::uint32>::max()
                                         ? td::Status::OK()
                                         : td::Status::Error("--pool-size must be in [1, 2^32-2]");
                            });
  parser.add_checked_option(
      '\0', "init-mode", "recipient JettonWallet init mode: 0 full StateInit, 1 bare address", [&](td::Slice value) {
        TRY_RESULT_ASSIGN(spam_options.init_mode, td::to_integer_safe<td::uint32>(value));
        return spam_options.init_mode <= 1 ? td::Status::OK() : td::Status::Error("--init-mode must be 0 or 1");
      });
  parser.add_option('\0', "out-dir", "prepare output directory",
                    [&](td::Slice value) { options.out_dir = value.str(); });
  parser.add_option('\0', "liteserver", "liteserver <ip>:<port>",
                    [&](td::Slice value) { spam_options.liteserver_addr = value.str(); });
  parser.add_option('\0', "liteserver-pubkey-b64", "liteserver Ed25519 public key",
                    [&](td::Slice value) { spam_options.liteserver_pubkey_b64 = value.str(); });
  parser.add_checked_option('\0', "rate", "target external-message fires per second", [&](td::Slice value) {
    spam_options.rate = td::to_double(value);
    return spam_options.rate > 0 ? td::Status::OK() : td::Status::Error("--rate must be positive");
  });
  parser.add_option('\0', "rate-schedule", "comma-separated rate:seconds phases (overrides --rate/--duration)",
                    [&](td::Slice value) { spam_options.rate_schedule = value.str(); });
  parser.add_checked_option('\0', "duration", "offer duration in seconds", [&](td::Slice value) {
    spam_options.duration = td::to_double(value);
    return spam_options.duration > 0 ? td::Status::OK() : td::Status::Error("--duration must be positive");
  });
  parser.add_checked_option('\0', "warmup", "seconds excluded from measurement start", [&](td::Slice value) {
    spam_options.warmup = td::to_double(value);
    return spam_options.warmup >= 0 ? td::Status::OK() : td::Status::Error("--warmup must be non-negative");
  });
  parser.add_checked_option('\0', "drain", "block-watching time after offers stop", [&](td::Slice value) {
    spam_options.drain = td::to_double(value);
    return spam_options.drain >= 0 ? td::Status::OK() : td::Status::Error("--drain must be non-negative");
  });
  parser.add_checked_option('\0', "track-sample", "fraction of transfers traced through TX4", [&](td::Slice value) {
    spam_options.track_sample = td::to_double(value);
    return spam_options.track_sample >= 0 && spam_options.track_sample <= 1
               ? td::Status::OK()
               : td::Status::Error("--track-sample must be in [0,1]");
  });
  parser.add_checked_option('\0', "rng-seed", "sender/recipient/comment RNG seed", [&](td::Slice value) {
    TRY_RESULT_ASSIGN(spam_options.rng_seed, td::to_integer_safe<td::uint64>(value));
    return td::Status::OK();
  });
  parser.add_checked_option('\0', "max-inflight", "send acknowledgements in flight; 0 uses 2*rate+10",
                            [&](td::Slice value) {
                              TRY_RESULT_ASSIGN(spam_options.max_inflight, td::to_integer_safe<td::uint64>(value));
                              return td::Status::OK();
                            });
  parser.add_checked_option('\0', "presign", "signed-message buffer; 0 uses two seconds of target load",
                            [&](td::Slice value) {
                              TRY_RESULT_ASSIGN(spam_options.presign, td::to_integer_safe<td::uint64>(value));
                              return td::Status::OK();
                            });
  parser.add_checked_option('\0', "signer-threads", "signing threads; 0 chooses automatically", [&](td::Slice value) {
    TRY_RESULT_ASSIGN(spam_options.signer_threads, td::to_integer_safe<int>(value));
    return spam_options.signer_threads >= 0 ? td::Status::OK()
                                            : td::Status::Error("--signer-threads must be non-negative");
  });
  parser.add_checked_option('\0', "connections", "parallel send-only ADNL connections", [&](td::Slice value) {
    TRY_RESULT_ASSIGN(spam_options.connections, td::to_integer_safe<int>(value));
    return spam_options.connections > 0 ? td::Status::OK() : td::Status::Error("--connections must be positive");
  });
  parser.add_option('\0', "out", "spam result JSON path",
                    [&](td::Slice value) { spam_options.out_path = value.str(); });
  parser.add_option('\0', "blocks-csv", "per-block stage counter CSV path",
                    [&](td::Slice value) { spam_options.blocks_csv_path = value.str(); });
  parser.add_option('\0', "timeline-csv", "per-second generator/completion CSV path",
                    [&](td::Slice value) { spam_options.timeline_csv_path = value.str(); });
  parser.add_option('\0', "traces-csv", "sampled per-request TX1-TX4 trace CSV path",
                    [&](td::Slice value) { spam_options.traces_csv_path = value.str(); });
  parser.add_option('\0', "topology-csv", "workchain shard-topology event CSV path",
                    [&](td::Slice value) { spam_options.topology_csv_path = value.str(); });
  parser.add_option('v', "verbosity", "verbosity level",
                    [&](td::Slice value) { SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + td::to_integer<int>(value)); });

  auto parsed = parser.run(argc, argv);
  if (parsed.is_error()) {
    LOG(ERROR) << parsed.error();
    LOG(ERROR) << parser;
    return 2;
  }
  auto args = parsed.move_as_ok();
  if (args.size() != 1) {
    LOG(ERROR) << parser;
    return 2;
  }
  std::string command = args[0];
  if (command == "self-test") {
    auto status = jetton_sim::run_self_test();
    if (status.is_error()) {
      LOG(ERROR) << "self-test failed: " << status;
      return 1;
    }
    LOG(INFO) << "self-test: exact WalletSpam address, transfer body, signature, TL-B, and BoC checks passed";
    return 0;
  }
  if (command == "prepare") {
    return jetton_sim::prepare(options);
  }
  if (command == "spam") {
    return jetton_sim::run_spam(spam_options);
  }
  LOG(ERROR) << "unknown command: " << command;
  LOG(ERROR) << parser;
  return 2;
}
