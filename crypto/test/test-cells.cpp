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
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/bigexp.h"
#include "common/bigint.hpp"
#include "common/bitstring.h"
#include "common/checksum.h"
#include "common/refcnt.hpp"
#include "common/refint.h"
#include "common/util.h"
#include "td/utils/crypto.h"
#include "td/utils/misc.h"
#include "td/utils/tests.h"
#include "vm/boc.h"
#include "vm/cells.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cells/MerkleUpdate.h"
#include "vm/cellslice.h"
#include "vm/dict.h"

static std::stringstream create_ss() {
  std::stringstream ss;
  ss.imbue(std::locale::classic());
  ss.setf(std::ios_base::fixed, std::ios_base::floatfield);
  ss.precision(6);
  return ss;
}
static std::stringstream os = create_ss();

void show_total_cells(std::ostream& stream) {
  stream << "total cells = " << vm::DataCell::get_total_data_cells() << std::endl;
}

TEST(Cells, simple) {
  os = create_ss();
  using namespace td::literals;
  vm::CellBuilder cb1, cb2;
  cb1.store_bytes("Hello, ", 7).reserve_slice(48) = td::BitSlice{(const unsigned char*)"world!", 48};
  cb2.store_bits(td::BitSlice{(const unsigned char*)"\xd0", 4})
      .store_long(17239, 16)
      .store_long(-17, 11)
      .store_long(1000000239, 32)
      .store_long(1000000239LL * 1000000239)
      .store_int256("-1000000000000000000000000239"_i256, 91);
  cb1.store_ref(cb2.finalize_copy());
  show_total_cells(os);
  cb2.store_bytes("<->", 3);
  td::Ref<vm::DataCell> c1{cb1.finalize_copy()}, c2{cb2.finalize_copy()};
  unsigned char hbuff[vm::Cell::hash_bytes];
  os << "cb1 = " << cb1 << "; hash=" << td::buffer_to_hex(td::Slice(cb1.compute_hash(hbuff), 32)) << "; c1 = " << *c1
     << std::endl;
  os << "cb2 = " << cb2 << "; hash=" << td::buffer_to_hex(td::Slice(cb2.compute_hash(hbuff), 32)) << "; c2 = " << *c2
     << std::endl;
  show_total_cells(os);

  vm::CellSlice cr1(c1);
  cr1.dump(os);
  os << "fetch_octet() = " << cr1.fetch_octet() << std::endl;
  cr1.dump(os);
  os << "fetch_octet() = " << cr1.fetch_octet() << std::endl;
  cr1.dump(os);
  os << "fetch_octet() = " << cr1.fetch_octet() << std::endl;
  cr1.dump(os);
  os << "fetch_octet() = " << cr1.fetch_octet() << std::endl;
  cr1.dump(os);
  os << "fetch_ref()=" << td::buffer_to_hex(cr1.prefetch_ref()->get_hash().as_slice()) << std::endl;

  vm::CellSlice cr(vm::NoVm(), cr1.fetch_ref());
  cr.dump(os);
  os << "prefetch_ulong(4)=" << cr.prefetch_ulong(4) << std::endl;
  cr.dump(os);
  os << "fetch_ulong(4)=" << cr.fetch_ulong(4) << std::endl;
  cr.dump(os);
  os << "fetch_long(16)=" << cr.fetch_long(16) << std::endl;
  cr.dump(os);
  os << "prefetch_long(11)=" << cr.prefetch_long(11) << std::endl;
  cr.dump(os);
  os << "fetch_int256(11)=" << cr.fetch_int256(11) << std::endl;
  cr.dump(os);
  os << "fetch_long(32)=" << cr.fetch_long(32) << std::endl;
  cr.dump(os);
  os << "prefetch_long(64)=" << cr.prefetch_long(64) << std::endl;
  cr.dump(os);
  os << "fetch_long(64)=" << cr.fetch_long(64) << std::endl;
  cr.dump(os);
  os << "prefetch_int256(91)=" << cr.prefetch_int256(91) << std::endl;
  cr.dump(os);
  os << "fetch_int256(91)=" << cr.fetch_int256(91) << std::endl;
  cr.dump(os);
  os << "fetch_long(24)=" << cr.fetch_long(24) << std::endl;
  cr.dump(os);
  cr.clear();

  REGRESSION_VERIFY(os.str());
}

namespace {

class CountAugmentation final : public vm::dict::AugmentationData {
 public:
  bool skip_extra(vm::CellSlice& cs) const override {
    return cs.advance(16);
  }

  bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice& value) const override {
    return cb.store_long_bool(1, 16);
  }

  bool eval_fork(vm::CellBuilder& cb, vm::CellSlice& left, vm::CellSlice& right) const override {
    return left.have(16) && right.have(16) && cb.store_long_bool(left.fetch_ulong(16) + right.fetch_ulong(16), 16);
  }

  bool eval_empty(vm::CellBuilder& cb) const override {
    return cb.store_zeroes_bool(16);
  }
};

td::Ref<vm::CellSlice> make_dict_test_value(unsigned value) {
  vm::CellBuilder cb;
  cb.store_long(value, 32);
  return vm::load_cell_slice_ref(cb.finalize());
}

td::Ref<vm::CellSlice> make_dict_test_value_with_ref(unsigned value) {
  auto payload = vm::CellBuilder{}.store_long(value, 32).finalize();
  return vm::load_cell_slice_ref(vm::CellBuilder{}.store_long(value, 32).store_ref(std::move(payload)).finalize());
}

}  // namespace

TEST(AugmentedDictionary, multiset_matches_sequential_updates) {
  static const CountAugmentation augmentation;
  vm::AugmentedDictionary sequential{16, augmentation};
  for (unsigned i = 0; i < 512; ++i) {
    td::BitArray<16> key{static_cast<long long>(i * 2)};
    ASSERT_TRUE(sequential.set(key, make_dict_test_value(i)));
  }
  vm::AugmentedDictionary batched{sequential};

  std::vector<td::BitArray<16>> keys;
  std::vector<td::Ref<vm::CellSlice>> values;
  keys.reserve(320);
  values.reserve(320);
  for (unsigned i = 0; i < 128; ++i) {
    keys.emplace_back(static_cast<long long>(i * 2));
    values.push_back(make_dict_test_value(10000 + i));
  }
  for (unsigned i = 128; i < 192; ++i) {
    keys.emplace_back(static_cast<long long>(i * 2));
    values.emplace_back();
  }
  for (unsigned i = 0; i < 128; ++i) {
    keys.emplace_back(static_cast<long long>(i * 2 + 1));
    values.push_back(make_dict_test_value(20000 + i));
  }

  std::vector<vm::AugmentedDictionary::MultiSetValue> updates;
  updates.reserve(keys.size());
  for (size_t i = 0; i < keys.size(); ++i) {
    updates.emplace_back(keys[i].bits(), values[i]);
    if (values[i].is_null()) {
      ASSERT_TRUE(sequential.lookup_delete(keys[i]).not_null());
    } else {
      ASSERT_TRUE(sequential.set(keys[i], values[i]));
    }
  }
  std::reverse(updates.begin(), updates.end());
  ASSERT_TRUE(batched.multiset(updates));
  ASSERT_TRUE(sequential.validate());
  ASSERT_TRUE(batched.validate());
  ASSERT_EQ(sequential.get_wrapped_dict_root()->get_hash(), batched.get_wrapped_dict_root()->get_hash());
}

TEST(AugmentedDictionary, lookup_multi_matches_sequential_lookups) {
  static const CountAugmentation augmentation;
  vm::AugmentedDictionary dict{16, augmentation};
  for (unsigned i = 0; i < 512; ++i) {
    td::BitArray<16> key{static_cast<long long>(i * 2)};
    ASSERT_TRUE(dict.set(key, make_dict_test_value(i)));
  }

  std::vector<td::BitArray<16>> keys;
  std::vector<td::ConstBitPtr> key_ptrs;
  for (unsigned i = 0; i < 768; ++i) {
    keys.emplace_back(static_cast<long long>(i));
  }
  for (const auto& key : keys) {
    key_ptrs.push_back(key.bits());
  }
  auto values = dict.lookup_multi(key_ptrs, 16);
  ASSERT_EQ(values.size(), keys.size());
  for (size_t i = 0; i < keys.size(); ++i) {
    auto expected = dict.lookup(keys[i]);
    ASSERT_EQ(values[i].is_null(), expected.is_null());
    if (expected.not_null()) {
      ASSERT_EQ(values[i]->prefetch_ulong(32), expected->prefetch_ulong(32));
    }
  }
}

