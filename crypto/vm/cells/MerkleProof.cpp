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
#include <array>
#include <map>
#include <vector>

#include "td/utils/HashMap.h"
#include "td/utils/HashSet.h"
#include "td/utils/Status.h"
#include "vm/boc.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/CellSlice.h"
#include "vm/cells/MerkleProof.h"

namespace vm {
namespace detail {
class MerkleProofImpl {
 public:
  explicit MerkleProofImpl(MerkleProof::IsPrunnedFunction is_prunned) : is_prunned_(std::move(is_prunned)) {
  }
  explicit MerkleProofImpl(MerkleProof::IsHashPrunnedFunction is_prunned) : is_hash_prunned_(std::move(is_prunned)) {
  }
  explicit MerkleProofImpl(CellUsageTree *usage_tree) : usage_tree_(usage_tree) {
  }

  td::Result<Ref<Cell>> create_from(Ref<Cell> cell) {
    if (!is_prunned_ && !is_hash_prunned_) {
      CHECK(usage_tree_);
      dfs_usage_tree(cell, usage_tree_->root_id());
      is_hash_prunned_ = [this](const Cell::Hash &hash) { return visited_cells_.count(hash) == 0; };
    }
    try {
      auto merkle_depth = cell->get_level();
      return dfs(std::move(cell), merkle_depth, {});
    } catch (CellBuilder::CellWriteError &) {
      return td::Status::Error("failed to generate Merkle proof: cell write error");
    } catch (CellBuilder::CellCreateError &) {
      return td::Status::Error("failed to generate Merkle proof: cell create error");
    }
  }

 private:
  using Key = std::pair<Cell::Hash, unsigned>;
  // The output is known only after its children have been built. Keep a stable
  // vector slot in the map so a first visit does not need a second map probe.
  td::HashMap<Key, std::size_t> cell_slots_;
  std::vector<Ref<Cell>> cells_;
  td::HashSet<Cell::Hash> visited_cells_;
  CellUsageTree *usage_tree_{nullptr};
  MerkleProof::IsPrunnedFunction is_prunned_;
  MerkleProof::IsHashPrunnedFunction is_hash_prunned_;

  static void apply_tree_context(CellUsageTree::NodePtr tree_node, Cell::LoadedCell &loaded) {
    if (!tree_node.empty() && tree_node.on_load(loaded)) {
      CHECK(loaded.tree_node.empty());
      loaded.tree_node = std::move(tree_node);
    }
  }

  void dfs_usage_tree(Ref<Cell> cell, CellUsageTree::NodeId node_id, CellUsageTree::NodePtr tree_node = {}) {
    if (!usage_tree_->is_loaded(node_id)) {
      return;
    }
    visited_cells_.insert(cell->get_hash());
    auto r_loaded = cell->load_cell();
    if (r_loaded.is_error()) {
      // CellSlice(NoVm, cell), used by the legacy traversal, silently becomes
      // empty on a failed load and therefore has no children to visit.
      return;
    }
    auto loaded = r_loaded.move_as_ok();
    apply_tree_context(std::move(tree_node), loaded);
    auto data_cell = std::move(loaded.data_cell);
    auto loaded_tree_node = std::move(loaded.tree_node);
    bool merkle = data_cell->special_type() == Cell::SpecialType::MerkleProof ||
                  data_cell->special_type() == Cell::SpecialType::MerkleUpdate;
    auto child_effective_level = merkle ? loaded.effective_level + 1 : loaded.effective_level;
    for (unsigned i = 0; i < data_cell->get_refs_cnt(); i++) {
      auto child = data_cell->get_ref(i);
      auto *child_ptr = child.get();
      child = child_ptr->virtualize_ref(std::move(child), child_effective_level);
      auto child_tree_node = loaded_tree_node.empty() ? CellUsageTree::NodePtr{} : loaded_tree_node.create_child(i);
      dfs_usage_tree(std::move(child), usage_tree_->get_child(node_id, i), std::move(child_tree_node));
    }
  }

