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
#include "vm/cells/CellUsageTree.h"

#include "td/utils/logging.h"

#include "DataCell.h"

namespace vm {
//
// CellUsageTree::NodePtr
//
bool CellUsageTree::NodePtr::on_load(const Cell::LoadedCell& loaded_cell) const {
  auto tree = tree_weak_.lock();
  if (!tree) {
    return false;
  }
  tree->on_load(node_id_, loaded_cell);
  return true;
}

CellUsageTree::NodePtr CellUsageTree::NodePtr::create_child(unsigned ref_id) const {
  auto tree = tree_weak_.lock();
  if (!tree) {
    return {};
  }
  return {tree_weak_, tree->create_child(node_id_, ref_id)};
}

bool CellUsageTree::NodePtr::is_from_tree(const CellUsageTree* master_tree) const {
  DCHECK(master_tree);
  auto tree = tree_weak_.lock();
  if (tree.get() != master_tree) {
    return false;
  }
  return true;
}

CellUsageTree::NodeId CellUsageTree::NodePtr::node_id_for(const CellUsageTree* tree) const {
  auto owner = tree_weak_.lock();
  return owner.get() == tree ? node_id_ : 0;
}

bool CellUsageTree::NodePtr::mark_path(CellUsageTree* master_tree) const {
  DCHECK(master_tree);
  auto tree = tree_weak_.lock();
  if (tree.get() != master_tree) {
    return false;
  }
  master_tree->mark_path(node_id_);
  return true;
}

//
// CellUsageTree
//
CellUsageTree::CellUsageTree() {
  ensure_chunk(0);
}

CellUsageTree::~CellUsageTree() {
  for (auto& slot : chunks_) {
    delete slot.load(std::memory_order_relaxed);
  }
}

CellUsageTree::NodePtr CellUsageTree::root_ptr() {
  return {shared_from_this(), 1};
}

CellUsageTree::NodePtr CellUsageTree::node_ptr(NodeId node_id) {
  DCHECK(node_id != 0 && node_id < node_count_.load(std::memory_order_acquire));
  return {shared_from_this(), node_id};
}

std::shared_ptr<CellUsageTree> CellUsageTree::clone_for_proof() const {
  auto clone = std::make_shared<CellUsageTree>();
  size_t count = node_count_.load(std::memory_order_acquire);
  for (size_t chunk = 1; chunk <= ((count - 1) >> chunk_bits); ++chunk) {
    clone->ensure_chunk(chunk);
  }
  clone->node_count_.store(count, std::memory_order_release);
  for (size_t i = 0; i < count; ++i) {
    const auto& source = node_at(static_cast<NodeId>(i));
    auto& target = clone->node_at(static_cast<NodeId>(i));
    target.is_loaded.store(source.is_loaded.load(std::memory_order_relaxed), std::memory_order_relaxed);
    target.has_mark.store(source.has_mark.load(std::memory_order_relaxed), std::memory_order_relaxed);
    target.parent = source.parent;
    target.parent_ref = source.parent_ref;
    for (unsigned ref_id = 0; ref_id < CellTraits::max_refs; ++ref_id) {
      target.children[ref_id].store(source.children[ref_id].load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
    }
  }
  clone->use_mark_ = use_mark_;
  return clone;
}

CellUsageTree::NodeId CellUsageTree::root_id() const {
  return 1;
};

bool CellUsageTree::is_loaded(NodeId node_id) const {
  if (use_mark_) {
    return node_at(node_id).has_mark.load(std::memory_order_acquire);
  }
  return node_at(node_id).is_loaded.load(std::memory_order_acquire);
}

bool CellUsageTree::has_mark(NodeId node_id) const {
  return node_at(node_id).has_mark.load(std::memory_order_acquire);
}

void CellUsageTree::set_mark(NodeId node_id, bool mark) {
  if (node_id == 0) {
    return;
  }
  node_at(node_id).has_mark.store(mark, std::memory_order_release);
}

void CellUsageTree::set_loaded(NodeId node_id, bool loaded) {
  if (node_id == 0) {
    return;
  }
  node_at(node_id).is_loaded.store(loaded, std::memory_order_release);
}

void CellUsageTree::mark_path(NodeId node_id) {
  auto cur_node_id = get_parent(node_id);
  while (cur_node_id != 0) {
    if (has_mark(cur_node_id)) {
      break;
    }
    set_mark(cur_node_id);
    cur_node_id = get_parent(cur_node_id);
  }
}

CellUsageTree::NodeId CellUsageTree::get_parent(NodeId node_id) const {
  return node_at(node_id).parent;
}

unsigned CellUsageTree::get_parent_ref(NodeId node_id) const {
  DCHECK(node_id != 0 && node_id < node_count_.load(std::memory_order_acquire));
  return node_at(node_id).parent_ref;
}

CellUsageTree::NodeId CellUsageTree::get_child(NodeId node_id, unsigned ref_id) const {
  DCHECK(ref_id < CellTraits::max_refs);
  return node_at(node_id).children[ref_id].load(std::memory_order_acquire);
}

void CellUsageTree::set_use_mark_for_is_loaded(bool use_mark) {
  use_mark_ = use_mark;
}

void CellUsageTree::on_load(NodeId node_id, const Cell::LoadedCell& loaded_cell) {
  if (ignore_loads_.load(std::memory_order_relaxed) != 0) {
    return;
  }
  bool expected = false;
  if (!node_at(node_id).is_loaded.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
    return;
  }
  if (cell_load_callback_) {
    cell_load_callback_(loaded_cell);
  }
}

CellUsageTree::NodeId CellUsageTree::create_child(NodeId node_id, unsigned ref_id) {
  DCHECK(ref_id < CellTraits::max_refs);
  NodeId res = node_at(node_id).children[ref_id].load(std::memory_order_acquire);
  if (res) {
    return res;
  }
  NodeId candidate = create_node(node_id, ref_id);
  NodeId expected = 0;
  if (node_at(node_id).children[ref_id].compare_exchange_strong(expected, candidate, std::memory_order_release,
                                                                std::memory_order_acquire)) {
    return candidate;
  }
  return expected;
}

void CellUsageTree::import_loaded_paths_from(const CellUsageTree& source, NodeId target_root,
                                             std::vector<NodeId>* source_to_target) {
  DCHECK(target_root != 0 && target_root < node_count_.load(std::memory_order_acquire));
  if (source_to_target != nullptr) {
    source_to_target->assign(source.node_count(), 0);
    (*source_to_target)[source.root_id()] = target_root;
  }
  std::vector<std::pair<NodeId, NodeId>> pending{{source.root_id(), target_root}};
  while (!pending.empty()) {
    auto [source_node, target_node] = pending.back();
    pending.pop_back();
    if (source.is_loaded(source_node)) {
      set_loaded(target_node);
    }
    for (unsigned ref_id = 0; ref_id < CellTraits::max_refs; ++ref_id) {
      NodeId source_child = source.get_child(source_node, ref_id);
      if (source_child == 0) {
        continue;
      }
      NodeId target_child = create_child(target_node, ref_id);
      if (source_to_target != nullptr) {
        (*source_to_target)[source_child] = target_child;
      }
      pending.emplace_back(source_child, target_child);
    }
  }
}

CellUsageTree::NodeId CellUsageTree::create_node(NodeId parent, unsigned parent_ref) {
  size_t res = node_count_.fetch_add(1, std::memory_order_acq_rel);
  LOG_CHECK(res < max_chunks * chunk_size) << "CellUsageTree node limit exceeded";
  ensure_chunk(res >> chunk_bits);
  auto& node = node_at(static_cast<NodeId>(res));
  node.parent = parent;
  node.parent_ref = static_cast<td::uint8>(parent_ref);
  return static_cast<NodeId>(res);
}

void CellUsageTree::ensure_chunk(size_t chunk_index) {
  if (chunks_[chunk_index].load(std::memory_order_acquire) != nullptr) {
    return;
  }
  auto* chunk = new Chunk{};
  Chunk* expected = nullptr;
  if (!chunks_[chunk_index].compare_exchange_strong(expected, chunk, std::memory_order_release,
                                                     std::memory_order_acquire)) {
    delete chunk;
  }
}

}  // namespace vm
