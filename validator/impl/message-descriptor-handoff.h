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
#include <optional>
#include <vector>

#include "vm/cells/Cell.h"
#include "vm/cells/CellSlice.h"

namespace ton::validator::detail {

struct MessageDescriptorLink {
  td::Ref<vm::Cell> transaction;
  td::Ref<vm::Cell> message;
};

struct MessageDescriptorRoots {
  td::Ref<vm::Cell> in_wrapped;
  td::Ref<vm::Cell> in_inner;
  td::Ref<vm::Cell> out_wrapped;
  td::Ref<vm::Cell> out_inner;
};

inline bool message_descriptor_roots_match(const MessageDescriptorRoots& expected, const td::Ref<vm::Cell>& in_wrapped,
                                           const td::Ref<vm::Cell>& in_inner, const td::Ref<vm::Cell>& out_wrapped,
                                           const td::Ref<vm::Cell>& out_inner) {
  return expected.in_wrapped.get() == in_wrapped.get() && expected.in_inner.get() == in_inner.get() &&
         expected.out_wrapped.get() == out_wrapped.get() && expected.out_inner.get() == out_inner.get();
}

template <class TransactionPlan>
bool capture_in_message_descriptor_cursor(const TransactionPlan& transactions, const MessageDescriptorLink& link,
                                          vm::CellSlice original, std::vector<std::optional<vm::CellSlice>>& cursors) {
  if (link.transaction.is_null() && link.message.is_null()) {
    return true;
  }
  if (link.transaction.is_null() || link.message.is_null() || cursors.size() != transactions.records.size()) {
    return false;
  }
  auto it = transactions.exact_root_index.find(link.transaction.get());
  if (it == transactions.exact_root_index.end() || it->second >= transactions.records.size()) {
    return false;
  }
  const auto index = it->second;
  const auto& transaction = transactions.records[index];
  if (transaction.root.get() != link.transaction.get() || transaction.in_message.get() != link.message.get() ||
      cursors[index].has_value()) {
    return false;
  }
  cursors[index].emplace(std::move(original));
  return true;
}

template <class TransactionPlan>
bool capture_out_message_descriptor_cursor(const TransactionPlan& transactions, const MessageDescriptorLink& link,
                                           vm::CellSlice original, std::vector<std::optional<vm::CellSlice>>& cursors) {
  if (link.transaction.is_null() && link.message.is_null()) {
    return true;
  }
  if (link.transaction.is_null() || link.message.is_null() || cursors.size() != transactions.out_messages.size()) {
    return false;
  }
  auto it = transactions.exact_root_index.find(link.transaction.get());
  if (it == transactions.exact_root_index.end() || it->second >= transactions.records.size()) {
    return false;
  }
  const auto& transaction = transactions.records[it->second];
  if (transaction.root.get() != link.transaction.get() ||
      transaction.out_messages_end < transaction.out_messages_begin ||
      transaction.out_messages_end > transactions.out_messages.size()) {
    return false;
  }
  std::size_t match = transactions.out_messages.size();
  for (std::size_t i = transaction.out_messages_begin; i < transaction.out_messages_end; ++i) {
    if (transactions.out_messages[i].get() == link.message.get()) {
      if (match != transactions.out_messages.size()) {
        return false;
      }
      match = i;
    }
  }
  if (match == transactions.out_messages.size() || cursors[match].has_value()) {
    return false;
  }
  cursors[match].emplace(std::move(original));
  return true;
}

template <class TransactionPlan>
bool message_descriptor_cursor_slots_complete(const TransactionPlan& transactions,
                                              const std::vector<std::optional<vm::CellSlice>>& in_cursors,
                                              const std::vector<std::optional<vm::CellSlice>>& out_cursors) {
  if (in_cursors.size() != transactions.records.size() || out_cursors.size() != transactions.out_messages.size()) {
    return false;
  }
  for (std::size_t i = 0; i < transactions.records.size(); ++i) {
    if (transactions.records[i].in_message.not_null() != in_cursors[i].has_value()) {
      return false;
    }
  }
  for (const auto& cursor : out_cursors) {
    if (!cursor.has_value()) {
      return false;
    }
  }
  return true;
}

template <class TransactionPlan>
bool preflight_message_descriptor_transaction(const TransactionPlan& transactions,
                                              const std::vector<std::optional<vm::CellSlice>>& in_cursors,
                                              const std::vector<std::optional<vm::CellSlice>>& out_cursors,
                                              const typename TransactionPlan::value_type* transaction,
                                              const td::Ref<vm::Cell>& current_root,
                                              const td::Ref<vm::Cell>& current_in_message,
                                              std::size_t& transaction_index) {
  if (transaction == nullptr || current_root.is_null() || in_cursors.size() != transactions.records.size() ||
      out_cursors.size() != transactions.out_messages.size()) {
    return false;
  }
  auto it = transactions.exact_root_index.find(current_root.get());
  if (it == transactions.exact_root_index.end() || it->second >= transactions.records.size()) {
    return false;
  }
  transaction_index = it->second;
  const auto& expected = transactions.records[transaction_index];
  if (&expected != transaction || expected.root.get() != current_root.get() ||
      expected.in_message.get() != current_in_message.get() ||
      expected.in_message.not_null() != in_cursors[transaction_index].has_value() ||
      expected.out_messages_end < expected.out_messages_begin || expected.out_messages_end > out_cursors.size()) {
    return false;
  }
  for (std::size_t i = expected.out_messages_begin; i < expected.out_messages_end; ++i) {
    if (transactions.out_messages[i].is_null() || !out_cursors[i].has_value()) {
      return false;
    }
  }
  return true;
}

}  // namespace ton::validator::detail