  Ref<Cell> create_pruned_branch(Ref<Cell> cell, unsigned merkle_depth, CellUsageTree::NodePtr tree_node) {
    if (tree_node.empty()) {
      return CellBuilder::create_pruned_branch(std::move(cell), merkle_depth + 1);
    }
    // Preserve create_pruned_branch's loaded-leaf shortcut and the UsageCell
    // load callback without allocating a wrapper on the common non-leaf path.
    if (cell->is_loaded() && !cell->is_virtualized()) {
      auto return_tree_node = tree_node;
      auto r_loaded = cell->load_cell();
      if (r_loaded.is_error()) {
        return UsageCell::create(std::move(cell), std::move(return_tree_node));
      }
      auto loaded = r_loaded.move_as_ok();
      apply_tree_context(std::move(tree_node), loaded);
      if (loaded.data_cell->size_refs() == 0) {
        return UsageCell::create(std::move(cell), std::move(return_tree_node));
      }
    }
    return CellBuilder::do_create_pruned_branch(std::move(cell), merkle_depth + 1);
  }

  Ref<Cell> dfs(Ref<Cell> cell, unsigned merkle_depth, CellUsageTree::NodePtr tree_node) {
    CHECK(cell.not_null());
    Key key{cell->get_hash(), merkle_depth};
    auto [it, inserted] = cell_slots_.emplace(key, cells_.size());
    if (!inserted) {
      CHECK(cells_[it->second].not_null());
      return cells_[it->second];
    }
    auto slot = it->second;
    cells_.emplace_back();

    bool is_prunned = is_hash_prunned_ ? is_hash_prunned_(key.first) : is_prunned_(cell);
    if (is_prunned) {
      auto res = create_pruned_branch(std::move(cell), merkle_depth, std::move(tree_node));
      CHECK(res.not_null());
      cells_[slot] = res;
      return res;
    }
    auto hash_hint = [&](unsigned level, const Cell::LevelMask &, CellHash &hash) {
      if (level <= merkle_depth) {
        hash = cell->get_hash(level);
        return true;
      }
      return false;
    };
    auto loaded = cell->load_cell().move_as_ok();
    apply_tree_context(std::move(tree_node), loaded);
    auto data_cell = std::move(loaded.data_cell);
    auto loaded_tree_node = std::move(loaded.tree_node);
    auto bits = data_cell->get_bits();
    auto refs_count = data_cell->get_refs_cnt();
    bool merkle = data_cell->special_type() == Cell::SpecialType::MerkleProof ||
                  data_cell->special_type() == Cell::SpecialType::MerkleUpdate;
    int children_merkle_depth = merkle ? merkle_depth + 1 : merkle_depth;
    auto child_effective_level = merkle ? loaded.effective_level + 1 : loaded.effective_level;
    std::array<Ref<Cell>, Cell::max_refs> refs;
    for (unsigned i = 0; i < refs_count; i++) {
      auto child = data_cell->get_ref(i);
      auto *child_ptr = child.get();
      child = child_ptr->virtualize_ref(std::move(child), child_effective_level);
      auto child_tree_node = loaded_tree_node.empty() ? CellUsageTree::NodePtr{} : loaded_tree_node.create_child(i);
      if (is_hash_prunned_) {
        refs[i] = dfs(std::move(child), children_merkle_depth, std::move(child_tree_node));
      } else {
        if (!child_tree_node.empty()) {
          child = UsageCell::create(std::move(child), std::move(child_tree_node));
        }
        refs[i] = dfs(std::move(child), children_merkle_depth, {});
      }
    }
    auto res = CellBuilder::create_data_cell(td::Slice{data_cell->get_data(), (bits + 7) / 8}, bits,
                                             td::mutable_span(refs.data(), refs_count), data_cell->is_special(),
                                             std::move(hash_hint));
    CHECK(res.not_null());
    cells_[slot] = res;
    return res;
  }
};
}  // namespace detail

td::Result<Ref<Cell>> MerkleProof::generate_raw(Ref<Cell> cell, IsPrunnedFunction is_prunned) {
  return detail::MerkleProofImpl(std::move(is_prunned)).create_from(std::move(cell));
}

td::Result<Ref<Cell>> MerkleProof::generate_raw_by_hash(Ref<Cell> cell, IsHashPrunnedFunction is_prunned) {
  return detail::MerkleProofImpl(std::move(is_prunned)).create_from(std::move(cell));
}

td::Result<Ref<Cell>> MerkleProof::generate_raw(Ref<Cell> cell, CellUsageTree *usage_tree) {
  return detail::MerkleProofImpl(usage_tree).create_from(cell);
}

Ref<Cell> MerkleProof::virtualize_raw(Ref<Cell> cell, td::uint32 effective_level) {
  auto *cell_ptr = cell.get();
  return cell_ptr->virtualize_ref(std::move(cell), effective_level);
}

td::Result<Ref<Cell>> MerkleProof::generate(Ref<Cell> cell, IsPrunnedFunction is_prunned) {
  if (cell.is_null()) {
    return td::Status::Error("failed to generate Merkle proof: cell is null");
  }
  if (cell->get_level() != 0) {
    return td::Status::Error("failed to generate Merkle proof: level is not 0");
  }
  TRY_RESULT(raw, generate_raw(std::move(cell), std::move(is_prunned)));
  return CellBuilder::create_merkle_proof(std::move(raw));
}

td::Result<Ref<Cell>> MerkleProof::generate_by_hash(Ref<Cell> cell, IsHashPrunnedFunction is_prunned) {
  if (cell.is_null()) {
    return td::Status::Error("failed to generate Merkle proof: cell is null");
  }
  if (cell->get_level() != 0) {
    return td::Status::Error("failed to generate Merkle proof: level is not 0");
  }
  TRY_RESULT(raw, generate_raw_by_hash(std::move(cell), std::move(is_prunned)));
  return CellBuilder::create_merkle_proof(std::move(raw));
}

td::Result<Ref<Cell>> MerkleProof::generate(Ref<Cell> cell, CellUsageTree *usage_tree) {
  if (cell.is_null()) {
    return td::Status::Error("failed to generate Merkle proof: cell is null");
  }
  if (cell->get_level() != 0) {
    return td::Status::Error("failed to generate Merkle proof: level is not 0");
  }
  TRY_RESULT(raw, generate_raw(std::move(cell), usage_tree));
  return CellBuilder::create_merkle_proof(std::move(raw));
}

static td::Result<Ref<Cell>> unpack_proof(Ref<Cell> cell) {
  if (cell.is_null()) {
    return td::Status::Error("failed to unpack Merkle proof: cell is null");
  }
  if (cell->get_level()) {
    return td::Status::Error("failed to unpack Merkle proof: level of MerkleProof must be zero");
  }
  CellSlice cs(NoVm(), std::move(cell));
  if (cs.special_type() != Cell::SpecialType::MerkleProof) {
    return td::Status::Error("failed to unpack Merkle proof: not a MerkleProof cell");
  }
  return cs.fetch_ref();
}

td::Result<Ref<Cell>> MerkleProof::virtualize(Ref<Cell> cell) {
  TRY_RESULT(unpacked_cell, unpack_proof(std::move(cell)));
  auto *cell_ptr = unpacked_cell.get();
  return cell_ptr->virtualize_ref(std::move(unpacked_cell), 0);
}

class MerkleProofCombineFast {
 public:
  MerkleProofCombineFast(Ref<Cell> a, Ref<Cell> b) : a_(std::move(a)), b_(std::move(b)) {
  }
  td::Result<Ref<Cell>> run() {
    if (a_.is_null()) {
      return b_;
    }
    if (b_.is_null()) {
      return a_;
    }
    TRY_RESULT_ASSIGN(a_, unpack_proof(a_));
    TRY_RESULT_ASSIGN(b_, unpack_proof(b_));
    TRY_RESULT(res, run_raw());
    return CellBuilder::create_merkle_proof(std::move(res));
  }