TEST(AugmentedDictionary, lookup_multi_preserves_usage_proof) {
  static const CountAugmentation augmentation;
  vm::AugmentedDictionary original{16, augmentation};
  for (unsigned i = 0; i < 512; ++i) {
    td::BitArray<16> key{static_cast<long long>(i * 2)};
    ASSERT_TRUE(original.set(key, make_dict_test_value(i)));
  }
  auto raw_root = original.get_root_cell();
  auto sequential_tree = std::make_shared<vm::CellUsageTree>();
  auto batched_tree = std::make_shared<vm::CellUsageTree>();
  vm::AugmentedDictionary sequential{vm::UsageCell::create(raw_root, sequential_tree->root_ptr()), 16, augmentation,
                                     false};
  vm::AugmentedDictionary batched{vm::UsageCell::create(raw_root, batched_tree->root_ptr()), 16, augmentation, false};

  std::vector<td::BitArray<16>> keys;
  std::vector<td::ConstBitPtr> key_ptrs;
  for (unsigned i = 0; i < 256; ++i) {
    keys.emplace_back(static_cast<long long>(i * 3));
  }
  for (const auto& key : keys) {
    key_ptrs.push_back(key.bits());
    sequential.lookup(key);
  }
  batched.lookup_multi(key_ptrs, 16);

  vm::NewCellStorageStat sequential_stat;
  vm::NewCellStorageStat batched_stat;
  sequential_stat.add_proof(sequential.get_root_cell(), sequential_tree.get());
  batched_stat.add_proof(batched.get_root_cell(), batched_tree.get());
  ASSERT_TRUE(sequential_stat.get_proof_stat() == batched_stat.get_proof_stat());
}

TEST(AugmentedDictionary, analytical_replacement_proof_matches_materialized_updates) {
  static const CountAugmentation augmentation;
  vm::AugmentedDictionary original{16, augmentation};
  for (unsigned i = 0; i < 4096; ++i) {
    ASSERT_TRUE(original.set(td::BitArray<16>{static_cast<long long>(i)}, make_dict_test_value_with_ref(i)));
  }
  auto raw_root = original.get_root_cell();
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  vm::AugmentedDictionary materialized{vm::UsageCell::create(raw_root, usage_tree->root_ptr()), 16, augmentation,
                                       false};
  vm::AugmentedDictionary analytical{raw_root, 16, augmentation, false};
  vm::NewCellStorageStat proof_stat;
  std::vector<td::BitArray<16>> previous_keys;

  const std::array<std::array<unsigned, 8>, 4> batches{{
      {{1, 3, 127, 1024, 2048, 3000, 4000, 4095}},
      {{2, 4, 126, 1025, 2049, 3001, 3999, 4094}},
      {{5, 6, 125, 1026, 2050, 3002, 3998, 4093}},
      {{7, 8, 124, 1027, 2051, 3003, 3997, 4092}},
  }};
  for (size_t batch_index = 0; batch_index < batches.size(); ++batch_index) {
    std::vector<td::BitArray<16>> current_keys;
    std::vector<td::Ref<vm::CellSlice>> values;
    std::vector<vm::AugmentedDictionary::MultiSetValue> updates;
    for (unsigned key : batches[batch_index]) {
      current_keys.emplace_back(static_cast<long long>(key));
    }
    std::sort(current_keys.begin(), current_keys.end());
    for (const auto& key : current_keys) {
      auto payload = vm::CellBuilder{}
                         .store_long(10000 + batch_index * 4096 + key.bits().get_uint(16), 32)
                         .finalize();
      proof_stat.add_proof(payload, usage_tree.get());
      values.push_back(vm::load_cell_slice_ref(
          vm::CellBuilder{}.store_long(20000 + batch_index, 32).store_ref(std::move(payload)).finalize()));
    }
    for (size_t i = 0; i < current_keys.size(); ++i) {
      updates.emplace_back(current_keys[i].bits(), values[i]);
    }

    std::vector<td::ConstBitPtr> current_ptrs;
    std::vector<td::ConstBitPtr> previous_ptrs;
    for (const auto& key : current_keys) {
      current_ptrs.push_back(key.bits());
    }
    for (const auto& key : previous_keys) {
      previous_ptrs.push_back(key.bits());
    }
    auto predicted = analytical.estimate_replacement_proof_increment(current_ptrs, previous_ptrs, 16);
    auto before = proof_stat.get_proof_stat();
    ASSERT_TRUE(materialized.multiset(updates));
    proof_stat.add_proof(materialized.get_root_cell(), usage_tree.get());
    auto after = proof_stat.get_proof_stat();
    vm::NewCellStorageStat::Stat actual{after.cells - before.cells, after.bits - before.bits,
                                        after.internal_refs - before.internal_refs,
                                        after.external_refs - before.external_refs};
    ASSERT_EQ(predicted.cells, actual.cells);
    ASSERT_EQ(predicted.bits, actual.bits);
    ASSERT_EQ(predicted.internal_refs, actual.internal_refs);
    ASSERT_EQ(predicted.external_refs, actual.external_refs);

    previous_keys.insert(previous_keys.end(), current_keys.begin(), current_keys.end());
    std::sort(previous_keys.begin(), previous_keys.end());
  }
}

TEST(AugmentedDictionary, separate_usage_tree_preserves_proof_accounting) {
  static const CountAugmentation augmentation;
  vm::AugmentedDictionary original{16, augmentation};
  for (unsigned i = 0; i < 512; ++i) {
    td::BitArray<16> key{static_cast<long long>(i * 2)};
    ASSERT_TRUE(original.set(key, make_dict_test_value(i)));
  }
  auto raw_root = original.get_root_cell();

  auto first_tree = std::make_shared<vm::CellUsageTree>();
  auto second_tree = std::make_shared<vm::CellUsageTree>();
  auto first_root = vm::UsageCell::create(raw_root, first_tree->root_ptr());
  auto second_root = vm::UsageCell::create(raw_root, second_tree->root_ptr());
  auto* first_usage_cell = dynamic_cast<const vm::UsageCell*>(first_root.get());
  ASSERT_TRUE(first_usage_cell != nullptr);
  ASSERT_EQ(first_usage_cell->underlying_cell().get(), raw_root.get());

  vm::AugmentedDictionary first{first_root, 16, augmentation, false};
  vm::AugmentedDictionary second{second_root, 16, augmentation, false};
  std::vector<td::BitArray<16>> keys;
  std::vector<td::Ref<vm::CellSlice>> values;
  std::vector<vm::AugmentedDictionary::MultiSetValue> first_updates;
  std::vector<vm::AugmentedDictionary::MultiSetValue> second_updates;
  for (unsigned i = 0; i < 64; ++i) {
    keys.emplace_back(static_cast<long long>(i * 6));
    values.push_back(make_dict_test_value(10000 + i));
  }
  for (size_t i = 0; i < keys.size(); ++i) {
    first_updates.emplace_back(keys[i].bits(), values[i]);
    second_updates.emplace_back(keys[i].bits(), values[i]);
  }
  ASSERT_TRUE(first.multiset(first_updates));
  ASSERT_TRUE(second.multiset(second_updates));
  ASSERT_EQ(first.get_root_cell()->get_hash(), second.get_root_cell()->get_hash());

  vm::NewCellStorageStat first_stat;
  vm::NewCellStorageStat second_stat;
  first_stat.add_proof(first.get_root_cell(), first_tree.get());
  second_stat.add_proof(second.get_root_cell(), second_tree.get());
  ASSERT_TRUE(first_stat.get_proof_stat() == second_stat.get_proof_stat());
}

TEST(CellUsageTree, usage_cell_can_rebind_to_existing_tree_path) {
  auto leaf = vm::CellBuilder{}.store_long(0x11, 8).finalize();
  auto raw_root = vm::CellBuilder{}.store_long(0x22, 8).store_ref(leaf).finalize();
  auto source_tree = std::make_shared<vm::CellUsageTree>();
  auto target_tree = std::make_shared<vm::CellUsageTree>();
  auto source_root = vm::UsageCell::create(raw_root, source_tree->root_ptr());
  auto target_root = vm::UsageCell::create(raw_root, target_tree->root_ptr());
  vm::CellSlice source_slice{vm::NoVm(), source_root};
  vm::CellSlice target_slice{vm::NoVm(), target_root};
  auto source_child = source_slice.prefetch_ref(0);
  auto target_child = target_slice.prefetch_ref(0);
  auto source_node = source_child->get_tree_node().node_id_for(source_tree.get());
  auto target_node = target_child->get_tree_node().node_id_for(target_tree.get());
  ASSERT_TRUE(source_node != 0);
  ASSERT_TRUE(target_node != 0);
  ASSERT_TRUE(!target_tree->is_loaded(target_node));

  auto* usage_child = dynamic_cast<const vm::UsageCell*>(source_child.get());
  ASSERT_TRUE(usage_child != nullptr);
  ASSERT_TRUE(usage_child->rebind_tree_node(source_tree.get(), target_child->get_tree_node()));
  ASSERT_EQ(source_child->get_tree_node().node_id_for(source_tree.get()), 0u);
  ASSERT_EQ(source_child->get_tree_node().node_id_for(target_tree.get()), target_node);
  vm::CellSlice rebound_slice{vm::NoVm(), source_child};
  ASSERT_TRUE(target_tree->is_loaded(target_node));
}

