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
#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "common/refcnt.hpp"
#include "ton/ton-types.h"
#include "vm/cells/Cell.h"
#include "vm/cells/CellSlice.h"

namespace ton::validator::detail {

struct AccountBlockSnapshotEntry {
  StdSmcAddress account;
  td::Ref<vm::CellSlice> value;
};

struct AccountBlockSnapshot {
  // Root identity is intentionally occurrence-based, not hash-based. Snap,
  // UsageCell, and loaded DataCell occurrences may share a hash while carrying
  // different access context.
  td::Ref<vm::Cell> wrapped_root;
  td::Ref<vm::Cell> inner_root;
  std::vector<AccountBlockSnapshotEntry> entries;
};

class AccountBlockSnapshotBuilder {
 public:
  AccountBlockSnapshotBuilder(td::Ref<vm::Cell> wrapped_root, td::Ref<vm::Cell> inner_root) {
    snapshot_.wrapped_root = std::move(wrapped_root);
    snapshot_.inner_root = std::move(inner_root);
    valid_ = snapshot_.wrapped_root.not_null();
  }

  bool append(td::Ref<vm::CellSlice> value, td::ConstBitPtr key, int key_len) {
    if (!valid_ || key_len != 256 || value.is_null()) {
      valid_ = false;
      return false;
    }
    StdSmcAddress account = key;
    if (!snapshot_.entries.empty() && !(snapshot_.entries.back().account < account)) {
      valid_ = false;
      return false;
    }
    snapshot_.entries.push_back(AccountBlockSnapshotEntry{std::move(account), std::move(value)});
    return true;
  }

  std::shared_ptr<const AccountBlockSnapshot> finish(bool traversal_complete) && {
    if (!valid_ || !traversal_complete) {
      return {};
    }
    return std::make_shared<const AccountBlockSnapshot>(std::move(snapshot_));
  }

 private:
  AccountBlockSnapshot snapshot_;
  bool valid_{false};
};

inline bool account_block_snapshot_matches(const AccountBlockSnapshot& snapshot, const td::Ref<vm::Cell>& wrapped_root,
                                           const td::Ref<vm::Cell>& inner_root) {
  return wrapped_root.not_null() && snapshot.wrapped_root.get() == wrapped_root.get() &&
         snapshot.inner_root.get() == inner_root.get();
}

inline td::Ref<vm::CellSlice> take_ordered_account_block(const AccountBlockSnapshot& snapshot, std::size_t& position,
                                                         const StdSmcAddress& account) {
  while (position < snapshot.entries.size() && snapshot.entries[position].account < account) {
    ++position;
  }
  if (position == snapshot.entries.size() || snapshot.entries[position].account != account) {
    return {};
  }
  // CellSlice is copy-on-write. The immutable snapshot keeps its cursor while
  // each consumer advances an independently owned copy of the same occurrence.
  return snapshot.entries[position++].value;
}

}  // namespace ton::validator::detail