  td::Result<Ref<Cell>> run_raw() {
    if (a_->get_hash(0) != b_->get_hash(0)) {
      return td::Status::Error("Can't combine MerkleProofs with different roots");
    }
    return merge(a_, b_, 0);
  }

 private:
  Ref<Cell> a_;
  Ref<Cell> b_;
#if TD_HAVE_ABSL
  td::HashMap<std::tuple<Cell::Hash, Cell::Hash, td::uint32>, Ref<Cell>> visited_;
#else
  std::map<std::tuple<Cell::Hash, Cell::Hash, td::uint32>, Ref<Cell>> visited_;
#endif

  Ref<Cell> merge(Ref<Cell> a, Ref<Cell> b, td::uint32 merkle_depth) {
    if (a->get_hash() == b->get_hash()) {
      return a;
    }
    if (a->get_level() == merkle_depth) {
      return a;
    }
    if (b->get_level() == merkle_depth) {
      return b;
    }

    CellSlice csa(NoVm(), a);
    CellSlice csb(NoVm(), b);

    if (csa.is_special() && csa.special_type() == vm::Cell::SpecialType::PrunnedBranch) {
      return b;
    }
    if (csb.is_special() && csb.special_type() == vm::Cell::SpecialType::PrunnedBranch) {
      return a;
    }
    std::tuple key{a->get_hash(), b->get_hash(), merkle_depth};
    if (auto it = visited_.find(key); it != visited_.end()) {
      return it->second;
    }

    CHECK(csa.size_refs() != 0);

    auto child_merkle_depth = csa.child_merkle_depth(merkle_depth);

    CellBuilder cb;
    cb.store_bits(csa.fetch_bits(csa.size()));
    for (unsigned i = 0; i < csa.size_refs(); i++) {
      cb.store_ref(merge(csa.prefetch_ref(i), csb.prefetch_ref(i), child_merkle_depth));
    }
    return visited_[key] = cb.finalize(csa.is_special());
  }
};

class MerkleProofCombine {
 public:
  MerkleProofCombine(Ref<Cell> a, Ref<Cell> b) : a_(std::move(a)), b_(std::move(b)) {
  }
  td::Result<Ref<Cell>> run() {
    if (a_.is_null()) {
      return b_;
    }
    if (b_.is_null()) {
      return a_;
    }
    TRY_RESULT_ASSIGN(a_, unpack_proof(a_));
    TRY_RESULT_ASSIGN(b_, unpack_proof(b_));
    TRY_RESULT(res, run_raw());
    return CellBuilder::create_merkle_proof(std::move(res));
  }

