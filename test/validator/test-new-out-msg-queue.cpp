#include <queue>
#include <utility>
#include <vector>

#include "td/utils/tests.h"
#include "vm/cells/CellBuilder.h"

#include "collator-impl.h"

namespace ton::validator {
namespace {

using LegacyNewOutMsgQueue =
    std::priority_queue<block::NewOutMsg, std::vector<block::NewOutMsg>, std::greater<block::NewOutMsg>>;

td::Ref<vm::Cell> make_cell(td::uint64 value) {
  return vm::CellBuilder{}.store_long(static_cast<long long>(value), 64).finalize_novm();
}

block::NewOutMsg make_message(LogicalTime lt, td::uint64 tag, unsigned msg_idx) {
  return block::NewOutMsg{lt, make_cell(tag), make_cell(tag ^ 0x9e3779b97f4a7c15ULL), msg_idx};
}

void assert_same_message(const block::NewOutMsg& lhs, const block::NewOutMsg& rhs) {
  ASSERT_EQ(lhs.lt, rhs.lt);
  ASSERT_EQ(lhs.msg_idx, rhs.msg_idx);
  ASSERT_TRUE(lhs.msg->get_hash() == rhs.msg->get_hash());
  ASSERT_TRUE(lhs.trans->get_hash() == rhs.trans->get_hash());
}

class HashFailure {};

class ThrowingHashCell final : public vm::Cell {
 public:
  ThrowingHashCell(td::Ref<vm::Cell> cell, const bool* fail) : cell_(std::move(cell)), fail_(fail) {
  }

  td::Status set_data_cell(td::Ref<vm::DataCell>&& data_cell) const override {
    return cell_->set_data_cell(std::move(data_cell));
  }

  td::Result<LoadedCell> load_cell() const override {
    return cell_->load_cell();
  }

  td::Ref<Cell> virtualize(td::uint32 effective_level) const override {
    return cell_->virtualize(effective_level);
  }

  bool is_virtualized() const override {
    return cell_->is_virtualized();
  }

  vm::CellUsageTree::NodePtr get_tree_node() const override {
    return cell_->get_tree_node();
  }

  bool is_loaded() const override {
    return cell_->is_loaded();
  }

  LevelMask get_level_mask() const override {
    return cell_->get_level_mask();
  }

 private:
  td::uint16 do_get_depth(td::uint32 level) const override {
    return cell_->get_depth(level);
  }

  const Hash do_get_hash(td::uint32 level) const override {
    if (*fail_) {
      throw HashFailure{};
    }
    return cell_->get_hash(level);
  }

  td::Ref<vm::Cell> cell_;
  const bool* fail_;
};

template <class Queue>
void fill_hash_failure_queue(Queue& queue, bool& fail) {
  for (unsigned i = 0; i < 4; ++i) {
    td::Ref<vm::Cell> msg = td::Ref<ThrowingHashCell>{true, make_cell(0x100 + i), &fail};
    queue.push(block::NewOutMsg{17, std::move(msg), make_cell(0x200 + i), i});
  }
}

TEST(NewOutMsgQueue, matches_legacy_order_with_ties) {
  LegacyNewOutMsgQueue legacy;
  NewOutMsgQueue queue;

  for (unsigned i = 0; i < 256; ++i) {
    auto message = make_message((i * 37) % 23, 0x100000 + i * 7919, i);
    legacy.push(message);
    queue.push(std::move(message));
    ASSERT_TRUE(message.msg.is_null());
    ASSERT_TRUE(message.trans.is_null());
  }

  while (!legacy.empty()) {
    ASSERT_TRUE(!queue.empty());
    auto expected = legacy.top();
    legacy.pop();
    auto actual = queue.pop_move();
    assert_same_message(expected, actual);
  }
  ASSERT_TRUE(queue.empty());
}

TEST(NewOutMsgQueue, transfers_all_cell_ownership) {
  auto msg = make_cell(0x11);
  auto trans = make_cell(0x22);
  auto envelope = make_cell(0x33);
  auto msg_probe = msg;
  auto trans_probe = trans;
  auto envelope_probe = envelope;

  block::NewOutMsg source{42, std::move(msg), std::move(trans), 7};
  source.msg_env_from_dispatch_queue = std::move(envelope);

  NewOutMsgQueue queue;
  queue.push(std::move(source));
  ASSERT_TRUE(source.msg.is_null());
  ASSERT_TRUE(source.trans.is_null());
  ASSERT_TRUE(source.msg_env_from_dispatch_queue.is_null());
  ASSERT_EQ(msg_probe->get_refcnt(), 2);
  ASSERT_EQ(trans_probe->get_refcnt(), 2);
  ASSERT_EQ(envelope_probe->get_refcnt(), 2);

  {
    auto result = queue.pop_move();
    ASSERT_TRUE(queue.empty());
    ASSERT_TRUE(result.msg.get() == msg_probe.get());
    ASSERT_TRUE(result.trans.get() == trans_probe.get());
    ASSERT_TRUE(result.msg_env_from_dispatch_queue.get() == envelope_probe.get());
    ASSERT_EQ(msg_probe->get_refcnt(), 2);
    ASSERT_EQ(trans_probe->get_refcnt(), 2);
    ASSERT_EQ(envelope_probe->get_refcnt(), 2);
  }

  ASSERT_EQ(msg_probe->get_refcnt(), 1);
  ASSERT_EQ(trans_probe->get_refcnt(), 1);
  ASSERT_EQ(envelope_probe->get_refcnt(), 1);
}

TEST(NewOutMsgQueue, matches_legacy_comparator_error) {
  bool legacy_fail = false;
  bool queue_fail = false;
  LegacyNewOutMsgQueue legacy;
  NewOutMsgQueue queue;
  fill_hash_failure_queue(legacy, legacy_fail);
  fill_hash_failure_queue(queue, queue_fail);

  legacy_fail = true;
  queue_fail = true;
  bool legacy_threw = false;
  bool queue_threw = false;
  try {
    legacy.pop();
  } catch (const HashFailure&) {
    legacy_threw = true;
  }
  try {
    queue.pop_move();
  } catch (const HashFailure&) {
    queue_threw = true;
  }

  ASSERT_TRUE(legacy_threw);
  ASSERT_TRUE(queue_threw);
  ASSERT_EQ(queue.size(), legacy.size());
}

TEST(NewOutMsgQueue, empty_boundary_and_reuse) {
  NewOutMsgQueue queue;
  ASSERT_TRUE(queue.empty());
  ASSERT_EQ(queue.size(), 0u);

  for (unsigned i = 0; i < 32; ++i) {
    queue.push(make_message(i, 0x100 + i, i));
    ASSERT_TRUE(!queue.empty());
    auto result = queue.pop_move();
    ASSERT_EQ(result.lt, i);
    ASSERT_EQ(result.msg_idx, i);
    ASSERT_TRUE(queue.empty());
    ASSERT_EQ(queue.size(), 0u);
  }
}

}  // namespace
}  // namespace ton::validator
