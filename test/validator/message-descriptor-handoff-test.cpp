#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "td/utils/tests.h"
#include "vm/boc.h"
#include "vm/cells/UsageCell.h"

#include "message-descriptor-handoff.h"

namespace {

struct TransactionEntry {
  td::Ref<vm::Cell> root;
  td::Ref<vm::Cell> in_message;
  std::size_t out_messages_begin{0};
  std::size_t out_messages_end{0};
};

struct TransactionPlan {
  using value_type = TransactionEntry;

  std::vector<TransactionEntry> records;
  std::vector<td::Ref<vm::Cell>> out_messages;
  std::unordered_map<const vm::Cell*, std::size_t> exact_root_index;
};

TEST(MessageDescriptorHandoff, CapturesOnlyExactUniqueCompletePlan) {
  auto transaction = vm::CellBuilder{}.store_long(1, 1).finalize();
  auto input = vm::CellBuilder{}.store_long(2, 2).finalize();
  auto output0 = vm::CellBuilder{}.store_long(3, 2).finalize();
  auto output1 = vm::CellBuilder{}.store_long(1, 2).finalize();
  TransactionPlan transactions;
  transactions.records.push_back({transaction, input, 0, 2});
  transactions.out_messages = {output0, output1};
  transactions.exact_root_index.emplace(transaction.get(), 0);

  auto descriptor_child = vm::CellBuilder{}.store_long(0x5a, 8).finalize();
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto descriptor = vm::UsageCell::create(vm::CellBuilder{}.store_long(5, 3).store_ref(descriptor_child).finalize(),
                                          usage_tree->root_ptr());
  auto original = vm::load_cell_slice(descriptor);
  original.advance(2);
  const auto original_bits = original.size();
  const auto original_refs = original.size_refs();

  std::vector<std::optional<vm::CellSlice>> in_cursors(1);
  std::vector<std::optional<vm::CellSlice>> out_cursors(2);
  std::vector<std::optional<vm::CellSlice>> half_link_in(1);
  std::vector<std::optional<vm::CellSlice>> half_link_out(2);
  ASSERT_TRUE(!ton::validator::detail::capture_in_message_descriptor_cursor(transactions, {transaction, {}},
                                                                            vm::CellSlice{original}, half_link_in));
  ASSERT_TRUE(!ton::validator::detail::capture_out_message_descriptor_cursor(transactions, {transaction, {}},
                                                                             vm::CellSlice{original}, half_link_out));
  ton::validator::detail::MessageDescriptorLink input_link{transaction, input};
  ASSERT_TRUE(ton::validator::detail::capture_in_message_descriptor_cursor(transactions, input_link,
                                                                           vm::CellSlice{original}, in_cursors));
  ASSERT_EQ(in_cursors[0]->size(), original_bits);
  ASSERT_EQ(in_cursors[0]->size_refs(), original_refs);
  ASSERT_TRUE(in_cursors[0]->prefetch_ref()->get_tree_node().is_from_tree(usage_tree.get()));
  ASSERT_TRUE(!ton::validator::detail::capture_in_message_descriptor_cursor(transactions, input_link,
                                                                            vm::CellSlice{original}, in_cursors));

  ASSERT_TRUE(ton::validator::detail::capture_out_message_descriptor_cursor(transactions, {transaction, output0},
                                                                            vm::CellSlice{original}, out_cursors));
  ASSERT_TRUE(!ton::validator::detail::message_descriptor_cursor_slots_complete(transactions, in_cursors, out_cursors));
  ASSERT_TRUE(ton::validator::detail::capture_out_message_descriptor_cursor(transactions, {transaction, output1},
                                                                            vm::CellSlice{original}, out_cursors));
  ASSERT_TRUE(ton::validator::detail::message_descriptor_cursor_slots_complete(transactions, in_cursors, out_cursors));
  ASSERT_TRUE(!ton::validator::detail::capture_out_message_descriptor_cursor(transactions, {transaction, output0},
                                                                             vm::CellSlice{original}, out_cursors));

  std::vector<std::optional<vm::CellSlice>> no_input_cursors(1);
  TransactionPlan no_input = transactions;
  no_input.records[0].in_message.clear();
  ASSERT_TRUE(
      ton::validator::detail::message_descriptor_cursor_slots_complete(no_input, no_input_cursors, out_cursors));
}

TEST(MessageDescriptorHandoff, PreflightIsWholeTransactionAndPointerExact) {
  auto transaction = vm::CellBuilder{}.store_long(1, 1).finalize();
  auto input = vm::CellBuilder{}.store_long(2, 2).finalize();
  auto output0 = vm::CellBuilder{}.store_long(3, 2).finalize();
  auto output1 = vm::CellBuilder{}.store_long(1, 2).finalize();
  auto cursor = vm::load_cell_slice(vm::CellBuilder{}.store_long(7, 3).finalize());

  TransactionPlan transactions;
  transactions.records.push_back({transaction, input, 0, 2});
  transactions.out_messages = {output0, output1};
  transactions.exact_root_index.emplace(transaction.get(), 0);
  std::vector<std::optional<vm::CellSlice>> in_cursors(1, cursor);
  std::vector<std::optional<vm::CellSlice>> out_cursors(2, cursor);

  std::size_t index = std::numeric_limits<std::size_t>::max();
  ASSERT_TRUE(ton::validator::detail::preflight_message_descriptor_transaction(
      transactions, in_cursors, out_cursors, &transactions.records[0], transaction, input, index));
  ASSERT_EQ(index, 0u);

  auto transaction_boc = vm::std_boc_serialize(transaction, 31).move_as_ok();
  auto same_hash_transaction = vm::std_boc_deserialize(transaction_boc.as_slice()).move_as_ok();
  ASSERT_EQ(same_hash_transaction->get_hash(), transaction->get_hash());
  ASSERT_TRUE(same_hash_transaction.get() != transaction.get());
  ASSERT_TRUE(!ton::validator::detail::preflight_message_descriptor_transaction(
      transactions, in_cursors, out_cursors, &transactions.records[0], same_hash_transaction, input, index));

  auto input_boc = vm::std_boc_serialize(input, 31).move_as_ok();
  auto same_hash_input = vm::std_boc_deserialize(input_boc.as_slice()).move_as_ok();
  ASSERT_EQ(same_hash_input->get_hash(), input->get_hash());
  ASSERT_TRUE(same_hash_input.get() != input.get());
  ASSERT_TRUE(!ton::validator::detail::preflight_message_descriptor_transaction(
      transactions, in_cursors, out_cursors, &transactions.records[0], transaction, same_hash_input, index));

  out_cursors[1].reset();
  ASSERT_TRUE(!ton::validator::detail::preflight_message_descriptor_transaction(
      transactions, in_cursors, out_cursors, &transactions.records[0], transaction, input, index));
}

TEST(MessageDescriptorHandoff, RejectsDistinctUsageAndVirtualOccurrences) {
  auto base_transaction = vm::CellBuilder{}.store_long(1, 1).finalize();
  auto input = vm::CellBuilder{}.store_long(2, 2).finalize();
  auto first_tree = std::make_shared<vm::CellUsageTree>();
  auto second_tree = std::make_shared<vm::CellUsageTree>();
  auto first_usage = vm::UsageCell::create(base_transaction, first_tree->root_ptr());
  auto second_usage = vm::UsageCell::create(base_transaction, second_tree->root_ptr());
  ASSERT_EQ(first_usage->get_hash(), second_usage->get_hash());
  ASSERT_TRUE(first_usage.get() != second_usage.get());

  auto cursor = vm::load_cell_slice(vm::CellBuilder{}.store_long(7, 3).finalize());
  TransactionPlan usage_transactions;
  usage_transactions.records.push_back({first_usage, input, 0, 0});
  usage_transactions.exact_root_index.emplace(first_usage.get(), 0);
  std::vector<std::optional<vm::CellSlice>> usage_in(1, cursor);
  std::vector<std::optional<vm::CellSlice>> no_outputs;
  std::size_t index = 0;
  ASSERT_TRUE(ton::validator::detail::preflight_message_descriptor_transaction(
      usage_transactions, usage_in, no_outputs, &usage_transactions.records[0], first_usage, input, index));
  ASSERT_TRUE(!ton::validator::detail::preflight_message_descriptor_transaction(
      usage_transactions, usage_in, no_outputs, &usage_transactions.records[0], second_usage, input, index));

  auto leaf = vm::CellBuilder{}.store_long(1, 1).finalize();
  auto pruned_source = vm::CellBuilder{}.store_ref(leaf).finalize();
  auto pruned = vm::CellBuilder::create_pruned_branch(pruned_source, 1);
  auto levelled = vm::CellBuilder{}.store_ref(pruned).finalize();
  ASSERT_TRUE(levelled->get_level() > 0);
  auto first_virtual = levelled->virtualize(0);
  auto second_virtual = levelled->virtualize(0);
  ASSERT_TRUE(first_virtual->is_virtualized());
  ASSERT_EQ(first_virtual->get_hash(), second_virtual->get_hash());
  ASSERT_TRUE(first_virtual.get() != second_virtual.get());

  TransactionPlan virtual_transactions;
  virtual_transactions.records.push_back({first_virtual, input, 0, 0});
  virtual_transactions.exact_root_index.emplace(first_virtual.get(), 0);
  std::vector<std::optional<vm::CellSlice>> virtual_in(1, cursor);
  ASSERT_TRUE(ton::validator::detail::preflight_message_descriptor_transaction(
      virtual_transactions, virtual_in, no_outputs, &virtual_transactions.records[0], first_virtual, input, index));
  ASSERT_TRUE(!ton::validator::detail::preflight_message_descriptor_transaction(
      virtual_transactions, virtual_in, no_outputs, &virtual_transactions.records[0], second_virtual, input, index));
}

TEST(MessageDescriptorHandoff, RootsRequireBothConcreteIdentityLayers) {
  auto in_wrapped = vm::CellBuilder{}.store_long(1, 1).finalize();
  auto in_inner = vm::CellBuilder{}.store_long(2, 2).finalize();
  auto out_wrapped = vm::CellBuilder{}.store_long(3, 2).finalize();
  auto out_inner = vm::CellBuilder{}.store_long(1, 2).finalize();
  ton::validator::detail::MessageDescriptorRoots roots{in_wrapped, in_inner, out_wrapped, out_inner};
  ASSERT_TRUE(
      ton::validator::detail::message_descriptor_roots_match(roots, in_wrapped, in_inner, out_wrapped, out_inner));

  auto in_wrapped_boc = vm::std_boc_serialize(in_wrapped, 31).move_as_ok();
  auto same_hash_in_wrapped = vm::std_boc_deserialize(in_wrapped_boc.as_slice()).move_as_ok();
  ASSERT_EQ(same_hash_in_wrapped->get_hash(), in_wrapped->get_hash());
  ASSERT_TRUE(same_hash_in_wrapped.get() != in_wrapped.get());
  ASSERT_TRUE(!ton::validator::detail::message_descriptor_roots_match(roots, same_hash_in_wrapped, in_inner,
                                                                      out_wrapped, out_inner));
}

}  // namespace