TEST(CellUsageTree, auxiliary_paths_support_merkle_update) {
  auto old_leaf = vm::CellBuilder{}.store_long(1, 8).finalize();
  auto new_leaf = vm::CellBuilder{}.store_long(2, 8).finalize();
  auto branch_left = vm::CellBuilder{}.store_long(3, 8).finalize();
  auto branch_right = vm::CellBuilder{}.store_long(4, 8).finalize();
  auto unchanged_branch =
      vm::CellBuilder{}.store_long(5, 8).store_ref(branch_left).store_ref(branch_right).finalize();
  auto old_account =
      vm::CellBuilder{}.store_long(6, 8).store_ref(old_leaf).store_ref(unchanged_branch).finalize();
  auto other_state = vm::CellBuilder{}.store_long(7, 8).finalize();
  auto old_state = vm::CellBuilder{}.store_long(8, 8).store_ref(old_account).store_ref(other_state).finalize();

  auto canonical_tree = std::make_shared<vm::CellUsageTree>();
  auto canonical_state = vm::UsageCell::create(old_state, canonical_tree->root_ptr());
  vm::CellSlice canonical_state_slice{vm::NoVm(), canonical_state};
  auto canonical_account = canonical_state_slice.prefetch_ref(0);
  auto canonical_other = canonical_state_slice.prefetch_ref(1);

  auto auxiliary_tree = std::make_shared<vm::CellUsageTree>();
  auto auxiliary_account = vm::UsageCell::create(old_account, auxiliary_tree->root_ptr());
  vm::CellSlice auxiliary_account_slice{vm::NoVm(), auxiliary_account};
  auto auxiliary_unchanged_branch = auxiliary_account_slice.prefetch_ref(1);
  auto updated_account = vm::CellBuilder{}
                             .store_long(6, 8)
                             .store_ref(new_leaf)
                             .store_ref(auxiliary_unchanged_branch)
                             .finalize();
  auto updated_state =
      vm::CellBuilder{}.store_long(8, 8).store_ref(updated_account).store_ref(canonical_other).finalize();

  vm::CellSlice canonical_account_slice{vm::NoVm(), canonical_account};
  auto canonical_unchanged_branch = canonical_account_slice.prefetch_ref(1);
  auto canonical_updated_account = vm::CellBuilder{}
                                       .store_long(6, 8)
                                       .store_ref(new_leaf)
                                       .store_ref(canonical_unchanged_branch)
                                       .finalize();
  auto canonical_updated_state =
      vm::CellBuilder{}.store_long(8, 8).store_ref(canonical_updated_account).store_ref(canonical_other).finalize();
  vm::NewCellStorageStat auxiliary_stat;
  vm::NewCellStorageStat canonical_stat;
  auxiliary_stat.add_proof(updated_state, canonical_tree.get(), auxiliary_tree.get());
  canonical_stat.add_proof(canonical_updated_state, canonical_tree.get());
  ASSERT_TRUE(auxiliary_stat.get_proof_stat() == canonical_stat.get_proof_stat());

  auto target_node = canonical_account->get_tree_node().node_id_for(canonical_tree.get());
  ASSERT_TRUE(target_node != 0);
  canonical_tree->import_paths_from(*auxiliary_tree, target_node);

  auto raw_update = vm::MerkleUpdate::generate_raw(canonical_state, updated_state, canonical_tree.get(),
                                                    auxiliary_tree.get(), target_node);
  ASSERT_TRUE(raw_update.is_ok());
  auto [update_from, update_to] = raw_update.move_as_ok();
  vm::CellSlice update_state_slice{vm::NoVm(), update_to};
  vm::CellSlice update_account_slice{vm::NoVm(), update_state_slice.prefetch_ref(0)};
  vm::CellSlice pruned_branch_slice{vm::NoVm(), update_account_slice.prefetch_ref(1)};
  ASSERT_EQ(pruned_branch_slice.special_type(), vm::CellTraits::SpecialType::PrunnedBranch);

  auto update = vm::CellBuilder::create_merkle_update(std::move(update_from), std::move(update_to));
  ASSERT_TRUE(vm::MerkleUpdate::validate(update).is_ok());
  auto applied = vm::MerkleUpdate::apply(old_state, update);
  ASSERT_TRUE(applied.is_ok());
  ASSERT_EQ(applied.ok()->get_hash(), updated_state->get_hash());
}

TEST(CellUsageTree, direct_merkle_usage_node_matches_legacy_update) {
  auto old_left = vm::CellBuilder{}.store_long(1, 8).finalize();
  auto old_right_left = vm::CellBuilder{}.store_long(2, 8).finalize();
  auto old_right_right = vm::CellBuilder{}.store_long(3, 8).finalize();
  auto old_right = vm::CellBuilder{}
                       .store_long(4, 8)
                       .store_ref(old_right_left)
                       .store_ref(old_right_right)
                       .finalize();
  auto old_root = vm::CellBuilder{}.store_long(5, 8).store_ref(old_left).store_ref(old_right).finalize();
  auto new_right_left = vm::CellBuilder{}.store_long(6, 8).finalize();

  auto prepare = [&](const std::shared_ptr<vm::CellUsageTree>& tree) {
    auto usage_root = vm::UsageCell::create(old_root, tree->root_ptr());
    vm::CellSlice root_slice{vm::NoVm(), usage_root};
    auto usage_left = root_slice.prefetch_ref(0);
    auto usage_right = root_slice.prefetch_ref(1);
    vm::CellSlice right_slice{vm::NoVm(), usage_right};
    auto usage_right_right = right_slice.prefetch_ref(1);
    auto new_right = vm::CellBuilder{}
                         .store_long(4, 8)
                         .store_ref(new_right_left)
                         .store_ref(usage_right_right)
                         .finalize();
    auto new_root = vm::CellBuilder{}
                        .store_long(5, 8)
                        .store_ref(usage_left)
                        .store_ref(new_right)
                        .finalize();
    return std::make_pair(std::move(usage_root), std::move(new_root));
  };

  auto legacy_tree = std::make_shared<vm::CellUsageTree>();
  auto direct_tree = std::make_shared<vm::CellUsageTree>();
  auto snapshot_source_tree = std::make_shared<vm::CellUsageTree>();
  auto split_tree = std::make_shared<vm::CellUsageTree>();
  auto [legacy_old, legacy_new] = prepare(legacy_tree);
  auto [direct_old, direct_new] = prepare(direct_tree);
  auto [snapshot_old, snapshot_new] = prepare(snapshot_source_tree);
  auto [split_old, split_new] = prepare(split_tree);
  auto proof_snapshot = snapshot_source_tree->clone_for_proof();
  auto legacy = vm::MerkleUpdate::generate(legacy_old, legacy_new, legacy_tree.get(), nullptr, 0, false);
  auto direct = vm::MerkleUpdate::generate(direct_old, direct_new, direct_tree.get(), nullptr, 0, true);
  auto snapshot = vm::MerkleUpdate::generate(old_root, snapshot_new, snapshot_source_tree.get(), nullptr, 0,
                                              true, proof_snapshot.get());
  auto raw_split_new = vm::MerkleUpdate::generate_raw_new_proof(split_new, split_tree.get());
  ASSERT_TRUE(raw_split_new.is_ok());
  split_tree->set_use_mark_for_is_loaded(true);
  auto split = vm::MerkleUpdate::complete_from_raw_new_proof(split_old, raw_split_new.move_as_ok(), split_tree.get());
  ASSERT_TRUE(legacy.is_ok());
  ASSERT_TRUE(direct.is_ok());
  ASSERT_TRUE(snapshot.is_ok());
  ASSERT_TRUE(split.is_ok());
  ASSERT_EQ(legacy.ok()->get_hash(), direct.ok()->get_hash());
  ASSERT_EQ(legacy.ok()->get_hash(), snapshot.ok()->get_hash());
  ASSERT_EQ(legacy.ok()->get_hash(), split.ok()->get_hash());
  ASSERT_TRUE(vm::MerkleUpdate::validate(direct.ok()).is_ok());
  auto applied = vm::MerkleUpdate::apply(old_root, direct.ok());
  ASSERT_TRUE(applied.is_ok());
  ASSERT_EQ(applied.ok()->get_hash(), direct_new->get_hash());
}

