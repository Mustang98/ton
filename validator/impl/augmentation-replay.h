#pragma once

#include "block/block-auto.h"
#include "vm/cells/Cell.h"

namespace ton::validator::detail {

// Records the concrete dictionary-wrapper occurrences checked by the generated
// Block validator. A replay is eligible only when all three expected calls
// happened exactly once and the handwritten pass sees the same objects.
class GeneratedAugmentationCertificate {
 public:
  bool is_target(const tlb::TLB* type) const {
    return find(type) != nullptr;
  }

  void record(const tlb::TLB* type, const td::Ref<vm::Cell>& cell) {
    auto* entry = find(type);
    CHECK(entry != nullptr);
    ++entry->count;
    if (entry->count == 1) {
      entry->cell = cell;
    }
  }

  bool complete() const {
    return valid(in_msg_descr_) && valid(out_msg_descr_) && valid(shard_account_blocks_);
  }

  bool matches(const td::Ref<vm::Cell>& in_msg_descr, const td::Ref<vm::Cell>& out_msg_descr,
               const td::Ref<vm::Cell>& shard_account_blocks) const {
    return complete() && in_msg_descr.not_null() && out_msg_descr.not_null() && shard_account_blocks.not_null() &&
           in_msg_descr_.cell.get() == in_msg_descr.get() && out_msg_descr_.cell.get() == out_msg_descr.get() &&
           shard_account_blocks_.cell.get() == shard_account_blocks.get();
  }

 private:
  struct Entry {
    td::Ref<vm::Cell> cell;
    unsigned count{0};
  };

  static bool valid(const Entry& entry) {
    return entry.count == 1 && entry.cell.not_null();
  }

  Entry* find(const tlb::TLB* type) {
    if (type == &block::gen::t_InMsgDescr) {
      return &in_msg_descr_;
    }
    if (type == &block::gen::t_OutMsgDescr) {
      return &out_msg_descr_;
    }
    if (type == &block::gen::t_ShardAccountBlocks) {
      return &shard_account_blocks_;
    }
    return nullptr;
  }

  const Entry* find(const tlb::TLB* type) const {
    return const_cast<GeneratedAugmentationCertificate*>(this)->find(type);
  }

  Entry in_msg_descr_;
  Entry out_msg_descr_;
  Entry shard_account_blocks_;
};

}  // namespace ton::validator::detail