  td::Result<Ref<Cell>> run_raw() {
    if (a_->get_hash(0) != b_->get_hash(0)) {
      return td::Status::Error("Can't combine MerkleProofs with different roots");
    }
    dfs(a_, 0);
    dfs(b_, 0);
    return create_A(a_, 0, 0);
  }

 private:
  Ref<Cell> a_;
  Ref<Cell> b_;

  struct Info {
    Ref<Cell> cell_;
    Ref<Cell> prunned_cells_[Cell::max_level];  // Cache prunned cells with different levels to reuse them

    Ref<Cell> get_prunned_cell(int depth) {
      if (depth < Cell::max_level) {
        return prunned_cells_[depth];
      }
      return {};
    }
    Ref<Cell> get_any_cell() const {
      if (cell_.not_null()) {
        return cell_;
      }
      for (auto &cell : prunned_cells_) {
        if (cell.not_null()) {
          return cell;
        }
      }
      UNREACHABLE();
    }
  };

  using Key = std::pair<Cell::Hash, int>;
  td::HashMap<Cell::Hash, Info> cells_;
  td::HashMap<Key, Ref<Cell>> create_A_res_;
  td::HashSet<Key> visited_;

  void dfs(Ref<Cell> cell, int merkle_depth) {
    if (!visited_.emplace(cell->get_hash(), merkle_depth).second) {
      return;
    }

    auto &info = cells_[cell->get_hash(merkle_depth)];
    CellSlice cs(NoVm(), cell);
    // check if prunned cell is bounded
    if (cs.special_type() == Cell::SpecialType::PrunnedBranch && static_cast<int>(cell->get_level()) > merkle_depth) {
      info.prunned_cells_[cell->get_level() - 1] = std::move(cell);
      return;
    }
    info.cell_ = std::move(cell);

    auto child_merkle_depth = cs.child_merkle_depth(merkle_depth);
    for (size_t i = 0, size = cs.size_refs(); i < size; i++) {
      dfs(cs.fetch_ref(), child_merkle_depth);
    }
  }