TEST(CellUsageTree, shared_old_state_traversal_matches_separate_proofs) {
  auto old_left = vm::CellBuilder{}.store_long(1, 8).finalize();
  auto old_right_left = vm::CellBuilder{}.store_long(2, 8).finalize();
  auto old_right_right = vm::CellBuilder{}.store_long(3, 8).finalize();
  auto old_right = vm::CellBuilder{}
                       .store_long(4, 8)
                       .store_ref(old_right_left)
                       .store_ref(old_right_right)
                       .finalize();
  auto old_root = vm::CellBuilder{}.store_long(5, 8).store_ref(old_left).store_ref(old_right).finalize();
  auto new_right_left = vm::CellBuilder{}.store_long(6, 8).finalize();

  auto prepare = [&](const std::shared_ptr<vm::CellUsageTree>& tree) {
    auto usage_root = vm::UsageCell::create(old_root, tree->root_ptr());
    vm::CellSlice root_slice{vm::NoVm(), usage_root};
    auto usage_left = root_slice.prefetch_ref(0);
    auto usage_right = root_slice.prefetch_ref(1);
    vm::CellSlice right_slice{vm::NoVm(), usage_right};
    auto usage_right_right = right_slice.prefetch_ref(1);
    auto new_right = vm::CellBuilder{}
                         .store_long(4, 8)
                         .store_ref(new_right_left)
                         .store_ref(usage_right_right)
                         .finalize();
    auto new_root = vm::CellBuilder{}
                        .store_long(5, 8)
                        .store_ref(usage_left)
                        .store_ref(new_right)
                        .finalize();
    return std::make_pair(std::move(usage_root), std::move(new_root));
  };
  auto collated_is_prunned = [old_right_hash = old_right->get_hash()](const td::Ref<vm::Cell>& cell) {
    return cell->get_hash() == old_right_hash;
  };

  auto legacy_tree = std::make_shared<vm::CellUsageTree>();
  auto shared_tree = std::make_shared<vm::CellUsageTree>();
  auto [legacy_old, legacy_new] = prepare(legacy_tree);
  auto [shared_old, shared_new] = prepare(shared_tree);
  auto legacy_update = vm::MerkleUpdate::generate(legacy_old, legacy_new, legacy_tree.get());
  auto legacy_collated_proof = vm::MerkleProof::generate(legacy_old, collated_is_prunned);
  auto shared = vm::MerkleUpdate::generate_with_shared_old_proof(
      shared_old, shared_new, shared_tree.get(), collated_is_prunned);

  ASSERT_TRUE(legacy_update.is_ok());
  ASSERT_TRUE(legacy_collated_proof.is_ok());
  ASSERT_TRUE(shared.is_ok());
  ASSERT_EQ(shared.ok().first->get_hash(), legacy_update.ok()->get_hash());
  ASSERT_EQ(shared.ok().second->get_hash(), legacy_collated_proof.ok()->get_hash());
  ASSERT_TRUE(vm::MerkleUpdate::validate(shared.ok().first).is_ok());
  auto applied = vm::MerkleUpdate::apply(old_root, shared.ok().first);
  ASSERT_TRUE(applied.is_ok());
  ASSERT_EQ(applied.ok()->get_hash(), shared_new->get_hash());
}

TEST(MerkleProof, parallel_predicate_traversal_matches_serial_boc) {
  auto shared_leaf = vm::CellBuilder{}.store_long(17, 8).finalize();
  auto left = vm::CellBuilder{}.store_long(1, 8).store_ref(shared_leaf).finalize();
  auto right_left = vm::CellBuilder{}.store_long(2, 8).store_ref(shared_leaf).finalize();
  auto right_right = vm::CellBuilder{}.store_long(3, 8).finalize();
  auto right = vm::CellBuilder{}.store_long(4, 8).store_ref(right_left).store_ref(right_right).finalize();
  auto fork = vm::CellBuilder{}.store_long(5, 8).store_ref(left).store_ref(right).finalize();
  auto root = vm::CellBuilder{}.store_long(6, 8).store_ref(fork).finalize();
  auto is_pruned = [pruned_hash = right_left->get_hash()](const td::Ref<vm::Cell>& cell) {
    return cell->get_hash() == pruned_hash;
  };

  auto serial = vm::MerkleProof::generate(root, is_pruned);
  auto parallel = vm::MerkleProof::generate_parallel(root, is_pruned, 8);
  ASSERT_TRUE(serial.is_ok());
  ASSERT_TRUE(parallel.is_ok());
  ASSERT_EQ(serial.ok()->get_hash(), parallel.ok()->get_hash());

  auto serialize = [](td::Ref<vm::Cell> proof) {
    vm::BagOfCells boc;
    boc.set_root(std::move(proof));
    ASSERT_TRUE(boc.import_cells().is_ok());
    return boc.serialize_to_slice(31);
  };
  auto serial_boc = serialize(serial.move_as_ok());
  auto parallel_boc = serialize(parallel.move_as_ok());
  ASSERT_TRUE(serial_boc.is_ok());
  ASSERT_TRUE(parallel_boc.is_ok());
  ASSERT_TRUE(serial_boc.ok().as_slice() == parallel_boc.ok().as_slice());
}

TEST(MerkleProof, parallel_usage_tree_traversal_matches_serial_boc) {
  std::vector<td::Ref<vm::Cell>> level;
  for (unsigned i = 0; i < 256; ++i) {
    level.push_back(vm::CellBuilder{}.store_long(i, 16).finalize());
  }
  while (level.size() > 1) {
    std::vector<td::Ref<vm::Cell>> next;
    for (size_t i = 0; i < level.size(); i += 2) {
      next.push_back(vm::CellBuilder{}.store_ref(level[i]).store_ref(level[i + 1]).finalize());
    }
    level = std::move(next);
  }
  auto root = level.front();
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_root = vm::UsageCell::create(root, usage_tree->root_ptr());
  std::function<void(td::Ref<vm::Cell>, unsigned)> load_paths =
      [&](td::Ref<vm::Cell> cell, unsigned depth) {
        vm::CellSlice cs{vm::NoVm(), std::move(cell)};
        if (depth == 0) {
          return;
        }
        load_paths(cs.prefetch_ref(0), depth - 1);
        load_paths(cs.prefetch_ref(1), depth - 1);
      };
  load_paths(std::move(usage_root), 6);

  auto serial = vm::MerkleProof::generate_raw(root, usage_tree.get());
  auto parallel = vm::MerkleProof::generate_raw_parallel(root, usage_tree.get(), 8);
  ASSERT_TRUE(serial.is_ok());
  ASSERT_TRUE(parallel.is_ok());
  ASSERT_EQ(serial.ok()->get_hash(), parallel.ok()->get_hash());

  auto serialize = [](td::Ref<vm::Cell> proof) {
    vm::BagOfCells boc;
    boc.set_root(std::move(proof));
    ASSERT_TRUE(boc.import_cells().is_ok());
    return boc.serialize_to_slice(31);
  };
  auto serial_boc = serialize(serial.move_as_ok());
  auto parallel_boc = serialize(parallel.move_as_ok());
  ASSERT_TRUE(serial_boc.is_ok());
  ASSERT_TRUE(parallel_boc.is_ok());
  ASSERT_TRUE(serial_boc.ok().as_slice() == parallel_boc.ok().as_slice());
}

