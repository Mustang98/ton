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

#include <memory>
#include <utility>
#include <vector>

#include "td/utils/tests.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/CellUsageTree.h"
#include "vm/cells/UsageCell.h"

#include "transaction-record-handoff.h"

namespace {

struct Record {
  ton::StdSmcAddress account;
  ton::LogicalTime lt{0};
  td::Ref<vm::Cell> root;
};

using Plan = std::vector<Record>;

struct UsageFixture {
  std::shared_ptr<vm::CellUsageTree> tree;
  td::Ref<vm::Cell> root;
};

ton::StdSmcAddress make_address(unsigned char suffix) {
  ton::StdSmcAddress address;
  address.set_zero();
  address.as_array().back() = suffix;
  return address;
}

UsageFixture wrap_with_usage(const td::Ref<vm::Cell>& concrete) {
  auto tree = std::make_shared<vm::CellUsageTree>();
  return {tree, vm::UsageCell::create(concrete, tree->root_ptr())};
}

struct ActorShapedContext {
  std::shared_ptr<const Plan> plan;
  bool available{false};
  std::size_t position{0};
  std::size_t end{0};
};

ActorShapedContext take_range(std::shared_ptr<const Plan> plan, std::size_t& position,
                              const ton::StdSmcAddress& address) {
  ActorShapedContext context;
  if (plan && ton::validator::detail::try_take_ordered_transaction_range(*plan, position, address, context.available,
                                                                         context.position, context.end)) {
    context.plan = std::move(plan);
  }
  return context;
}

const Record* take_record(ActorShapedContext& context, const ton::StdSmcAddress& address, ton::LogicalTime lt) {
  if (!context.plan) {
    return nullptr;
  }
  return ton::validator::detail::try_take_ordered_transaction(*context.plan, context.available, context.position,
                                                              context.end, address, lt);
}

TEST(TransactionRecordHandoff, PublicationRequiresStrictAccountAndLtOrder) {
  auto root = vm::CellBuilder{}.store_long(1, 1).finalize();
  auto address1 = make_address(1);
  auto address2 = make_address(2);

  Plan ordered{{address1, 10, root}, {address1, 20, root}, {address2, 1, root}};
  ASSERT_TRUE(ton::validator::detail::transaction_record_plan_is_strictly_ordered(ordered));

  auto duplicate = ordered;
  duplicate[1].lt = duplicate[0].lt;
  ASSERT_TRUE(!ton::validator::detail::transaction_record_plan_is_strictly_ordered(duplicate));

  auto reversed = ordered;
  std::swap(reversed[0], reversed[2]);
  ASSERT_TRUE(!ton::validator::detail::transaction_record_plan_is_strictly_ordered(reversed));

  auto null_root = ordered;
  null_root[1].root.clear();
  ASSERT_TRUE(!ton::validator::detail::transaction_record_plan_is_strictly_ordered(null_root));
}

TEST(TransactionRecordHandoff, RangeAndRecordMismatchDisableOnlyTheCandidatePath) {
  auto root = vm::CellBuilder{}.store_long(1, 1).finalize();
  auto address1 = make_address(1);
  auto address2 = make_address(2);
  auto address3 = make_address(3);
  std::shared_ptr<const Plan> plan =
      std::make_shared<Plan>(Plan{{address1, 10, root}, {address1, 20, root}, {address3, 30, root}});

  std::size_t plan_position = 0;
  auto first = take_range(plan, plan_position, address1);
  ASSERT_TRUE(first.available);
  ASSERT_EQ(first.position, 0u);
  ASSERT_EQ(first.end, 2u);
  ASSERT_EQ(plan_position, 2u);
  ASSERT_TRUE(take_record(first, address1, 10) != nullptr);

  // An lt mismatch cannot be resynchronized to a later record. Production
  // consequently unpacks this and all remaining leaves in this account.
  ASSERT_TRUE(take_record(first, address1, 21) == nullptr);
  ASSERT_TRUE(!first.available);
  ASSERT_TRUE(take_record(first, address1, 20) == nullptr);

  // A missing account leaves the serial cursor in place, so a later exact
  // account can still obtain its independent range.
  auto missing = take_range(plan, plan_position, address2);
  ASSERT_TRUE(!missing.available);
  ASSERT_EQ(plan_position, 2u);
  auto third = take_range(plan, plan_position, address3);
  ASSERT_TRUE(third.available);
  ASSERT_TRUE(take_record(third, address3, 30) != nullptr);
}

TEST(TransactionRecordHandoff, ReplayRequiresExactAddressLtAndConcreteRoot) {
  auto concrete = vm::CellBuilder{}.store_long(5, 3).finalize();
  auto first = wrap_with_usage(concrete);
  auto same_hash = wrap_with_usage(concrete);
  ASSERT_EQ(first.root->get_hash(), same_hash.root->get_hash());
  ASSERT_TRUE(first.root.get() != same_hash.root.get());

  auto address = make_address(1);
  Record record{address, 10, first.root};
  ASSERT_TRUE(
      ton::validator::detail::prechecked_transaction_matches_current_occurrence(&record, address, 10, first.root));
  ASSERT_TRUE(!ton::validator::detail::prechecked_transaction_matches_current_occurrence(&record, make_address(2), 10,
                                                                                         first.root));
  ASSERT_TRUE(
      !ton::validator::detail::prechecked_transaction_matches_current_occurrence(&record, address, 11, first.root));
  // Same representation hash, different UsageCell occurrence: force legacy
  // parsing of the current leaf instead of replaying the retained record.
  ASSERT_TRUE(
      !ton::validator::detail::prechecked_transaction_matches_current_occurrence(&record, address, 10, same_hash.root));
  ASSERT_TRUE(!ton::validator::detail::prechecked_transaction_matches_current_occurrence<Record>(nullptr, address, 10,
                                                                                                 first.root));
}

TEST(TransactionRecordHandoff, SharedPlanKeepsExactUsageOccurrencesAliveAcrossActorMoves) {
  auto concrete = vm::CellBuilder{}.store_long(5, 3).finalize();
  auto first = wrap_with_usage(concrete);
  auto second = wrap_with_usage(concrete);
  auto address1 = make_address(1);
  auto address2 = make_address(2);

  std::shared_ptr<const Plan> plan =
      std::make_shared<Plan>(Plan{{address1, 10, first.root}, {address2, 20, second.root}});
  std::weak_ptr<const Plan> lifetime = plan;
  std::size_t position = 0;
  auto first_actor = take_range(plan, position, address1);
  auto second_actor = take_range(plan, position, address2);
  plan.reset();
  ASSERT_TRUE(!lifetime.expired());

  // Parallel actors may finish in either order. Each owns its exact UsageCell
  // wrapper and therefore preserves the correct NodePtr while the tree lives.
  auto* second_record = take_record(second_actor, address2, 20);
  ASSERT_TRUE(second_record != nullptr);
  ASSERT_TRUE(second_record->root.get() == second.root.get());
  ASSERT_TRUE(second_record->root->get_tree_node().is_from_tree(second.tree.get()));
  ASSERT_TRUE(!second_record->root->get_tree_node().is_from_tree(first.tree.get()));

  auto* first_record = take_record(first_actor, address1, 10);
  ASSERT_TRUE(first_record != nullptr);
  ASSERT_TRUE(first_record->root.get() == first.root.get());
  ASSERT_TRUE(first_record->root->get_tree_node().is_from_tree(first.tree.get()));
  ASSERT_TRUE(!first_record->root->get_tree_node().is_from_tree(second.tree.get()));
}

}  // namespace