  Ref<Cell> create_A(Ref<Cell> cell, int merkle_depth, int a_merkle_depth) {
    merkle_depth = cell->get_level_mask().apply(merkle_depth).get_level();
    auto key = Key(cell->get_hash(merkle_depth), a_merkle_depth);
    auto it = create_A_res_.find(key);
    if (it != create_A_res_.end()) {
      return it->second;
    }

    auto res = do_create_A(std::move(cell), merkle_depth, a_merkle_depth);
    create_A_res_.emplace(key, res);
    return res;
  }

  Ref<Cell> do_create_A(Ref<Cell> cell, int merkle_depth, int a_merkle_depth) {
    auto &info = cells_[cell->get_hash(merkle_depth)];

    if (info.cell_.is_null()) {
      Ref<Cell> res = info.get_prunned_cell(a_merkle_depth);
      if (res.is_null()) {
        res = CellBuilder::create_pruned_branch(info.get_any_cell(), a_merkle_depth + 1, merkle_depth);
      }
      return res;
    }

    CHECK(info.cell_.not_null());
    CellSlice cs(NoVm(), info.cell_);

    //CHECK(cs.size_refs() != 0);
    if (cs.size_refs() == 0) {
      return info.cell_;
    }

    auto child_merkle_depth = cs.child_merkle_depth(merkle_depth);
    auto child_a_merkle_depth = cs.child_merkle_depth(a_merkle_depth);

    CellBuilder cb;
    cb.store_bits(cs.fetch_bits(cs.size()));
    for (unsigned i = 0; i < cs.size_refs(); i++) {
      cb.store_ref(create_A(cs.prefetch_ref(i), child_merkle_depth, child_a_merkle_depth));
    }
    return cb.finalize(cs.is_special());
  }
};

td::Result<Ref<Cell>> MerkleProof::combine(Ref<Cell> a, Ref<Cell> b) {
  return MerkleProofCombine(std::move(a), std::move(b)).run();
}

td::Result<Ref<Cell>> MerkleProof::combine_fast(Ref<Cell> a, Ref<Cell> b) {
  return MerkleProofCombineFast(std::move(a), std::move(b)).run();
}

td::Result<Ref<Cell>> MerkleProof::combine_raw(Ref<Cell> a, Ref<Cell> b) {
  return MerkleProofCombine(std::move(a), std::move(b)).run_raw();
}

td::Result<Ref<Cell>> MerkleProof::combine_fast_raw(Ref<Cell> a, Ref<Cell> b) {
  return MerkleProofCombineFast(std::move(a), std::move(b)).run_raw();
}

MerkleProofBuilder::MerkleProofBuilder(Ref<Cell> root)
    : usage_tree(std::make_shared<CellUsageTree>()), orig_root(std::move(root)) {
  usage_root = UsageCell::create(orig_root, usage_tree->root_ptr());
}

Ref<Cell> MerkleProofBuilder::init(Ref<Cell> root) {
  usage_tree = std::make_shared<CellUsageTree>();
  orig_root = std::move(root);
  usage_root = UsageCell::create(orig_root, usage_tree->root_ptr());
  return usage_root;
}

bool MerkleProofBuilder::clear() {
  usage_tree.reset();
  orig_root.clear();
  usage_root.clear();
  return true;
}

td::Result<Ref<Cell>> MerkleProofBuilder::extract_proof() const {
  return MerkleProof::generate(orig_root, usage_tree.get());
}

bool MerkleProofBuilder::extract_proof_to(Ref<Cell> &proof_root) const {
  if (orig_root.is_null()) {
    return false;
  }
  auto R = extract_proof();
  if (R.is_error()) {
    return false;
  }
  proof_root = R.move_as_ok();
  return true;
}

td::Result<td::BufferSlice> MerkleProofBuilder::extract_proof_boc() const {
  TRY_RESULT(proof_root, extract_proof());
  return std_boc_serialize(std::move(proof_root));
}

}  // namespace vm