TEST(CellUsageTree, auxiliary_dictionary_update_supports_virtualized_scan_diff) {
  static const CountAugmentation augmentation;
  auto make_key = [](td::Slice domain, unsigned i) {
    return td::sha256_bits256(PSTRING() << domain << ':' << i);
  };
  vm::AugmentedDictionary original{256, augmentation};
  std::vector<td::Bits256> original_keys;
  for (unsigned i = 0; i < 4096; ++i) {
    original_keys.push_back(make_key("old", i));
    ASSERT_TRUE(original.set(original_keys.back(), make_dict_test_value(i)));
  }
  auto raw_dict_root = original.get_root_cell();
  auto unchanged = vm::CellBuilder{}.store_long(0x1234, 16).finalize();
  auto raw_state = vm::CellBuilder{}.store_long(0x55, 8).store_ref(raw_dict_root).store_ref(unchanged).finalize();

  auto canonical_tree = std::make_shared<vm::CellUsageTree>();
  auto canonical_stat = std::make_shared<vm::ProofStorageStat>();
  canonical_tree->set_cell_load_callback([canonical_stat](const vm::LoadedCell& cell) {
    canonical_stat->add_loaded_cell(cell.data_cell, static_cast<td::uint8>(cell.effective_level));
  });
  auto canonical_state = vm::UsageCell::create(raw_state, canonical_tree->root_ptr());
  vm::CellSlice canonical_state_slice{vm::NoVm(), canonical_state};
  auto canonical_dict_root = canonical_state_slice.prefetch_ref(0);
  vm::AugmentedDictionary canonical_dict{canonical_dict_root, 256, augmentation, false};

  auto worker_tree = std::make_shared<vm::CellUsageTree>();
  auto worker_stat = std::make_shared<vm::ProofStorageStat>();
  worker_tree->set_cell_load_callback([worker_stat](const vm::LoadedCell& cell) {
    worker_stat->add_loaded_cell(cell.data_cell, static_cast<td::uint8>(cell.effective_level));
  });
  vm::AugmentedDictionary worker_dict{vm::UsageCell::create(raw_dict_root, worker_tree->root_ptr()), 256,
                                      augmentation, false};

  std::vector<td::Bits256> keys;
  std::vector<td::Ref<vm::CellSlice>> values;
  std::vector<vm::AugmentedDictionary::MultiSetValue> updates;
  for (unsigned i = 0; i < 64; ++i) {
    keys.push_back(original_keys[i * 17]);
    values.push_back(make_dict_test_value_with_ref(10000 + i));
  }
  for (unsigned i = 0; i < 32; ++i) {
    keys.push_back(original_keys[2048 + i * 19]);
    values.emplace_back();
  }
  for (unsigned i = 0; i < 64; ++i) {
    keys.push_back(make_key("new", i));
    values.push_back(make_dict_test_value_with_ref(20000 + i));
  }
  for (size_t i = 0; i < keys.size(); ++i) {
    updates.emplace_back(keys[i].bits(), values[i]);
  }
  ASSERT_TRUE(worker_dict.multiset(updates));

  unsigned frontier_differences = 0;
  ASSERT_TRUE(canonical_dict.scan_diff(
      worker_dict,
      [&frontier_differences](td::ConstBitPtr, int, td::Ref<vm::CellSlice>, td::Ref<vm::CellSlice>) {
        ++frontier_differences;
        return true;
      },
      2));
  ASSERT_EQ(frontier_differences, updates.size());

  auto updated_state = vm::CellBuilder{}
                           .store_long(0x55, 8)
                           .store_ref(worker_dict.get_root_cell())
                           .store_ref(canonical_state_slice.prefetch_ref(1))
                           .finalize();
  auto canonical_target = canonical_dict_root->get_tree_node().node_id_for(canonical_tree.get());
  ASSERT_TRUE(canonical_target != 0);
  canonical_tree->import_paths_from(*worker_tree, canonical_target);
  canonical_stat->add_loaded_cells(*worker_stat);

  auto update = vm::MerkleUpdate::generate(canonical_state, updated_state, canonical_tree.get(), worker_tree.get(),
                                            canonical_target);
  ASSERT_TRUE(update.is_ok());

  auto collated_proof = vm::MerkleProof::generate(
      canonical_state, [canonical_stat](const td::Ref<vm::Cell>& cell) {
        return !canonical_stat->is_loaded(cell->get_hash());
      });
  ASSERT_TRUE(collated_proof.is_ok());
  auto virtual_old = vm::MerkleProof::virtualize(collated_proof.ok());
  ASSERT_TRUE(virtual_old.is_ok());
  auto virtual_new = vm::MerkleUpdate::apply(virtual_old.ok(), update.ok());
  ASSERT_TRUE(virtual_new.is_ok());

  vm::CellSlice old_state_slice{vm::NoVm(), virtual_old.ok()};
  vm::CellSlice new_state_slice{vm::NoVm(), virtual_new.ok()};
  vm::AugmentedDictionary old_dict{old_state_slice.prefetch_ref(0), 256, augmentation, false};
  vm::AugmentedDictionary new_dict{new_state_slice.prefetch_ref(0), 256, augmentation, false};
  unsigned differences = 0;
  ASSERT_TRUE(old_dict.scan_diff(
      new_dict,
      [&differences](td::ConstBitPtr, int, td::Ref<vm::CellSlice>, td::Ref<vm::CellSlice>) {
        ++differences;
        return true;
      },
      2));
  ASSERT_EQ(differences, updates.size());
}

TEST(AugmentedDictionary, partitioned_scan_diff_matches_serial_usage_proof) {
  static const CountAugmentation augmentation;
  auto make_key = [](unsigned i) { return td::sha256_bits256(PSTRING() << "partitioned-scan:" << i); };

  vm::AugmentedDictionary original{256, augmentation};
  std::vector<td::Bits256> keys;
  keys.reserve(4096);
  for (unsigned i = 0; i < 4096; ++i) {
    keys.push_back(make_key(i));
    ASSERT_TRUE(original.set(keys.back(), make_dict_test_value(i)));
  }
  auto raw_root = original.get_root_cell();

  struct Result {
    vm::CellHash updated_hash;
    vm::CellHash proof_hash;
    td::uint64 proof_size;
    unsigned differences;
    size_t tasks;
  };
  auto run = [&](bool partitioned) {
    auto usage_tree = std::make_shared<vm::CellUsageTree>();
    auto proof_stat = std::make_shared<vm::ProofStorageStat>();
    std::mutex proof_mutex;
    usage_tree->set_cell_load_callback([&](const vm::LoadedCell& cell) {
      std::lock_guard<std::mutex> guard{proof_mutex};
      proof_stat->add_loaded_cell(cell.data_cell, static_cast<td::uint8>(cell.effective_level));
    });
    auto wrapped_root = vm::UsageCell::create(raw_root, usage_tree->root_ptr());
    vm::AugmentedDictionary old_dict{wrapped_root, 256, augmentation, false};
    vm::AugmentedDictionary new_dict{wrapped_root, 256, augmentation, false};

    std::vector<td::Ref<vm::CellSlice>> values;
    std::vector<vm::AugmentedDictionary::MultiSetValue> updates;
    values.reserve(512);
    updates.reserve(512);
    for (unsigned i = 0; i < 512; ++i) {
      values.push_back(make_dict_test_value_with_ref(100000 + i));
      updates.emplace_back(keys[i * 7].bits(), values.back());
    }
    ASSERT_TRUE(new_dict.multiset(updates));

    std::atomic<unsigned> differences{0};
    auto callback = [&](td::ConstBitPtr, int, td::Ref<vm::CellSlice>, td::Ref<vm::CellSlice>) {
      differences.fetch_add(1, std::memory_order_relaxed);
      return true;
    };
    size_t task_count = 1;
    if (partitioned) {
      auto tasks = old_dict.prepare_scan_diff_tasks(new_dict, callback, 2, 8);
      task_count = tasks.size();
      std::vector<td::uint8> results(tasks.size(), 0);
      std::vector<std::exception_ptr> errors(tasks.size());
      std::vector<std::thread> threads;
      threads.reserve(tasks.size());
      for (size_t i = 0; i < tasks.size(); ++i) {
        threads.emplace_back([&, i] {
          try {
            results[i] = tasks[i]();
          } catch (...) {
            errors[i] = std::current_exception();
          }
        });
      }
      for (auto& thread : threads) {
        thread.join();
      }
      for (size_t i = 0; i < tasks.size(); ++i) {
        if (errors[i]) {
          std::rethrow_exception(errors[i]);
        }
        ASSERT_TRUE(results[i] != 0);
      }
    } else {
      ASSERT_TRUE(old_dict.scan_diff(new_dict, callback, 2));
    }

    auto proof = vm::MerkleProof::generate(
        wrapped_root, [proof_stat](const td::Ref<vm::Cell>& cell) { return !proof_stat->is_loaded(cell->get_hash()); });
    ASSERT_TRUE(proof.is_ok());
    return Result{new_dict.get_root_cell()->get_hash(), proof.ok()->get_hash(), proof_stat->estimate_proof_size(),
                  differences.load(std::memory_order_relaxed), task_count};
  };

  auto serial = run(false);
  auto partitioned = run(true);
  ASSERT_EQ(partitioned.tasks, 8u);
  ASSERT_EQ(partitioned.differences, 512u);
  ASSERT_EQ(partitioned.updated_hash, serial.updated_hash);
  ASSERT_EQ(partitioned.proof_hash, serial.proof_hash);
  ASSERT_EQ(partitioned.proof_size, serial.proof_size);
  ASSERT_EQ(partitioned.differences, serial.differences);
}

