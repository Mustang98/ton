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

#pragma once

#include <cstddef>

#include "ton/ton-types.h"

namespace ton::validator::detail {

template <class Entries, class ExactRootIndex, class RootRef>
const typename Entries::value_type* find_exact_transaction_root(const Entries& entries,
                                                                const ExactRootIndex& exact_root_index,
                                                                const RootRef& root) {
  if (root.is_null()) {
    return nullptr;
  }
  auto it = exact_root_index.find(root.get());
  if (it == exact_root_index.end() || it->second >= entries.size()) {
    return nullptr;
  }
  const auto& entry = entries[it->second];
  // A same-hash Usage/Virtual/Snap wrapper is not the occurrence that was
  // checked. Only the retained concrete transaction root can certify reuse.
  return entry.root.get() == root.get() ? &entry : nullptr;
}

template <class Entry, class FlatMessages>
const typename FlatMessages::value_type* find_prechecked_transaction_out_message(const Entry& entry,
                                                                                 const FlatMessages& messages,
                                                                                 LogicalTime created_lt) {
  if (entry.out_messages_end < entry.out_messages_begin || entry.out_messages_end > messages.size()) {
    return nullptr;
  }
  const auto outmsg_count = entry.out_messages_end - entry.out_messages_begin;
  // Mirror block::is_transaction_out_msg's uint64 arithmetic, including
  // wraparound for malformed near-UINT64_MAX logical times.
  if (created_lt <= entry.lt || created_lt > entry.lt + outmsg_count) {
    return nullptr;
  }
  const auto offset = created_lt - entry.lt - 1;
  if (offset >= outmsg_count) {
    return nullptr;
  }
  return &messages[entry.out_messages_begin + offset];
}

template <class Entries>
bool transaction_record_plan_is_strictly_ordered(const Entries& entries) {
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].root.is_null()) {
      return false;
    }
    if (i != 0) {
      const auto& previous = entries[i - 1];
      const auto& current = entries[i];
      if (!(previous.account < current.account || (previous.account == current.account && previous.lt < current.lt))) {
        return false;
      }
    }
  }
  return true;
}

template <class Entries>
bool try_take_ordered_transaction_range(const Entries& entries, std::size_t& position, const StdSmcAddress& address,
                                        bool& available, std::size_t& begin, std::size_t& end) {
  if (position >= entries.size() || entries[position].account != address) {
    available = false;
    begin = 0;
    end = 0;
    return false;
  }
  begin = position;
  do {
    ++position;
  } while (position < entries.size() && entries[position].account == address);
  end = position;
  available = true;
  return true;
}

template <class Entries>
const typename Entries::value_type* try_take_ordered_transaction(const Entries& entries, bool& available,
                                                                 std::size_t& position, std::size_t end,
                                                                 const StdSmcAddress& address, LogicalTime lt) {
  if (!available || position >= end || position >= entries.size()) {
    available = false;
    return nullptr;
  }
  const auto& entry = entries[position];
  if (entry.account != address || entry.lt != lt) {
    // Do not resynchronize after a mismatch: the cached record is usable only
    // for the exact occurrence established by the two ordered traversals.
    available = false;
    return nullptr;
  }
  ++position;
  return &entry;
}

template <class Entry, class Address, class RootRef>
bool prechecked_transaction_matches_current_occurrence(const Entry* entry, const Address& address, LogicalTime lt,
                                                       const RootRef& current_root) {
  // Representation-hash equality is intentionally insufficient. Usage,
  // Virtual, Snap, and DataCell wrappers can have one hash but represent
  // different concrete traversal occurrences and ownership contexts.
  return entry != nullptr && entry->account == address && entry->lt == lt && entry->root.not_null() &&
         current_root.not_null() && entry->root.get() == current_root.get();
}

}  // namespace ton::validator::detail
