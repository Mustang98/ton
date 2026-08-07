#include <algorithm>
#include <limits>
#include <sstream>

#include "block/block-auto.h"
#include "td/utils/filesystem.h"
#include "td/utils/misc.h"
#include "td/utils/port/path.h"
#include "vm/boc.h"

#include "contracts.h"
#include "workload.h"

namespace jetton_sim {
namespace {

constexpr Uint128 kWalletValue = 100'000'000'000ULL;
constexpr Uint128 kJettonDeployValue = 10'000'000ULL;
constexpr Uint128 kFeeSlack = 100'000'000ULL;
constexpr Uint128 kTransferValue = 50'000'000ULL;
constexpr Uint128 kPrepaidJettonBalance = 1'000'000ULL;
constexpr td::uint32 kDeployChunk = 200;
constexpr td::uint32 kOpTransfer = 0x0f8a7ea5;
constexpr td::uint32 kOpTopUpGrams = 0xd372158c;
constexpr td::uint32 kOpReproduce = 0xba5e5d50;

int uint_bytes(Uint128 value) {
  int result = 0;
  while (value != 0) {
    value >>= 8;
    ++result;
  }
  return result;
}

void store_uint_be(vm::CellBuilder& cb, Uint128 value, int bytes) {
  if (bytes > 8) {
    cb.store_long(static_cast<long long>(static_cast<td::uint64>(value >> 64)), (bytes - 8) * 8);
    bytes = 8;
    value &= ~static_cast<Uint128>(0) >> 64;
  }
  cb.store_long(static_cast<long long>(static_cast<td::uint64>(value)), bytes * 8);
}

void store_grams(vm::CellBuilder& cb, Uint128 value) {
  int bytes = uint_bytes(value);
  CHECK(bytes < 16);
  cb.store_long(bytes, 4);
  store_uint_be(cb, value, bytes);
}

void store_addr_none(vm::CellBuilder& cb) {
  cb.store_long(0, 2);
}

void store_addr_std(vm::CellBuilder& cb, const td::Bits256& address) {
  cb.store_long(0b100, 3);  // addr_std$10 anycast:nothing$0
  cb.store_long(0, 8);
  cb.store_bits(address.bits(), 256);
}

void store_currency(vm::CellBuilder& cb, Uint128 grams) {
  store_grams(cb, grams);
  cb.store_long(0, 1);  // empty extra-currency dictionary
}

Ref<vm::DataCell> build_state_init(Ref<vm::Cell> code, Ref<vm::Cell> data) {
  vm::CellBuilder cb;
  cb.store_long(0b00110, 5);  // no split_depth/special, code+data, no library
  cb.store_ref(std::move(code));
  cb.store_ref(std::move(data));
  return cb.finalize_novm();
}

bool can_inline(const vm::CellBuilder& cb, const vm::CellSlice& value, int discriminator_bits, int leave_bits = 0,
                int leave_refs = 0) {
  return static_cast<int>(cb.remaining_bits()) >= discriminator_bits + static_cast<int>(value.size()) + leave_bits &&
         static_cast<int>(cb.remaining_refs()) >= static_cast<int>(value.size_refs()) + leave_refs;
}

void store_either_cell(vm::CellBuilder& cb, Ref<vm::Cell> value, int leave_bits = 0, int leave_refs = 0) {
  auto cs = vm::load_cell_slice(value);
  if (can_inline(cb, cs, 1, leave_bits, leave_refs)) {
    cb.store_long(0, 1);
    cb.append_cellslice(cs);
  } else {
    cb.store_long(1, 1);
    cb.store_ref(std::move(value));
  }
}

void store_maybe_state_init(vm::CellBuilder& cb, Ref<vm::Cell> state_init) {
  if (state_init.is_null()) {
    cb.store_long(0, 1);
    return;
  }
  cb.store_long(1, 1);
  store_either_cell(cb, std::move(state_init), 1, 1);
}

Ref<vm::DataCell> build_internal_message(const td::Bits256& destination, Uint128 value, bool bounce, Ref<vm::Cell> body,
                                         Ref<vm::Cell> state_init = {}) {
  vm::CellBuilder cb;
  cb.store_long(0, 1);  // int_msg_info$0
  cb.store_long(1, 1);  // ihr_disabled
  cb.store_long(bounce, 1);
  cb.store_long(0, 1);  // bounced
  store_addr_none(cb);  // source is filled by the action phase
  store_addr_std(cb, destination);
  store_currency(cb, value);
  store_grams(cb, 0);
  store_grams(cb, 0);
  cb.store_long(0, 64);
  cb.store_long(0, 32);
  store_maybe_state_init(cb, std::move(state_init));
  store_either_cell(cb, std::move(body));
  return cb.finalize_novm();
}

Ref<vm::DataCell> build_external_message(const td::Bits256& destination, Ref<vm::Cell> body,
                                         Ref<vm::Cell> state_init = {}) {
  vm::CellBuilder cb;
  cb.store_long(0b10, 2);  // ext_in_msg_info$10
  store_addr_none(cb);
  store_addr_std(cb, destination);
  store_grams(cb, 0);
  store_maybe_state_init(cb, std::move(state_init));
  store_either_cell(cb, std::move(body));
  return cb.finalize_novm();
}

td::Result<Ref<vm::DataCell>> load_code(td::Slice hex) {
  TRY_RESULT(bytes, td::hex_decode(hex));
  TRY_RESULT(root, vm::std_boc_deserialize(bytes));
  TRY_RESULT(loaded, root->load_cell());
  return std::move(loaded.data_cell);
}

Ref<vm::DataCell> make_wallet_spam_data(td::uint32 id, const td::Bits256& public_key, const td::Bits256& minter,
                                        Ref<vm::Cell> jetton_wallet_code) {
  vm::CellBuilder cb;
  cb.store_long(0, 32);  // StateInit always uses initial seqno zero.
  cb.store_long(id, 32);
  cb.store_bits(public_key.bits(), 256);
  store_addr_std(cb, minter);
  cb.store_ref(std::move(jetton_wallet_code));
  return cb.finalize_novm();
}

Ref<vm::DataCell> make_prepaid_wallet_data(const td::Bits256& owner, const td::Bits256& minter) {
  vm::CellBuilder cb;
  store_grams(cb, kPrepaidJettonBalance);
  store_addr_std(cb, owner);
  store_addr_std(cb, minter);
  return cb.finalize_novm();
}

std::string raw_address(const td::Bits256& address) {
  return "0:" + address.to_hex();
}

}  // namespace

td::Result<ContractSet> load_contracts() {
  ContractSet result;
  TRY_RESULT_ASSIGN(result.wallet_spam_code, load_code(contracts::kWalletSpamCodeHex));
  TRY_RESULT_ASSIGN(result.jetton_wallet_code, load_code(contracts::kJettonWalletCodeHex));
  return result;
}

td::Result<td::Bits256> parse_bits256(td::Slice value, td::Slice label) {
  TRY_RESULT(bytes, td::hex_decode(value));
  if (bytes.size() != 32) {
    return td::Status::Error(PSLICE() << label << " must contain exactly 32 bytes");
  }
  td::Bits256 result;
  result.as_slice().copy_from(bytes);
  return result;
}

std::string uint128_to_string(Uint128 value) {
  if (value == 0) {
    return "0";
  }
  std::string result;
  while (value != 0) {
    result.push_back(static_cast<char>('0' + value % 10));
    value /= 10;
  }
  std::reverse(result.begin(), result.end());
  return result;
}

td::Result<Workload> Workload::create(td::Bits256 seed, td::Bits256 minter) {
  TRY_RESULT(contracts, load_contracts());
  td::Ed25519::PrivateKey private_key{td::SecureString(seed.as_slice())};
  TRY_RESULT(public_key, private_key.get_public_key());
  auto octets = public_key.as_octet_string();
  if (octets.size() != 32) {
    return td::Status::Error("unexpected Ed25519 public key size");
  }
  td::Bits256 public_key_bits;
  public_key_bits.as_slice().copy_from(octets);
  return Workload{std::move(contracts), seed, public_key_bits, minter, std::move(private_key)};
}

Ref<vm::DataCell> Workload::wallet_spam_data(td::uint32 id) const {
  return make_wallet_spam_data(id, public_key_, minter_, contracts_.jetton_wallet_code);
}

Ref<vm::DataCell> Workload::prepaid_wallet_data(const td::Bits256& owner) const {
  return make_prepaid_wallet_data(owner, minter_);
}

Ref<vm::DataCell> Workload::wallet_spam_state_init(td::uint32 id) const {
  return build_state_init(contracts_.wallet_spam_code, wallet_spam_data(id));
}

Ref<vm::DataCell> Workload::prepaid_wallet_state_init(const td::Bits256& owner) const {
  return build_state_init(contracts_.jetton_wallet_code, prepaid_wallet_data(owner));
}

PoolAccount Workload::account(td::uint32 id) const {
  PoolAccount result;
  result.owner = td::Bits256{wallet_spam_state_init(id)->get_hash().bits()};
  result.jetton_wallet = td::Bits256{prepaid_wallet_state_init(result.owner)->get_hash().bits()};
  return result;
}

td::Result<Ref<vm::DataCell>> Workload::sign_wallet_body(const vm::CellBuilder& content) const {
  auto unsigned_body = content.finalize_copy();
  TRY_RESULT(signature, private_key_.sign(unsigned_body->get_hash().as_slice()));
  if (signature.size() != 64) {
    return td::Status::Error("unexpected Ed25519 signature size");
  }
  vm::CellBuilder result;
  result.store_bytes(signature.as_slice());
  result.append_builder(content);
  return result.finalize_novm();
}

td::Result<PreparedPool> Workload::prepare_pool(td::uint32 active_count) const {
  if (active_count == 0 || active_count == std::numeric_limits<td::uint32>::max()) {
    return td::Status::Error("active pool size must be in [1, 2^32-2]");
  }

  PreparedPool result;
  result.seed = seed_;
  result.public_key = public_key_;
  result.minter = minter_;
  result.active_count = active_count;
  result.funding = static_cast<Uint128>(active_count + 1) * (kWalletValue + kFeeSlack);
  result.first = account(0);
  result.sentinel = account(active_count);

  vm::CellBuilder top_up_body;
  top_up_body.store_long(kOpTopUpGrams, 32);
  auto deploy_jetton =
      build_internal_message(result.first.jetton_wallet, kJettonDeployValue, false, top_up_body.finalize_novm(),
                             prepaid_wallet_state_init(result.first.owner));

  auto wallet1 = account(1);
  vm::CellBuilder reproduce_body;
  reproduce_body.store_long(kOpReproduce, 32);
  reproduce_body.store_long(active_count, 32);
  reproduce_body.store_long(0, 32);
  reproduce_body.store_long(kDeployChunk, 16);
  reproduce_body.store_long(1, 1);
  store_grams(reproduce_body, kWalletValue);
  store_grams(reproduce_body, kJettonDeployValue);
  auto reproduce_value = static_cast<Uint128>(active_count) * (kWalletValue + kFeeSlack);
  auto reproduce = build_internal_message(wallet1.owner, reproduce_value, true, reproduce_body.finalize_novm(),
                                          wallet_spam_state_init(1));

  vm::CellBuilder content;
  content.store_long(0, 32);
  content.store_long(0xffffffffU, 32);
  content.store_long(0, 32);
  content.store_long(1, 8);
  content.store_ref(std::move(deploy_jetton));
  content.store_long(0, 8);
  content.store_ref(std::move(reproduce));
  TRY_RESULT(kickoff_body, sign_wallet_body(content));
  result.kickoff_external =
      build_external_message(result.first.owner, std::move(kickoff_body), wallet_spam_state_init(0));
  return result;
}

td::Result<TransferMessage> Workload::build_transfer(td::uint32 sender_id, td::uint32 recipient_id, td::uint32 seqno,
                                                     td::uint32 valid_until, td::uint32 comment,
                                                     td::uint32 init_mode) const {
  auto sender = account(sender_id);
  auto recipient = account(recipient_id);
  return build_transfer(sender_id, sender, recipient_id, recipient, seqno, valid_until, comment, init_mode);
}

td::Result<TransferMessage> Workload::build_transfer(td::uint32 sender_id, const PoolAccount& sender,
                                                     td::uint32 recipient_id, const PoolAccount& recipient,
                                                     td::uint32 seqno, td::uint32 valid_until, td::uint32 comment,
                                                     td::uint32 init_mode) const {
  Ref<vm::Cell> custom_payload;
  if (init_mode != 0) {
    vm::CellBuilder custom;
    custom.store_long(init_mode & 3, 2);
    custom_payload = custom.finalize_novm();
  }

  auto comment_string = td::to_string(comment);
  vm::CellBuilder forward_payload;
  forward_payload.store_long(0, 32);
  forward_payload.store_bytes(comment_string);

  vm::CellBuilder transfer_body;
  transfer_body.store_long(kOpTransfer, 32);
  transfer_body.store_long(0, 64);
  store_grams(transfer_body, 1);
  store_addr_std(transfer_body, recipient.owner);
  store_addr_std(transfer_body, sender.owner);
  if (custom_payload.is_null()) {
    transfer_body.store_long(0, 1);
  } else {
    transfer_body.store_long(1, 1);
    transfer_body.store_ref(std::move(custom_payload));
  }
  store_grams(transfer_body, 0);
  transfer_body.store_long(1, 1);
  transfer_body.store_ref(forward_payload.finalize_novm());

  auto internal = build_internal_message(sender.jetton_wallet, kTransferValue, true, transfer_body.finalize_novm());
  vm::CellBuilder content;
  content.store_long(sender_id, 32);
  content.store_long(valid_until, 32);
  content.store_long(seqno, 32);
  content.store_long(3, 8);
  content.store_ref(std::move(internal));
  TRY_RESULT(body, sign_wallet_body(content));

  TransferMessage result;
  result.sender_id = sender_id;
  result.recipient_id = recipient_id;
  result.seqno = seqno;
  result.body = body;
  result.external = build_external_message(sender.owner, std::move(body));
  return result;
}

td::Status run_self_test() {
  TRY_RESULT(contracts, load_contracts());
  TRY_RESULT(vector_pubkey, parse_bits256(kDefaultSeedHex, "vector public key"));
  td::Bits256 vector_minter{};
  vector_minter.as_slice()[30] = static_cast<char>(0x0a);
  vector_minter.as_slice()[31] = static_cast<char>(0xbc);
  auto vector_data = make_wallet_spam_data(0, vector_pubkey, vector_minter, contracts.jetton_wallet_code);
  auto vector_state = build_state_init(contracts.wallet_spam_code, vector_data);
  td::Bits256 expected_address;
  CHECK(expected_address.from_hex("5be5354a3ee0fd04a512251b4ee548163745d68a4daf813f7fb52feb495fbb53") == 256);
  if (td::Bits256{vector_state->get_hash().bits()} != expected_address) {
    return td::Status::Error("WalletSpam address differs from upstream Go/Tolk golden vector");
  }

  TRY_RESULT(seed, parse_bits256(kDefaultSeedHex, "test seed"));
  TRY_RESULT(workload, Workload::create(seed, vector_minter));
  TRY_RESULT(message, workload.build_transfer(0, 1, 0, 0xffffffffU, 42, 0));
  TRY_RESULT(default_message, workload.build_transfer(0, 1, 0, 0xffffffffU, 42));
  if (default_message.body->get_hash() != message.body->get_hash()) {
    return td::Status::Error("default transfer init mode is not mode 0");
  }
  TRY_RESULT(bare_message, workload.build_transfer(0, 1, 0, 0xffffffffU, 42, 1));
  if (bare_message.body->get_hash() == message.body->get_hash()) {
    return td::Status::Error("mode 0 and mode 1 transfer bodies unexpectedly match");
  }
  td::Bits256 expected_body;
  CHECK(expected_body.from_hex("4e9ec14dbc0e7a133e83ff7d1213a252b7904fa1beb4bea8ece1ec863e183e8f") == 256);
  if (td::Bits256{message.body->get_hash().bits()} != expected_body) {
    return td::Status::Error(PSLICE() << "signed transfer body differs from upstream golden vector: got "
                                      << message.body->get_hash().to_hex());
  }
  if (!block::gen::t_Message_Any.validate_ref(1'000'000, message.external)) {
    return td::Status::Error("live transfer external fails generated Message validation");
  }
  TRY_RESULT(boc, vm::std_boc_serialize(message.external, 31));
  TRY_RESULT(roundtrip, vm::std_boc_deserialize(boc.clone()));
  if (roundtrip->get_hash() != message.external->get_hash()) {
    return td::Status::Error("transfer external hash changed after BoC round-trip");
  }
  return td::Status::OK();
}

td::Status write_prepared_pool(const PreparedPool& pool, const Workload& workload, td::CSlice out_dir) {
  TRY_STATUS(td::mkpath(out_dir.str() + "/"));
  TRY_RESULT(kickoff_boc, vm::std_boc_serialize(pool.kickoff_external, 31));
  TRY_STATUS(td::write_file(out_dir.str() + "/kickoff-external.boc", kickoff_boc.as_slice()));

  std::ostringstream json;
  auto wallet1 = workload.account(1);
  json << "{\n"
       << "  \"version\": 1,\n"
       << "  \"workload\": \"devnet-jetton-spam-exact\",\n"
       << "  \"active_count\": " << pool.active_count << ",\n"
       << "  \"deployed_count\": " << (static_cast<td::uint64>(pool.active_count) + 1) << ",\n"
       << "  \"seed_hex\": \"" << pool.seed.to_hex() << "\",\n"
       << "  \"public_key_hex\": \"" << pool.public_key.to_hex() << "\",\n"
       << "  \"minter\": \"" << raw_address(pool.minter) << "\",\n"
       << "  \"wallet_spam_code_hash\": \"" << workload.contracts().wallet_spam_code->get_hash().to_hex() << "\",\n"
       << "  \"jetton_wallet_code_hash\": \"" << workload.contracts().jetton_wallet_code->get_hash().to_hex() << "\",\n"
       << "  \"funding_nanoton\": \"" << uint128_to_string(pool.funding) << "\",\n"
       << "  \"wallet0\": \"" << raw_address(pool.first.owner) << "\",\n"
       << "  \"wallet0_jetton\": \"" << raw_address(pool.first.jetton_wallet) << "\",\n"
       << "  \"wallet1\": \"" << raw_address(wallet1.owner) << "\",\n"
       << "  \"wallet1_jetton\": \"" << raw_address(wallet1.jetton_wallet) << "\",\n"
       << "  \"sentinel_wallet\": \"" << raw_address(pool.sentinel.owner) << "\",\n"
       << "  \"sentinel_jetton\": \"" << raw_address(pool.sentinel.jetton_wallet) << "\",\n"
       << "  \"kickoff_message_hash\": \"" << pool.kickoff_external->get_hash().to_hex() << "\"\n"
       << "}\n";
  return td::write_file(out_dir.str() + "/pool.json", json.str());
}

}  // namespace jetton_sim