TEST(CellUsageTree, concurrent_child_creation_and_load_are_deterministic) {
  auto tree = std::make_shared<vm::CellUsageTree>();
  std::atomic<unsigned> loaded{0};
  tree->set_cell_load_callback([&](const vm::LoadedCell&) { loaded.fetch_add(1, std::memory_order_relaxed); });
  auto data = vm::CellBuilder{}.store_long(0x55, 8).finalize();
  auto root = tree->root_ptr();

  std::vector<std::thread> threads;
  for (unsigned thread_id = 0; thread_id < 16; ++thread_id) {
    threads.emplace_back([&, thread_id] {
      for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        auto child = root.create_child(thread_id & 3);
        auto grandchild = child.create_child((thread_id >> 2) & 3);
        ASSERT_TRUE(grandchild.on_load(vm::LoadedCell{data, 0, {}}));
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  ASSERT_EQ(loaded.load(std::memory_order_relaxed), 16u);
  for (unsigned child_ref = 0; child_ref < 4; ++child_ref) {
    auto child = tree->get_child(tree->root_id(), child_ref);
    ASSERT_TRUE(child != 0);
    for (unsigned grandchild_ref = 0; grandchild_ref < 4; ++grandchild_ref) {
      auto grandchild = tree->get_child(child, grandchild_ref);
      ASSERT_TRUE(grandchild != 0);
      ASSERT_TRUE(tree->is_loaded(grandchild));
    }
  }
}

void test_two_bitstrings(const td::BitSlice& bs1, const td::BitSlice& bs2) {
  using td::to_binary;
  using td::to_hex;
  os << "bs1 = " << bs1.to_binary() << " = " << bs1.to_hex() << std::endl;
  os << "bs2 = " << to_binary(bs2) << " = " << to_hex(bs2) << std::endl;
  td::BitString st{bs1};
  //td::BitString st;
  //st.append(bs1);
  os << "st = " << to_binary(st) << " = " << to_hex(st) << std::endl;
  st.append(bs2);
  os << "st = " << to_binary(st) << " = " << to_hex(st) << std::endl;
  ASSERT_EQ(to_binary(st), to_binary(bs1) + to_binary(bs2));
  auto bs3 = st.subslice(bs1.size(), bs2.size());
  os << "bs3 = " << to_binary(bs3) << " = " << to_hex(bs3) << std::endl;
  ASSERT_EQ(to_binary(bs3), to_binary(bs2));
  ASSERT_EQ(to_hex(bs3), to_hex(bs2));
  bs1.dump(os);
  bs2.dump(os);
  bs3.dump(os);
  std::string bs2_bin = to_binary(bs2);
  for (unsigned i = 0; i <= bs2.size(); i++) {
    for (unsigned j = 0; j <= bs2.size() - i; j++) {
      auto bs4 = bs2.subslice(i, j);
      auto bs5 = bs3.subslice(i, j);
      if (!(to_binary(bs4) == to_binary(bs5) && to_hex(bs4) == to_hex(bs5) && to_binary(bs4) == bs2_bin.substr(i, j))) {
        bs4.dump(os);
        bs5.dump(os);
        os << "bs2.subslice(" << i << ", " << j << ") = " << to_binary(bs4) << " = " << to_hex(bs4) << std::endl;
        os << "bs3.subslice(" << i << ", " << j << ") = " << to_binary(bs5) << " = " << to_hex(bs5) << std::endl;
      }
      ASSERT_EQ(to_binary(bs4), to_binary(bs5));
      ASSERT_EQ(to_hex(bs4), to_hex(bs5));
      ASSERT_EQ(to_binary(bs4), bs2_bin.substr(i, j));
    }
  }
}

void test_one_bitstring(const td::BitSlice& bs) {
  std::string bs_bin = bs.to_binary();
  for (unsigned i1 = 0; i1 <= bs.size(); i1++) {
    for (unsigned j1 = 0; j1 <= bs.size() - i1; j1++) {
      auto bs1 = bs.subslice(i1, j1);
      ASSERT_EQ(bs1.to_binary(), bs_bin.substr(i1, j1));
      for (unsigned i2 = 0; i2 <= bs.size() && i2 < 8; i2++) {
        for (unsigned j2 = 0; j2 <= bs.size() - i2; j2++) {
          os << "(" << i1 << "," << j1 << ")+(" << i2 << "," << j2 << ")" << std::endl;
          auto bs2 = bs.subslice(i2, j2);
          ASSERT_EQ(bs2.to_binary(), bs_bin.substr(i2, j2));
          test_two_bitstrings(bs1, bs2);
        }
      }
    }
  }
}

void test_bitstring_fill(unsigned n, unsigned p, unsigned k) {
  td::BitString bs{n * 2};
  std::string s;
  auto sl1 = td::BitSlice{(const unsigned char*)"\x40", 2};
  for (unsigned i = 0; i < n; i++) {
    bs.append(sl1);
    s += "01";
  }
  os << td::to_binary(bs) << " = " << td::to_hex(bs) << std::endl;
  ASSERT_EQ(td::to_binary(bs), s);
  unsigned q = k %= p;
  for (unsigned i = 0; i < p; i++) {
    unsigned a = (q * n * 2) / p;
    unsigned b = ((q + 1) * n * 2) / p;
    bs.subslice_write(a, b - a) = (q & 1);
    std::fill(s.begin() + a, s.begin() + b, (q & 1) + '0');
    os << "Step " << i << " (" << a << "," << b << "): " << td::to_binary(bs) << " = " << td::to_hex(bs) << std::endl;
    ASSERT_EQ(td::to_binary(bs), s);
    q = (q + k) % p;
  }
  bs.subslice_write(4, 16) = td::BitSlice{(const unsigned char*)"\x69\x96", 16};
  os << td::to_binary(bs) << " = " << td::to_hex(bs) << std::endl;
  std::string t = "0110100110010110";
  std::copy(t.begin(), t.end(), s.begin() + 4);
  ASSERT_EQ(td::to_binary(bs), s);
}

TEST(Bitstrings, main) {
  os = create_ss();
  auto test = td::BitSlice{(const unsigned char*)"test", 32};
  ASSERT_EQ(test.to_hex(), "74657374");
  test_two_bitstrings({(const unsigned char*)"\xf1\xd0", 12}, test);
  test_two_bitstrings({(const unsigned char*)"\x9f", 3}, {(const unsigned char*)"t", 3});
  test_bitstring_fill(17 * 3, 17, 4);
  //test_one_bitstring({(const unsigned char*)"SuperTest", 72});
  REGRESSION_VERIFY(os.str());
}

void test_parse_dec(std::string s) {
  td::BigInt256 x, y;
  os << "s=\"" << s << "\"" << std::endl;
  x.parse_dec_slow(s);
  y.parse_dec(s);
  x.dump(os);
  y.dump(os);
  ASSERT_TRUE(x == y);
  std::string s1 = x.to_dec_string();
  os << s1 << std::endl;
  ASSERT_EQ(s, s1);
  std::string s2 = x.to_hex_string();
  os << s2 << std::endl;
  std::string s3 = x.to_hex_string_slow();
  os << s3 << std::endl;
  ASSERT_EQ(s2, s3);
}

void test_pow2(int exponent) {
  td::BigInt256 x;
  x.set_pow2(exponent);
  os << "2^" << exponent << " = " << x.to_dec_string() << " = 0x" << x.to_hex_string() << std::endl;
  x.dump(os);
}

void test_fits(const td::BigInt256& x) {
  int m = 0, n = 0;
  const int limit = 300;
  os << "x=" << x.to_dec_string() << "; log2(|x|)=" << std::log2(std::abs(x.to_double())) << std::endl;
  x.dump(os);
  while (m < limit && !x.unsigned_fits_bits(m)) {
    m++;
  }
  for (int i = m; i < limit; i++) {
    ASSERT_TRUE(x.unsigned_fits_bits(i));
  }
  int su = x.bit_size(false);
  while (n < limit && !x.signed_fits_bits(n)) {
    n++;
  }
  for (int i = n; i < limit; i++) {
    ASSERT_TRUE(x.signed_fits_bits(i));
  }
  int ss = x.bit_size();
  os << "x=" << x.to_dec_string() << "=0x" << x.to_hex_string() << "; x=" << x.to_double()
     << "; log2(|x|)=" << std::log2(std::abs(x.to_double())) << "; unsigned: " << m << "=" << su
     << " bits; signed: " << n << "=" << ss << " bits" << std::endl;
  ASSERT_TRUE(su == m || (su == 0x7fffffff && m == limit));
  ASSERT_EQ(ss, n);
  ASSERT_EQ(x.to_hex_string(), x.to_hex_string_slow());
  td::BigInt256 y, z;
  ASSERT_TRUE(y.parse_hex(x.to_hex_string()) && y == x);
  ASSERT_TRUE(z.parse_dec(x.to_dec_string()) && z == x);
}

void test_divmod(const td::BigInt256& x, const td::BigInt256& y) {
  td::BigInt256 q, r(x);
  os << "x = " << x << " = ";
  x.dump(os);
  os << "y = " << y << " = ";
  y.dump(os);
  if (!r.mod_div_bool(y, q)) {
    os << "division error!\n";
    ASSERT_TRUE(0);
  } else {
    q.dump(os);
    r.dump(os);
    if (!q.normalize_bool() || !r.normalize_bool()) {
      os << "cannot normalize q or r!\n";
      ASSERT_TRUE(0);
    } else {
      os << "q = " << q << "; r = " << r << std::endl;
      if (y.sgn() > 0) {
        ASSERT_TRUE(r.sgn() >= 0);
        ASSERT_TRUE(r.cmp(y) < 0);
      } else {
        ASSERT_TRUE(r.sgn() <= 0);
        ASSERT_TRUE(r.cmp(y) > 0);
      }
      r.add_mul(q, y);
      ASSERT_TRUE(r.normalize() == x);
    }
  }
}

void test_export_int(const td::BigInt256& x, bool sgnd = true) {
  os << "x = " << x.to_hex_string() << std::endl;
  int bad = 0, ok = 0;
  for (int i = 1; i <= 33; i++) {
    unsigned char buff[33];
    std::memset(buff, 0xcc, sizeof(buff));
    if (!x.export_bytes(buff, i, sgnd)) {
      ASSERT_EQ(bad, i - 1);
      bad = i;
      continue;
    } else if (++ok < 5) {
      if (bad == i - 1) {
        os << "export(" << bad << ", " << sgnd << ") = (bad)" << std::endl;
      }
      os << "export(" << i << ", " << sgnd << ") =";
      char tmp[33 * 3 + 1];
      for (int j = 0; j < i; j++) {
        sprintf(tmp + 3 * j, " %02x", buff[j]);
      }
      os << tmp << std::endl;
      td::BigInt256 y;
      ASSERT_TRUE(y.import_bytes(buff, i, sgnd));
      os << "import() = " << y.to_hex_string() << std::endl;
      ASSERT_TRUE(!x.cmp_un(y));
    }
  }
  if (!ok) {
    os << "export(" << bad << ", " << sgnd << ") = (bad)" << std::endl;
  }
}

TEST(Bigint, main) {
  os = create_ss();
  using namespace td::literals;
  td::BigInt256 x, y, z;
  test_parse_dec("0");
  test_parse_dec("1");
  test_parse_dec("-1");
  test_parse_dec("123");
  test_parse_dec("-239");
  test_parse_dec("-115792089237316195423570985008687907853269984665640564039457584007913129639936");
  test_parse_dec("115792089237316195423570985008687907853269984665640564039457584007913129639935");
  test_parse_dec("143126893554044595713052252685501316785002612509329899766666973726012466208042");
  test_parse_dec("100000000000000000000000000000000000000000000000000000000000000000000000000001");
  x.parse_dec("11111111111111111111111111111111111111111111111111111111111111111111111111111");
  y.parse_dec("22222222222222222222222222222222222");
  x += y;
  os << x.to_dec_string() << std::endl;
  x -= y;
  os << x.to_dec_string() << std::endl;
  x -= y;
  os << x.to_dec_string() << std::endl;
  y -= x;
  os << y.to_dec_string() << std::endl;
  y += x;
  os << x.to_dec_string() << std::endl;
  x.parse_dec("10000000000000000000000000000001");
  y.parse_dec("11111111111111111111111111111111");
  z.add_mul(x, y);
  os << x.to_dec_string() << " * " << y.to_dec_string() << " = " << z.to_dec_string() << std::endl;
  test_pow2(0);
  test_pow2(1);
  test_pow2(54);
  test_pow2(55);
  test_pow2(56);
  test_pow2(57);
  test_pow2(4 * 56 - 2);
  test_pow2(4 * 56 - 1);
  test_pow2(4 * 56);
  test_pow2(4 * 56 + 1);
  test_pow2(255);
  test_pow2(256);
  test_fits("1111111111111111111111111111"_i256);
  test_fits(
      "0000000000000000000000000000000000000000000000000000000000000000000000000000000000ffffffffffffffffffffffffffff"_x256);
  for (int i = 10; i >= -10; --i) {
    test_fits(td::BigInt256(i));
  }
  test_export_int("10000000000000000000000000000000000000"_i256);
  for (int k = 127; k <= 129; k++) {
    x.set_pow2(k).add_tiny(-1);
    test_export_int(x, true);
    test_export_int(x, false);
    x.add_tiny(1);
    test_export_int(x, true);
    test_export_int(x, false);
    x.add_tiny(1);
    test_export_int(x, true);
    test_export_int(x, false);
    x.negate();
    test_export_int(x, true);
    test_export_int(x, false);
    x.add_tiny(1);
    test_export_int(x, true);
    test_export_int(x, false);
    x.add_tiny(1);
    test_export_int(x, true);
    test_export_int(x, false);
  }
  for (x = 1, y.set_pow2(256).divmod_tiny(3); x.cmp(y) < 0; x.mul_tiny(3).normalize()) {
    test_export_int(x, true);
    x.negate();
    test_export_int(x, true);
    x.negate();
  }
  test_export_int("7fffffffffffffffffffffffffffffff"_x256);
  test_export_int("ffffffffffffffffffffffffffffffff"_x256);
  test_export_int("7fffffffffffffffffffffffffffffff"_x256);
  test_export_int("ffffffffffffffffffffffffffffffff"_x256);
  for (int i = 0; i <= 257; i++) {
    x.set_pow2(i).add_tiny(-3);
    for (int j = -3; j <= 3; j++) {
      x.negate().normalize();
      os << "-2^" << i << "+" << -j << ": ";
      test_fits(x);
      x.negate().normalize();
      os << "2^" << i << "+" << j << ": ";
      test_fits(x);
      x.add_tiny(1);
    }
  }

  for (auto t : {"fffffffffffffffffffffffffffffffffffffff"_x256, td::BigInt256{-1},
                 "123456789abcdef0123456789abcdef0123456789abcdef"_x256, "-8000000000000000000000000001"_x256}) {
    for (int i = 0; i <= 256; i++) {
      (x = t).mod_pow2(i).dump(os);
      os << "mod 2^" << i << " : " << x.to_hex_string() << std::endl;
    }
  }

  test_divmod(x.set_pow2(224), "10000000000000"_i256);
  test_divmod(x.set_pow2(256), "100000000000000000000000000000000000000000"_i256);
  test_divmod(x.set_pow2(256), "100000000000000000000000000000000000000000000"_i256);
  test_divmod(x.set_pow2(80), "-100000000000000000000000000000000000000000000"_i256);
  test_divmod(x.set_pow2(256), y.set_pow2(128).add_tiny(-1));
  test_divmod(x.set_pow2(224), y.set_pow2(112).add_tiny(-1));
  test_divmod(x.set_pow2(222), y.set_pow2(111).add_tiny(-1));
  test_divmod(td::BigInt256(-1), y.set_pow2(256));
  test_divmod("10000000000000000000000000000000000000000000000000000000000000000"_i256,
              "142857142857142857142857142857142857"_i256);
  test_divmod("100000000"_i256, "-253"_i256);
  test_divmod("-100000000"_i256, "-253"_i256);
  test_divmod("-100000000"_i256, "253"_i256);

  test_divmod(x.set_pow2(222), td::BigInt256{std::numeric_limits<td::BigInt256::word_t>::min()});
  test_divmod(x.set_pow2(222).negate(), td::BigInt256{std::numeric_limits<td::BigInt256::word_t>::min()});
  REGRESSION_VERIFY(os.str());
}

TEST(RefInt, main) {
  os = create_ss();
  using namespace td::literals;
  auto x = "10000000000000000000000"_ri256;
  td::RefInt256 y{true, -239}, z{false};
  auto v = x + y;
  os << x << " + " << y << " = " << x + y << std::endl;
  os << x << " - " << y << " = " << x - y << std::endl;
  os << x << " * " << y << " = " << x * y << std::endl;
  os << x << " / " << y << " = " << x / y << std::endl;
  os << x << " % " << y << " = " << x % y << std::endl;
  os << x << " + " << y << " = " << x + y << std::endl;
  os << "10000000000000000000000000000000000000000"_ri256 / "27182818284590"_ri256 << std::endl;
  {
    auto w(x + y);
    z = w;
  }
  os << "(x-y)*(x+y) = " << (x - y) * (x + y) << std::endl;
  os << "z = " << z << std::endl;
  z = x;
  x += y;
  os << "new x = " << x << " = 0x" << hex_string(x) << std::endl;
  os << "z = (old x) = " << std::move(z) << std::endl;
  os << "x + y = " << std::move(x) + std::move(y) << std::endl;
  z = "10000000000000000000000000000000000000000000000000000000000000000000000"_ri256;
  //z = td::RefInt256{true}
  //z.unique_write()->set_pow2(256);
  x = td::RefInt256{true, 0};
  int i = 1;
  while (z->sgn() > 0) {
    x += z;
    z.write().add_tiny(i >> 1).divmod_tiny(i);
    ++i;
  }
  x.write().normalize();
  os << x << " = " << hex_string(x) << std::endl;
  REGRESSION_VERIFY(os.str());
}

TEST(crc16, main) {
  os = create_ss();
  std::string s = "EMSI_FCK";
  unsigned crc16 = td::crc16(td::Slice{s});
  os << "s = `" << s << "`; crc16 = " << std::hex << crc16 << std::dec << std::endl;
  REGRESSION_VERIFY(os.str());
}

TEST(base64, main) {
  os = create_ss();
  std::vector<std::string> arr = {"TEST STRING NUMBER ONE", "TEST STRING NUMBER FOUR", "TEST STRING NUMBER THREE"};
  for (std::string s : arr) {
    std::string t = td::str_base64_encode(s);
    std::string u = td::str_base64_decode(t);
    os << "`" << s << "` -> `" << t << "` -> `" << u << "`" << std::endl;
    os << (s == u) << std::endl;
  }
  std::string s;
  int k = 0;
  for (int i = 0; i < 1024; i++) {
    s.push_back((char)(k >> 8));
    k = 69069 * k + 1;
  }
  std::string t = td::str_base64_encode(s);
  std::string u = td::str_base64_decode(t, true);
  os << t << std::endl;
  os << (s == u) << std::endl;
  t = td::str_base64_encode(s, true);
  u = td::str_base64_decode(t, true);
  os << t << std::endl;
  os << (s == u) << std::endl;
  u = td::sha256(td::Slice{s});
  for (int i = 0; i < 32; i++) {
    os << std::hex << ((u[i] >> 4) & 15) << (u[i] & 15);
  }
  os << std::dec << std::endl;
  REGRESSION_VERIFY(os.str());
}

void check_bits256_scan(std::ostream& stream, td::Bits256 a, td::Bits256 b) {
  auto c = a ^ b;
  auto bit = c.count_leading_zeroes();
  auto bit2 = a.count_matching(b);
  // stream << a.to_hex() << " and " << b.to_hex() << " match in " << bit << " or " << bit2 << " first bits" << std::endl;
  // std::cerr << a.to_hex() << " and " << b.to_hex() << " match in " << bit << " or " << bit2 << " first bits (a XOR b = " << c.to_hex() << ")" << std::endl;
  CHECK((int)bit >= 0 && bit <= 256);
  for (td::uint32 i = 0; i < bit; i++) {
    CHECK(a[i] == b[i]);
  }
  CHECK(bit == 256 || a[bit] != b[bit]);
  CHECK(bit == bit2);
}

void check_bits_scan(std::ostream& stream, td::ConstBitPtr a, bool value) {
  auto bit = (unsigned)a.scan(value, 256);
  CHECK((int)bit >= 0 && bit <= 256);
  for (td::uint32 i = 0; i < bit; i++) {
    CHECK(a[i] == value);
  }
  CHECK(bit == 256 || a[bit] != value);
}

TEST(bits256_scan, main) {
  os = create_ss();
  td::Bits256 a, b;
  int k = 0;
  unsigned char r[1024];
  for (auto& c : r) {
    c = (k & 0x80) ? (unsigned char)(k >> 8) : 0;
    k = 69069 * k + 1;
  }
  for (k = 0; k < 32; k++) {
    a = td::ConstBitPtr{r + 32 * k};
    for (int j = 0; j < 32; j++) {
      b = td::ConstBitPtr{r + 32 * j};
      check_bits256_scan(os, a, b);
    }
    b = a;
    unsigned i = r[7 + k];
    b[i] = b[i] ^ true;
    check_bits256_scan(os, a, b);
  }
  for (k = 0; k < 256; k++) {
    check_bits_scan(os, td::ConstBitPtr{r} + k, false);
    check_bits_scan(os, td::ConstBitPtr{r} + k, true);
  }
  os << "bits256_scan test OK";
  REGRESSION_VERIFY(os.str());
}

bool check_exp(std::ostream& stream, const td::NegExpBinTable& tab, double x) {
  long long xx = llround(x * (1LL << 52));
  td::BigInt256 yy;
  if (!tab.nexpf(yy, -xx, 52)) {
    stream << "cannot compute exp(" << x << ") = exp(" << xx << " * 2^(-52))" << std::endl;
    return false;
  }
  double y = yy.to_double() * exp2(-252);
  double y0 = exp(x);
  bool ok = (fabs(y - y0) < 1e-15);
  if (!ok) {
    stream << "exp(" << x << ") = exp(" << xx << " * 2^(-52)) = " << yy << " / 2^252 = " << y << " (correct value is "
           << y0 << ") " << (ok ? "match" : "incorrect") << std::endl;
  }
  return ok;
}

TEST(bigexp, main) {
  os = create_ss();
  td::NegExpBinTable tab(252, 32, -128);
  bool ok = true;
  if (!tab.is_valid()) {
    os << "cannot initialize td::NegExpBinTable(252, 32, -128)" << std::endl;
    ok = false;
  } else {
    // for (int i = -128; i < 32; i++) {
    //  os << "exp(-2^" << i << ") = " << tab.exp_pw2_ref(i) << " / 2^252 = " << tab.exp_pw2_ref(i)->to_double() * exp2(-252) << " (correct value is " << exp(-exp2(i)) << ")" << std::endl;
    // }
    ok &= check_exp(os, tab, -2.39);
    ok &= check_exp(os, tab, 0);
    ok &= check_exp(os, tab, -1);
    ok &= check_exp(os, tab, -2);
    ok &= check_exp(os, tab, -16);
    ok &= check_exp(os, tab, -17);
    ok &= check_exp(os, tab, -0.5);
    ok &= check_exp(os, tab, -0.25);
    ok &= check_exp(os, tab, -3.1415926535);
    ok &= check_exp(os, tab, -1e-9);
  }
  if (ok) {
    os << "bigexp test OK\n";
  } else {
    os << "bigexp test FAILED\n";
  }
  REGRESSION_VERIFY(os.str());
}

bool check_intexp(std::ostream& stream, td::uint64 x, unsigned k, td::uint64 yc = 0) {
  td::uint64 y = td::umulnexps32(x, k);
  long long delta = (long long)(y - yc);
  bool ok = (y <= x && std::abs(delta) <= 1);
  if (!ok) {
    stream << x << "*exp(-" << k << "/65536) = " << y << " (correct value " << yc << ", delta = " << delta << ")"
           << std::endl;
  }
  return ok;
}

TEST(uint64_exp, main) {
  os = create_ss();
  bool ok = true;
  ok &= check_intexp(os, 3167801306015831286, 4003, 2980099890648636481);
  ok &= check_intexp(os, 1583900653007915643, 4003, 1490049945324318240);
  ok &= check_intexp(os, 9094494907266047891, 17239, 6990995826652297465);
  ok &= check_intexp(os, 5487867407433215099, 239017, 143048684491504152);
  ok &= check_intexp(os, 46462010749955243, 239017, 1211095134625318);  // up
  ok &= check_intexp(os, 390263500024095125, 2700001, 1);
  ok &= check_intexp(os, 390263500024095124, 2700001, 1);
  ok &= check_intexp(os, std::numeric_limits<td::uint64>::max(), 2952601, 1);
  ok &= check_intexp(os, std::numeric_limits<td::uint64>::max(), 2952696, 1);
  ok &= check_intexp(os, std::numeric_limits<td::uint64>::max(), 2952697, 0);
  ok &= check_intexp(os, std::numeric_limits<td::uint64>::max(), 2952800, 0);
  ok &= check_intexp(os, std::numeric_limits<td::uint64>::max(), 295269700, 0);
  ok &= check_intexp(os, std::numeric_limits<td::uint64>::max(), 2000018, 1028453);
  ok &= check_intexp(os, 1ULL << 60, 2770991, 1);
  ok &= check_intexp(os, 1ULL << 60, 2770992, 0);
  if (ok) {
    os << "uint64_exp test OK\n";
  } else {
    os << "uint64_exp test FAILED\n";
  }
  REGRESSION_VERIFY(os.str());
}
