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

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "td/utils/int_types.h"
#include "td/utils/logging.h"
#include "vm/cells/CellTraits.h"

namespace vm {

class DataCell;
struct LoadedCell;

class CellUsageTree : public std::enable_shared_from_this<CellUsageTree> {
 public:
  using NodeId = td::uint32;

  struct NodePtr {
   public:
    NodePtr() noexcept : tree_weak_(), node_id_(0), keep_alive_(false) {
    }
    NodePtr(std::weak_ptr<CellUsageTree> tree_weak, NodeId node_id)
        : tree_weak_(std::move(tree_weak)), node_id_(node_id), keep_alive_(false) {
    }
    NodePtr(std::shared_ptr<CellUsageTree> tree, NodeId node_id)
        : tree_keep_alive_(std::move(tree)), node_id_(node_id), keep_alive_(true) {
    }
    NodePtr(const NodePtr& other) noexcept : node_id_(other.node_id_), keep_alive_(other.keep_alive_) {
      if (keep_alive_) {
        new (&tree_keep_alive_) std::shared_ptr<CellUsageTree>(other.tree_keep_alive_);
      } else {
        new (&tree_weak_) std::weak_ptr<CellUsageTree>(other.tree_weak_);
      }
    }
    NodePtr(NodePtr&& other) noexcept : node_id_(other.node_id_), keep_alive_(other.keep_alive_) {
      if (keep_alive_) {
        new (&tree_keep_alive_) std::shared_ptr<CellUsageTree>(std::move(other.tree_keep_alive_));
      } else {
        new (&tree_weak_) std::weak_ptr<CellUsageTree>(std::move(other.tree_weak_));
      }
    }
    NodePtr& operator=(const NodePtr& other) noexcept {
      if (this != &other) {
        if (keep_alive_ == other.keep_alive_) {
          if (keep_alive_) {
            tree_keep_alive_ = other.tree_keep_alive_;
          } else {
            tree_weak_ = other.tree_weak_;
          }
          node_id_ = other.node_id_;
        } else {
          this->~NodePtr();
          new (this) NodePtr(other);
        }
      }
      return *this;
    }
    NodePtr& operator=(NodePtr&& other) noexcept {
      if (this != &other) {
        if (keep_alive_ == other.keep_alive_) {
          if (keep_alive_) {
            tree_keep_alive_ = std::move(other.tree_keep_alive_);
          } else {
            tree_weak_ = std::move(other.tree_weak_);
          }
          node_id_ = other.node_id_;
        } else {
          this->~NodePtr();
          new (this) NodePtr(std::move(other));
        }
      }
      return *this;
    }
    ~NodePtr() {
      if (keep_alive_) {
        tree_keep_alive_.~shared_ptr();
      } else {
        tree_weak_.~weak_ptr();
      }
    }
    bool empty() const {
      return node_id_ == 0 || (keep_alive_ ? !tree_keep_alive_ : tree_weak_.expired());
    }

    bool on_load(const LoadedCell& loaded_cell) const;
    NodePtr create_child(unsigned ref_id) const;
    bool mark_path(CellUsageTree* master_tree) const;
    bool is_from_tree(const CellUsageTree* master_tree) const;
    NodeId node_id_for(const CellUsageTree* tree) const;

   private:
    union {
      std::weak_ptr<CellUsageTree> tree_weak_;
      std::shared_ptr<CellUsageTree> tree_keep_alive_;
    };
    NodeId node_id_{0};
    bool keep_alive_{false};
  };

  CellUsageTree();
  ~CellUsageTree();

  NodePtr root_ptr();
  // Use only when the produced usage cells can outlive the direct tree owner.
  NodePtr root_ptr_keep_alive();
  NodePtr node_ptr(NodeId node_id);
  NodePtr node_ptr_keep_alive(NodeId node_id);
  std::shared_ptr<CellUsageTree> clone_for_proof() const;
  NodeId root_id() const;
  size_t node_count() const {
    return node_count_.load(std::memory_order_acquire);
  }
  bool is_loaded(NodeId node_id) const;
  bool has_mark(NodeId node_id) const;
  void set_mark(NodeId node_id, bool mark = true);
  void set_loaded(NodeId node_id, bool loaded = true);
  void mark_path(NodeId node_id);
  NodeId get_parent(NodeId node_id) const;
  unsigned get_parent_ref(NodeId node_id) const;
  NodeId get_child(NodeId node_id, unsigned ref_id) const;
  void set_use_mark_for_is_loaded(bool use_mark = true);
  NodeId create_child(NodeId node_id, unsigned ref_id);
  void import_loaded_paths_from(const CellUsageTree& source, NodeId target_root,
                                std::vector<NodeId>* source_to_target = nullptr);

  void set_cell_load_callback(std::function<void(const LoadedCell&)> f) {
    cell_load_callback_ = std::move(f);
  }
  void set_ignore_loads(bool value) {
    ignore_loads_.fetch_add(value ? 1 : -1, std::memory_order_relaxed);
  }

 private:
  struct Node {
    std::atomic<bool> is_loaded{false};
    std::atomic<bool> has_mark{false};
    NodeId parent{0};
    td::uint8 parent_ref{0};
    std::array<std::atomic<td::uint32>, CellTraits::max_refs> children{};
  };

  static constexpr size_t chunk_bits = 13;
  static constexpr size_t chunk_size = 1 << chunk_bits;
  static constexpr size_t max_chunks = 1 << 11;
  struct Chunk {
    std::array<Node, chunk_size> nodes;
  };

  Node& node_at(NodeId id) {
    return chunks_[id >> chunk_bits].load(std::memory_order_acquire)->nodes[id & (chunk_size - 1)];
  }
  const Node& node_at(NodeId id) const {
    return chunks_[id >> chunk_bits].load(std::memory_order_acquire)->nodes[id & (chunk_size - 1)];
  }

  bool use_mark_{false};
  std::array<std::atomic<Chunk*>, max_chunks> chunks_{};
  std::atomic<size_t> node_count_{2};
  std::function<void(const LoadedCell&)> cell_load_callback_;
  void on_load(NodeId node_id, const LoadedCell& loaded_cell);
  NodeId create_node(NodeId parent, unsigned parent_ref);
  void ensure_chunk(size_t chunk_index);
  std::atomic<int> ignore_loads_{0};
};
}  // namespace vm
