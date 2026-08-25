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
*/

#include <type_traits>

#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/block.h"
#include "smc-envelope/GenericAccount.h"
#include "td/utils/tests.h"
#include "validator/impl/external-message.hpp"
#include "vm/boc.h"
#include "vm/cells.h"

namespace {

using ton::validator::ExtMessage;
using ton::validator::ExtMessageQ;

struct MessageFixture {
  ton::StdSmcAddress address;
  td::Ref<vm::Cell> root;
  td::BufferSlice data;
};

MessageFixture make_external_message(td::Ref<vm::Cell> state_init = {}) {
  ton::StdSmcAddress address;
  address.set_zero();
  address.as_array().back() = 0x42;
  auto body = vm::CellBuilder().store_long(0x12345678, 32).finalize();
  auto root = ton::GenericAccount::create_ext_message(block::StdAddress{0, address}, std::move(state_init), body);
  CHECK(root.not_null());
  auto data = vm::std_boc_serialize(root).move_as_ok();
  return {address, std::move(root), std::move(data)};
}

td::Ref<vm::Cell> make_state_init_with_invalid_libraries() {
  // StateInit accepts library:(Maybe ^Cell), so this is a valid generic StateInit.
  // StateInitWithLibs interprets the same one-bit/ref field as a HashmapE and
  // rejects the deliberately malformed dictionary root.
  auto malformed_dictionary = vm::CellBuilder().finalize();
  vm::CellBuilder state_init;
  CHECK(state_init.store_zeroes_bool(4) && state_init.store_ones_bool(1) &&
        state_init.store_ref_bool(std::move(malformed_dictionary)));
  return state_init.finalize();
}

class ArbitraryExtMessage final : public ExtMessage {
 public:
  explicit ArbitraryExtMessage(MessageFixture fixture) : fixture_(std::move(fixture)) {
  }

  ton::AccountIdPrefixFull shard() const override {
    return {0, fixture_.address.bits().get_uint(64)};
  }
  td::BufferSlice serialize() const override {
    return fixture_.data.clone();
  }
  td::Ref<vm::Cell> root_cell() const override {
    return fixture_.root;
  }
  Hash hash() const override {
    return fixture_.root->get_hash().bits();
  }
  Hash hash_norm() const override {
    return hash();
  }
  ton::WorkchainId wc() const override {
    return 0;
  }
  ton::StdSmcAddress addr() const override {
    return fixture_.address;
  }

 private:
  MessageFixture fixture_;
};

static_assert(!std::is_copy_constructible_v<ExtMessageQ>);
static_assert(!std::is_move_constructible_v<ExtMessageQ>);

TEST(ExtMessageReuse, BufferFactoryCertifiesExactlyTheCachedHeader) {
  auto fixture = make_external_message();
  ASSERT_TRUE(block::gen::t_Message_Any.validate_ref(128, fixture.root));
  ASSERT_TRUE(block::tlb::t_Message.validate_ref(128, fixture.root));
  ASSERT_TRUE(block::tlb::validate_message_libs(fixture.root));

  auto message =
      ExtMessageQ::create_ext_message(fixture.data.clone(), block::SizeLimitsConfig::ExtMsgLimits{}).move_as_ok();
  ASSERT_TRUE(message->structurally_validated());
  ASSERT_EQ(message->root_cell()->get_hash(), fixture.root->get_hash());
  ASSERT_EQ(message->hash(), td::Bits256{fixture.root->get_hash().bits()});
  ASSERT_EQ(message->wc(), 0);
  ASSERT_EQ(message->addr(), fixture.address);

  block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
  ASSERT_TRUE(tlb::unpack_cell_inexact(fixture.root, info));
  ASSERT_EQ(message->shard(), block::tlb::t_MsgAddressInt.get_prefix(info.dest));
  ton::WorkchainId parsed_wc;
  ton::StdSmcAddress parsed_address;
  ASSERT_TRUE(block::tlb::t_MsgAddressInt.extract_std_address(info.dest, parsed_wc, parsed_address));
  ASSERT_EQ(message->wc(), parsed_wc);
  ASSERT_EQ(message->addr(), parsed_address);
}

TEST(ExtMessageReuse, InvalidLibrariesStayOnTheOriginalValidationPath) {
  auto fixture = make_external_message(make_state_init_with_invalid_libraries());
  ASSERT_TRUE(block::gen::t_Message_Any.validate_ref(128, fixture.root));
  ASSERT_TRUE(block::tlb::t_Message.validate_ref(128, fixture.root));
  ASSERT_TRUE(!block::tlb::validate_message_libs(fixture.root));

  auto message =
      ExtMessageQ::create_ext_message(fixture.data.clone(), block::SizeLimitsConfig::ExtMsgLimits{}).move_as_ok();
  ASSERT_TRUE(!message->structurally_validated());
}

TEST(ExtMessageReuse, RootFactoryAndArbitraryImplementationsStayOnFallback) {
  auto fixture = make_external_message();
  auto root_message = ExtMessageQ::create_ext_message(fixture.root).move_as_ok();
  ASSERT_TRUE(!root_message->structurally_validated());

  td::Ref<ExtMessage> arbitrary = td::Ref<ArbitraryExtMessage>{true, std::move(fixture)};
  ASSERT_TRUE(dynamic_cast<const ExtMessageQ*>(arbitrary.get()) == nullptr);
}

}  // namespace
