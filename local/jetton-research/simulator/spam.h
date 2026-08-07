#pragma once

#include <string>

#include "td/utils/int_types.h"

namespace jetton_sim {

struct SpamOptions {
  std::string seed_hex;
  std::string minter_hex;
  td::uint32 pool_size{10'000};
  td::uint32 init_mode{0};
  std::string liteserver_addr;
  std::string liteserver_pubkey_b64;
  double rate{1'000.0};
  std::string rate_schedule;
  double duration{60.0};
  double warmup{5.0};
  double drain{10.0};
  double track_sample{0.01};
  td::uint64 rng_seed{1};
  td::uint64 max_inflight{0};
  td::uint64 presign{0};
  int signer_threads{0};
  int connections{1};
  std::string out_path{"results.json"};
  std::string blocks_csv_path{"blocks.csv"};
  std::string timeline_csv_path{"timeline.csv"};
  std::string traces_csv_path{"traces.csv"};
  std::string topology_csv_path{"topology.csv"};
};

int run_spam(const SpamOptions& options);

}  // namespace jetton_sim
