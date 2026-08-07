#pragma once

#include <string>
#include <vector>

#include "crypto/Ed25519.h"
#include "td/utils/Status.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/DataCell.h"

namespace jetton_sim {

using td::Ref;
using Uint128 = unsigned __int128;

inline constexpr td::Slice kDefaultSeedHex = "0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210";
inline constexpr td::Slice kDefaultMinterHex = "b2ddc5ae517ade7439975138a90e8df526e75ad98d748df2bd0d84b48013c08c";

struct ContractSet {
  Ref<vm::DataCell> wallet_spam_code;
  Ref<vm::DataCell> jetton_wallet_code;
};

struct PoolAccount {
  td::Bits256 owner;
  td::Bits256 jetton_wallet;
};

struct PreparedPool {
  td::Bits256 seed;
  td::Bits256 public_key;
  td::Bits256 minter;
  td::uint32 active_count{0};
  Uint128 funding{0};
  PoolAccount first;
  PoolAccount sentinel;
  Ref<vm::Cell> kickoff_external;
};

struct TransferMessage {
  td::uint32 sender_id{0};
  td::uint32 recipient_id{0};
  td::uint32 seqno{0};
  Ref<vm::Cell> body;
  Ref<vm::Cell> external;
};

td::Result<ContractSet> load_contracts();
td::Result<td::Bits256> parse_bits256(td::Slice value, td::Slice label);
std::string uint128_to_string(Uint128 value);

class Workload {
 public:
  static td::Result<Workload> create(td::Bits256 seed, td::Bits256 minter);

  const ContractSet& contracts() const {
    return contracts_;
  }
  const td::Bits256& seed() const {
    return seed_;
  }
  const td::Bits256& public_key() const {
    return public_key_;
  }
  const td::Bits256& minter() const {
    return minter_;
  }

  PoolAccount account(td::uint32 id) const;
  td::Result<PreparedPool> prepare_pool(td::uint32 active_count) const;
  td::Result<TransferMessage> build_transfer(td::uint32 sender_id, td::uint32 recipient_id, td::uint32 seqno,
                                             td::uint32 valid_until, td::uint32 comment,
                                             td::uint32 init_mode = 0) const;
  td::Result<TransferMessage> build_transfer(td::uint32 sender_id, const PoolAccount& sender, td::uint32 recipient_id,
                                             const PoolAccount& recipient, td::uint32 seqno, td::uint32 valid_until,
                                             td::uint32 comment, td::uint32 init_mode = 0) const;

 private:
  Workload(ContractSet contracts, td::Bits256 seed, td::Bits256 public_key, td::Bits256 minter,
           td::Ed25519::PrivateKey private_key)
      : contracts_(std::move(contracts))
      , seed_(seed)
      , public_key_(public_key)
      , minter_(minter)
      , private_key_(std::move(private_key)) {
  }

  Ref<vm::DataCell> wallet_spam_data(td::uint32 id) const;
  Ref<vm::DataCell> prepaid_wallet_data(const td::Bits256& owner) const;
  Ref<vm::DataCell> wallet_spam_state_init(td::uint32 id) const;
  Ref<vm::DataCell> prepaid_wallet_state_init(const td::Bits256& owner) const;
  td::Result<Ref<vm::DataCell>> sign_wallet_body(const vm::CellBuilder& content) const;

  ContractSet contracts_;
  td::Bits256 seed_;
  td::Bits256 public_key_;
  td::Bits256 minter_;
  td::Ed25519::PrivateKey private_key_;
};

td::Status run_self_test();
td::Status write_prepared_pool(const PreparedPool& pool, const Workload& workload, td::CSlice out_dir);

}  // namespace jetton_sim
