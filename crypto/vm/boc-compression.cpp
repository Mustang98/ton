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
#include "boc-compression.h"

#include <algorithm>
#include <bitset>
#include "common/refint.h"
#include "vm/boc.h"
#include "vm/cellslice.h"
#include "td/utils/Slice-decl.h"
#include "td/utils/lz4.h"
#include "crypto/block/block-auto.h"
#include "crypto/block/block-parse.h"
#include <fstream>

namespace vm {

td::Result<td::BufferSlice> boc_compress_baseline_lz4(const std::vector<td::Ref<vm::Cell>>& boc_roots) {
  TRY_RESULT(data, vm::std_boc_serialize_multi(std::move(boc_roots), 2));
  td::BufferSlice compressed = td::lz4_compress(data);

  // Add decompressed size at the beginning
  td::BufferSlice compressed_with_size(compressed.size() + kDecompressedSizeBytes);
  auto size_slice = td::BitSliceWrite(compressed_with_size.as_slice().ubegin(), kDecompressedSizeBytes * 8);
  size_slice.bits().store_uint(data.size(), kDecompressedSizeBytes * 8);
  memcpy(compressed_with_size.data() + kDecompressedSizeBytes, compressed.data(), compressed.size());

  return compressed_with_size;
}

td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress_baseline_lz4(td::Slice compressed, int max_decompressed_size) {
  // Check minimum input size for decompressed size header
  if (compressed.size() < kDecompressedSizeBytes) {
    return td::Status::Error("BOC decompression failed: input too small for header");
  }

  // Read decompressed size
  constexpr size_t kSizeBits = kDecompressedSizeBytes * 8;
  int decompressed_size = td::BitSlice(compressed.ubegin(), kSizeBits).bits().get_uint(kSizeBits);
  compressed.remove_prefix(kDecompressedSizeBytes);
  if (decompressed_size <= 0 || decompressed_size > max_decompressed_size) {
    return td::Status::Error("BOC decompression failed: invalid decompressed size");
  }

  TRY_RESULT(decompressed, td::lz4_decompress(compressed, decompressed_size));
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(decompressed));
  return roots;
}


td::HashMap<vm::Cell::Hash, size_t> local_cache;
td::HashMap<std::string, size_t> data_cache;
int cnt_v = 0;

std::vector<int> sourced_vertices;
std::vector<int> matching_data_vertices;
std::vector<std::pair<int, int>> direct_edges;
std::vector<std::pair<int, int>> back_edges;
std::map<int, std::string> vertex_data;
std::map<int, int> subtree_size;

void clear_cache() {
  local_cache.clear();
  data_cache.clear();
}

void print_tree_structure() {
  std::ofstream fout("/Users/olegvallas/midnight25/BOC_visualizer/input.txt");
  // std::cout << "direct_edges_inp=\"";
  for (auto& e : direct_edges) {
    fout << "(" << e.first << ">" << e.second << (e == direct_edges.back() ? ")" : "),");
  }
  fout << std::endl;
  // std::cout << "back_edges_inp=\"";
  for (auto& e : back_edges) {
    fout << "(" << e.first << ">" << e.second << (e == back_edges.back() ? ")" : "),");
  }
  fout << std::endl;
  // std::cout << "sourced_nodes_inp=\"";
  for (auto& e : sourced_vertices) {
    fout << e << (e == sourced_vertices.back() ? "" : ",");
  }
  fout << std::endl;
  for (auto& e : matching_data_vertices) {
    fout << e << (e == matching_data_vertices.back() ? "" : ",");
  }
  fout << std::endl;
  fout << vertex_data.size() << std::endl;
  for (auto& e : vertex_data) {
    fout << e.first << " " << e.second << std::endl;
  }
  fout << subtree_size.size() << std::endl;
  for (auto& e : subtree_size) {
    fout << e.first << " " << e.second << std::endl;
  }
  fout.close();
  // std::cout << "==========================" << std::endl;
}

// 9248
int analyze_tree_structure(td::Ref<vm::Cell> cell, bool right_tree, td::Ref<vm::Cell> left_cell, const std::set<vm::Cell::Hash>& skipped_diffs, int pr = -1) {
  // std::cout << data_cache.size() << std::endl;
  auto cell_hash = cell->get_hash();
  auto cache_it = local_cache.find(cell_hash);
  int current_id = cnt_v++;
  if (pr != -1) {
    direct_edges.push_back({pr, current_id});
  }

  // Mark right-tree vertices that are in skipped_diffs
  bool size_0 = false;
  if (right_tree && skipped_diffs.find(cell_hash) != skipped_diffs.end()) {
    matching_data_vertices.emplace_back(current_id);
    // size_0 = true;
  }

  if (right_tree && cache_it != local_cache.end()) {
    int matched_id = cache_it->second;
    sourced_vertices.push_back(current_id);
    sourced_vertices.push_back(matched_id);
    back_edges.push_back({current_id, matched_id});
    return 0;
  }

  if (!right_tree) {
    local_cache.emplace(cell_hash, current_id);
  }
  // if (left_cell != td::Ref<vm::Cell>()) {
  //   auto left_cell_hash = left_cell->get_hash();
  //   auto it_left = local_cache.find(left_cell_hash);
  //   if (it_left == local_cache.end()) {
  //     exit(229);
  //   }
  //   int matched_id = it_left->second;
  //   matching_data_vertices.emplace_back(matched_id);
  //   matching_data_vertices.emplace_back(current_id);
  // }

  bool is_special = false;
  int sum_size = 0;
  vm::CellSlice cell_slice = vm::load_cell_slice_special(cell, is_special);
  td::BitSlice cell_bitslice = cell_slice.as_bitslice();
  vertex_data[current_id] = cell_bitslice.to_hex();
  if (!right_tree) {
    for (int i = 1; i <= cell_bitslice.size(); ++i) {
      std::string hash = cell_bitslice.subslice(0, i).to_binary();
      data_cache.emplace(hash, current_id);
    }
  }
  int longest_common_prefix = 0;
  // if (right_tree) {
  //   for (int i = 1; i <= cell_bitslice.size(); ++i) {
  //     std::string hash = cell_bitslice.subslice(0, i).to_binary();
  //     if (data_cache.find(hash) != data_cache.end()) {
  //       longest_common_prefix = i;
  //     }
  //   }
  // }

  // if (left_cell != td::Ref<vm::Cell>()) {
  //   vm::CellSlice left_cell_slice = vm::load_cell_slice_special(left_cell, is_special);
  //   td::BitSlice left_cell_bitslice = left_cell_slice.as_bitslice();
  //   int k = 0;
  //   for (k = 0; k < std::min(cell_bitslice.size(), left_cell_bitslice.size()); ++k) {
  //     if (cell_bitslice[k] != left_cell_bitslice[k]) {
  //       break;
  //     }
  //   }
  // }
  if (!size_0) {
    sum_size += (cell_bitslice.size() - longest_common_prefix);
    if (cell_slice.special_type() == vm::CellTraits::SpecialType::PrunnedBranch) {
      sum_size -= 16;
    }
  }

  if (left_cell != td::Ref<vm::Cell>()) {
    vm::CellSlice left_cell_slice = vm::load_cell_slice_special(left_cell, is_special);
    if (left_cell_slice.size_refs() == cell_slice.size_refs()) {
      for (int i = 0; i < cell_slice.size_refs(); ++i) {
        sum_size += analyze_tree_structure(
          cell_slice.prefetch_ref(i),
          right_tree,
          left_cell_slice.prefetch_ref(i),
          skipped_diffs,
          current_id
        );
      }
      subtree_size[current_id] = sum_size;
      return sum_size;
    }
  }
  for (int i = 0; i < cell_slice.size_refs(); ++i) {
    sum_size += analyze_tree_structure(
      cell_slice.prefetch_ref(i),
      right_tree,
      td::Ref<vm::Cell>(),
      skipped_diffs,
      current_id
    );
  }
  subtree_size[current_id] = sum_size;
  return sum_size;
};

inline void append_uint(td::BitString& bs, unsigned val, unsigned n) {
  bs.reserve_bitslice(n).bits().store_uint(val, n);
}

inline td::Result<unsigned> read_uint(td::BitSlice& bs, int bits) {
  // Check if there enough bits available
  if (bs.size() < bits) {
    return td::Status::Error("BOC decompression failed: not enough bits to read");
  }
  unsigned result = bs.bits().get_uint(bits);
  bs.advance(bits);
  return result;
}

td::HashMap<vm::Cell::Hash, size_t> cur_cell_hashes;

// Helper function to decode DepthBalanceInfo and extract nanograms (manual implementation)
td::RefInt256 extract_balance_from_depth_balance_info_manual(vm::CellSlice& cs) {
  // DepthBalanceInfo structure: split_depth:(#<= 30) balance:CurrencyCollection
  // We need to skip split_depth and extract the grams from CurrencyCollection
  
  int split_depth;
  if (!cs.fetch_uint_leq(30, split_depth)) {
    return td::RefInt256{};
  }
  
  // Now extract grams from CurrencyCollection
  // CurrencyCollection = grams:Grams extra:ExtraCurrencyCollection
  // Grams is VarUInteger 16
  
  int len_bits = (int)cs.prefetch_ulong(4);
  if (!cs.have(4 + len_bits * 8)) {
    return td::RefInt256{};
  }
  
  cs.advance(4);
  if (len_bits == 0) {
    return td::make_refint(0);
  }
  
  auto grams = td::RefInt256{true};
  if (!grams.write().import_bits(cs.data_bits(), len_bits * 8, false)) {
    return td::RefInt256{};
  }
  
  return grams;
}

// Helper function to decode DepthBalanceInfo and extract nanograms (using TLB methods)
td::RefInt256 extract_balance_from_depth_balance_info(vm::CellSlice& cs) {
  // Use the existing TLB unpack method
  int split_depth;
  Ref<vm::CellSlice> balance_cs_ref;
  
  if (!block::gen::t_DepthBalanceInfo.unpack_depth_balance(cs, split_depth, balance_cs_ref)) {
    return td::RefInt256{};
  }
  if (split_depth != 0) {
    return td::RefInt256{};
  }

  if (!cs.empty()) {
    return td::RefInt256{};
  }
  // Extract grams from CurrencyCollection using the TLB method
  auto balance_cs = balance_cs_ref.write();
  auto res = block::tlb::t_Grams.as_integer_skip(balance_cs);
  if (balance_cs.size() != 1) {
    return td::RefInt256{};
  }
  int last_bit = balance_cs.fetch_ulong(1);
  if (last_bit != 0) {
    return td::RefInt256{};
  }
  return res;
}

// Skip the Hashmap label using the existing TLB implementation
bool skip_hashmap_label(vm::CellSlice& cs, int max_bits) {
  int k = cs.fetch_ulong(2);
  if (k != 0) {
    return false;
  }
  return true;
  // return block::gen::HmLabel{max_bits}.skip(cs);
}

// Process ShardAccounts tree vertices and output balance differences
td::RefInt256 process_shard_accounts_vertex(td::Ref<vm::Cell> left, td::Ref<vm::Cell> right) {  
  vm::CellSlice cs_left(NoVm(), left);
  vm::CellSlice cs_right(NoVm(), right);

  bool is_debug = cs_right.as_bitslice().to_hex() == "QQ";
  
  // Skip label on both sides
  if (skip_hashmap_label(cs_left, 256) && skip_hashmap_label(cs_right, 256)) {
    // // Now try to decode DepthBalanceInfo from augmentation values
    auto balance_left = extract_balance_from_depth_balance_info(cs_left);
    auto balance_right = extract_balance_from_depth_balance_info(cs_right);
    
    if (balance_left.not_null() && balance_right.not_null()) {
      // Compute difference: right - left
      td::RefInt256 diff = balance_right;
      diff -= balance_left;

      return diff;
    }
  }
  return td::RefInt256{};
}

// Try to reconstruct ShardAccounts vertex data from left vertex and sum of children's diffs.
// Returns true if reconstructed bits match exactly the original right vertex bits.
bool reconstruct_shard_vertex_and_compare(vm::CellSlice cs_left, vm::CellSlice cs_right, const td::RefInt256& sum_child_diff) {
  // Extract left grams
  if (!skip_hashmap_label(cs_left, 256)) {
    std::cout << "Failed to skip hashmap label" << std::endl;
    return false;
  }
  auto left_grams = extract_balance_from_depth_balance_info(cs_left);
  if (left_grams.is_null()) {
    std::cout << "Failed to extract balance from depth balance info" << std::endl;
    return false;
  }
  // Compute expected right grams = left + sum_child_diff
  td::RefInt256 expected_right_grams = left_grams;
  expected_right_grams += sum_child_diff;

  // Build expected right vertex bits: label '00', split_depth=0 (5 bits), then CurrencyCollection with expected grams and empty extra
  vm::CellBuilder cb;
  if (!cb.store_zeroes_bool(2)) {  // Hashmap label '00'
    std::cout << "Failed to store zeroes bool" << std::endl;
    return false;
  }
  if (!cb.store_zeroes_bool(5)) {  // split_depth:(#<=30) set to 0
    std::cout << "Failed to store zeroes bool" << std::endl;
    return false;
  }
  // Pack CurrencyCollection value (grams) with empty extra
  if (!block::tlb::t_CurrencyCollection.pack_special(cb, expected_right_grams, td::Ref<vm::Cell>())) {
    std::cout << "Failed to pack CurrencyCollection" << std::endl;
    return false;
  }
  td::Ref<vm::Cell> built_cell;
  try {
    built_cell = cb.finalize(false);
  } catch (vm::CellBuilder::CellWriteError&) {
    std::cout << "Failed to finalize cell" << std::endl;
    return false;
  }
  vm::CellSlice built_cs(NoVm(), built_cell);
  if (built_cs.as_bitslice().to_hex() != cs_right.as_bitslice().to_hex()) {
    std::cout << "Built cell does not match right cell" << std::endl;
    std::cout << "Built cell: " << built_cs.as_bitslice().to_binary() << std::endl;
    std::cout << "Right cell: " << cs_right.as_bitslice().to_binary() << std::endl;

    return false;
  }
  return true;
}


td::RefInt256 process_merkle_tree(td::Ref<vm::Cell> left, td::Ref<vm::Cell> right, std::set<vm::Cell::Hash>& skipped_diffs) {
  if (left.is_null() || right.is_null()) {
    return td::RefInt256{};
  }
  vm::CellSlice cs_left(NoVm(), left);
  vm::CellSlice cs_right(NoVm(), right);


  if (cs_left.special_type() == vm::CellTraits::SpecialType::PrunnedBranch) {
    return td::RefInt256{};
  }
  if (cs_right.special_type() == vm::CellTraits::SpecialType::PrunnedBranch) {
    return td::RefInt256{};
  }

  td::RefInt256 diff = process_shard_accounts_vertex(left, right);
  
  td::RefInt256 sum_child_diff = td::make_refint(0);
  for (unsigned i = 0; i < cs_right.size_refs(); i++) {
    auto diff_rec = process_merkle_tree(cs_left.prefetch_ref(i), cs_right.prefetch_ref(i), skipped_diffs);
    if (diff_rec.not_null()) {
      sum_child_diff += diff_rec;
    }
  }
  if (diff.not_null() && sum_child_diff.not_null() && cmp(sum_child_diff, diff) == 0) {
    // Verify we can reconstruct this vertex exactly from left vertex and children's diffs
    if (reconstruct_shard_vertex_and_compare(cs_left, cs_right, sum_child_diff)) {
      skipped_diffs.insert(right->get_hash());
    } else {
      exit(1);
    }
  }
  return diff;
};

int K = 0;
// Traverse a single tree and, for each vertex, compute the sum of children's values and
// compare it with the vertex's own value. If equal, add the vertex hash to balanced_vertices.
// Returns the computed "value" of the current vertex to allow parent accumulation.
td::RefInt256 process_single_vertex_sums(td::Ref<vm::Cell> node, std::set<vm::Cell::Hash>& balanced_vertices) {
  if (node.is_null()) {
    return td::RefInt256{};
  }

  vm::CellSlice cs(NoVm(), node);
  if (cs.special_type() == vm::CellTraits::SpecialType::PrunnedBranch) {
    return td::RefInt256{};
  }

  // Compute own value (ShardAccounts augmentation grams)
  vm::CellSlice cs_value = cs;
  td::RefInt256 own_value;
  if (skip_hashmap_label(cs_value, 256)) {
    own_value = extract_balance_from_depth_balance_info(cs_value);
  }

  // Accumulate children's values
  td::RefInt256 sum_children = td::make_refint(0);
  for (unsigned i = 0; i < cs.size_refs(); i++) {
    auto child = cs.prefetch_ref(i);
    td::RefInt256 child_val = process_single_vertex_sums(child, balanced_vertices);
    if (child_val.not_null()) {
        sum_children += child_val;
    }
  }

  // If own value exists and equals sum of children, mark node
  if (own_value.not_null() && own_value->to_dec_string() != "0" && td::cmp(sum_children, own_value) == 0) {
    balanced_vertices.insert(node->get_hash());
  }
  return own_value;
} 

td::Result<td::BufferSlice> boc_compress_improved_structure_lz4(
  const std::vector<td::Ref<vm::Cell>>& boc_roots, 
  bool compress_merkle_update,
  td::Ref<vm::Cell> state
) {
  const bool kMURemoveSubtreeSums = true;
  
  // Input validation
  if (boc_roots.empty()) {
    return td::Status::Error("No root cells were provided for serialization");
  }
  for (const auto& root : boc_roots) {
    if (root.is_null()) {
      return td::Status::Error("Cannot serialize a null cell reference into a bag of cells");
    }
  }

  // Initialize data structures for graph representation
  td::HashMap<vm::Cell::Hash, size_t> cell_hashes;
  std::vector<std::array<size_t, 4>> boc_graph;
  std::vector<size_t> refs_cnt;
  std::vector<td::BitSlice> cell_data;
  std::vector<size_t> cell_type;
  std::vector<size_t> prunned_branch_level;
  std::vector<size_t> root_indexes;
  size_t total_size_estimate = 0;
  std::set<vm::Cell::Hash> skipped_diffs;
  std::set<vm::Cell::Hash> balanced_vertices;
  // Accumulates 32-byte (256-bit) SHA256 hashes from prunned branches
  td::BitString prunned_branch_hashes_accumulator;

  // Precompute balanced vertices (own value equals sum of children's values) across all roots
  // for (const auto& root : boc_roots) {
  //   process_single_vertex_sums(root, balanced_vertices);
  // }

  // When enabled, collect mapping from (hash at merkle depth) to real state cell
  // while traversing the left subtree of a MerkleUpdate cell together with full state
  td::HashMap<vm::Cell::Hash, td::Ref<vm::Cell>> mu_known_cells;

  // Build graph representation using recursive lambda
  const auto build_graph = [&](auto&& self,
                               td::Ref<vm::Cell> cell,
                               bool under_mu_left = false,
                               bool under_mu_right = false) -> td::Result<size_t> {
    if (cell.is_null()) {
      return td::Status::Error("Error while importing a cell during serialization: cell is null");
    }

    auto cell_hash = cell->get_hash();
    bool is_special = false;
    vm::CellSlice cell_slice = vm::load_cell_slice_special(cell, is_special);
    
    auto it = cell_hashes.find(cell_hash);
    if (it != cell_hashes.end()) {
      return it->second;
    }

    size_t current_cell_id = boc_graph.size();
    cell_hashes.emplace(cell_hash, current_cell_id);

  
    if (!cell_slice.is_valid()) {
      return td::Status::Error("Invalid loaded cell data");
    }
    td::BitSlice cell_bitslice = cell_slice.as_bitslice();

    // Initialize new cell in graph
    boc_graph.emplace_back();
    refs_cnt.emplace_back(cell_slice.size_refs());
    cell_type.emplace_back(size_t(cell_slice.special_type()));
    prunned_branch_level.push_back(0);

    DCHECK(cell_slice.size_refs() <= 4);

    // Process special cell of type PrunnedBranch
    if (kMURemoveSubtreeSums && skipped_diffs.find(cell_hash) != skipped_diffs.end()) {
      cell_data.emplace_back();
      prunned_branch_level.back() = 9;
    } else if (balanced_vertices.find(cell_hash) != balanced_vertices.end()) {
      cell_data.emplace_back();
      prunned_branch_level.back() = 10;
    } else if (compress_merkle_update && under_mu_left) {
      cell_data.emplace_back();
    } else {
      if (cell_slice.special_type() == vm::CellTraits::SpecialType::PrunnedBranch) {
        DCHECK(cell_slice.size() >= 16);
        auto tail = cell_bitslice.subslice(16, cell_bitslice.size() - 16);
        cell_data.emplace_back(tail);
        // prunned_branch_hashes_accumulator.append(tail);
        prunned_branch_level.back() = cell_slice.data()[1];
      } else {
        cell_data.emplace_back(cell_bitslice);
      }
    }
    total_size_estimate += cell_bitslice.size();

    // If enabled and this is a MerkleUpdate cell, find replacable subtree sums
    if (kMURemoveSubtreeSums && cell_slice.special_type() == vm::CellTraits::SpecialType::MerkleUpdate) {
      process_merkle_tree(cell_slice.prefetch_ref(0), cell_slice.prefetch_ref(1), skipped_diffs);
    }

    // if (cell_slice.special_type() == vm::CellTraits::SpecialType::MerkleUpdate) {
    //   std::cout << "real L: " << analyze_tree_structure(cell_slice.prefetch_ref(0), false, td::Ref<vm::Cell>(), skipped_diffs)  / 8 << std::endl;
    //   // clear_cache();
    //   std::cout << "R with L in cache: " << analyze_tree_structure(cell_slice.prefetch_ref(1), true, cell_slice.prefetch_ref(0), skipped_diffs)  / 8 << std::endl;
    //   print_tree_structure();
    //   // exit(0);
    // }

    // Process cell references
    for (int i = 0; i < cell_slice.size_refs(); ++i) {
      bool is_mu = (cell_slice.special_type() == vm::CellTraits::SpecialType::MerkleUpdate);
      bool child_under_mu_left = under_mu_left || (is_mu && i == 0);
      bool child_under_mu_right = under_mu_right || (is_mu && i == 1);
      TRY_RESULT(child_id, self(self, cell_slice.prefetch_ref(i), child_under_mu_left, child_under_mu_right));
      boc_graph[current_cell_id][i] = child_id;
    }

    return current_cell_id;
  };

  // Build the graph starting from roots
  for (auto root : boc_roots) {
    TRY_RESULT(root_cell_id, build_graph(build_graph, root, false, false));
    root_indexes.push_back(root_cell_id);
  }

  // Check graph properties
  const size_t node_count = boc_graph.size();
  std::vector<std::vector<size_t>> reverse_graph(node_count);
  size_t edge_count = 0;

  // Build reverse graph
  for (int i = 0; i < node_count; ++i) {
    for (size_t child_index = 0; child_index < refs_cnt[i]; ++child_index) {
      size_t child = boc_graph[i][child_index];
      ++edge_count;
      reverse_graph[child].push_back(i);
    }
  }

  // Process cell data sizes
  std::vector<size_t> is_data_small(node_count, 0);
  for (int i = 0; i < node_count; ++i) {
    if (cell_type[i] != 1) {
      is_data_small[i] = cell_data[i].size() < 128;
    }
  }

  // Perform topological sort
  std::vector<size_t> topo_order, rank(node_count);
  const auto topological_sort = [&]() -> td::Status {
    std::vector<std::tuple<int, int, int>> queue;
    queue.reserve(node_count);
    std::vector<size_t> in_degree(node_count);

    // Calculate in-degrees and initialize queue
    for (int i = 0; i < node_count; ++i) {
      in_degree[i] = refs_cnt[i];
      if (in_degree[i] == 0) {
        queue.emplace_back(cell_type[i] == 0, -int(cell_data[i].size()), -i);
      }
    }

    if (queue.empty()) {
      return td::Status::Error("Cycle detected in cell references");
    }

    std::sort(queue.begin(), queue.end());

    // Process queue
    while (!queue.empty()) {
      int node = -std::get<2>(queue.back());
      queue.pop_back();
      topo_order.push_back(node);

      for (int parent : reverse_graph[node]) {
        if (--in_degree[parent] == 0) {
          queue.emplace_back(0, 0, -parent);
        }
      }
    }

    if (topo_order.size() != node_count) {
      return td::Status::Error("Invalid graph structure");
    }

    std::reverse(topo_order.begin(), topo_order.end());
    return td::Status::OK();
  };

  TRY_STATUS(topological_sort());

  // Calculate index of vertices in topsort
  for (int i = 0; i < node_count; ++i) {
    rank[topo_order[i]] = i;
  }

  // Build compressed representation
  td::BitString result;
  total_size_estimate += (node_count * 10 * 8);
  result.reserve_bits(total_size_estimate);

  // Store roots information
  append_uint(result, root_indexes.size(), 32);
  for (int root_ind : root_indexes) {
    append_uint(result, rank[root_ind], 32);
  }

  // Store node count
  append_uint(result, node_count, 32);

  // Store cell types and sizes
  for (int i = 0; i < node_count; ++i) {
    size_t node = topo_order[i];
    size_t currrent_cell_type = bool(cell_type[node]) + prunned_branch_level[node];
    append_uint(result, currrent_cell_type, 4);
    append_uint(result, refs_cnt[node], 4);

    if (cell_type[node] != 1) {
      if (is_data_small[node]) {
        append_uint(result, 1, 1);
        append_uint(result, cell_data[node].size(), 7);
      } else {
        append_uint(result, 0, 1);
        append_uint(result, 1 + cell_data[node].size() / 8, 7);
      }
    }
  }

  // Store edge information
  auto edge_bits = result.reserve_bitslice(edge_count).bits();
  for (int i = 0; i < node_count; ++i) {
    size_t node = topo_order[i];
    for (size_t child_index = 0; child_index < refs_cnt[node]; ++child_index) {
      size_t child = boc_graph[node][child_index];
      edge_bits.store_uint(rank[child] == i + 1, 1);
      ++edge_bits;
    }
  }

  // Store cell data
  for (size_t node : topo_order) {
    if (cell_type[node] != 1 && !is_data_small[node]) {
      continue;
    }
    result.append(cell_data[node].subslice(0, cell_data[node].size() % 8));
  }

  // Store BOC graph with optimized encoding
  for (size_t i = 0; i < node_count; ++i) {
    size_t node = topo_order[i];
    if (node_count <= i + 3)
      continue;

    for (int j = 0; j < refs_cnt[node]; ++j) {
      if (rank[boc_graph[node][j]] <= i + 1)
        continue;

      int delta = rank[boc_graph[node][j]] - i - 2; // Always >= 0 because of above check
      size_t required_bits = 1 + (31 ^ td::count_leading_zeroes32(node_count - i - 3));

      if (required_bits < 8 - (result.size() + 1) % 8 + 1) {
        append_uint(result, delta, required_bits);
      } else if (delta < (1 << (8 - (result.size() + 1) % 8))) {
        size_t available_bits = 8 - (result.size() + 1) % 8;
        append_uint(result, 1, 1);
        append_uint(result, delta, available_bits);
      } else {
        append_uint(result, 0, 1);
        append_uint(result, delta, required_bits);
      }
    }
  }

  // Pad result to byte boundary
  while (result.size() % 8) {
    append_uint(result, 0, 1);
  }

  // Store remaining cell data
  for (size_t node : topo_order) {
    if (cell_type[node] == 1 || is_data_small[node]) {
      size_t prefix_size = cell_data[node].size() % 8;
      result.append(cell_data[node].subslice(prefix_size, cell_data[node].size() - prefix_size));
    } else {
      size_t data_size = cell_data[node].size() + 1;
      size_t padding = (8 - data_size % 8) % 8;

      if (padding) {
        append_uint(result, 0, padding);
      }
      append_uint(result, 1, 1);
      result.append(cell_data[node]);
    }
  }

  // Final padding
  while (result.size() % 8) {
    append_uint(result, 0, 1);
  }

  // Create final compressed buffer
  // Append accumulated prunned branch hashes to the end before LZ4 compression
  if (prunned_branch_hashes_accumulator.size() > 0) {
    result.append(prunned_branch_hashes_accumulator);
  }
  td::BufferSlice serialized((const char*)result.bits().get_byte_ptr(), result.size() / 8);

  td::BufferSlice compressed = td::lz4_compress(serialized);

  // Add decompressed size at the beginning
  td::BufferSlice compressed_with_size(compressed.size() + kDecompressedSizeBytes);
  auto size_slice = td::BitSliceWrite(compressed_with_size.as_slice().ubegin(), kDecompressedSizeBytes * 8);
  size_slice.bits().store_uint(serialized.size(), kDecompressedSizeBytes * 8);
  memcpy(compressed_with_size.data() + kDecompressedSizeBytes, compressed.data(), compressed.size());

  return compressed_with_size;
}

td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress_improved_structure_lz4(td::Slice compressed, int max_decompressed_size) {
  constexpr size_t kMaxCellDataLengthBits = 1024;

  // Check minimum input size for decompressed size header
  if (compressed.size() < kDecompressedSizeBytes) {
    return td::Status::Error("BOC decompression failed: input too small for header");
  }

  // Read decompressed size
  constexpr size_t kSizeBits = kDecompressedSizeBytes * 8;
  size_t decompressed_size = td::BitSlice(compressed.ubegin(), kSizeBits).bits().get_uint(kSizeBits);
  compressed.remove_prefix(kDecompressedSizeBytes);
  if (decompressed_size > max_decompressed_size) {
    return td::Status::Error("BOC decompression failed: invalid decompressed size");
  }

  // Decompress LZ4 data
  TRY_RESULT(serialized, td::lz4_decompress(compressed, decompressed_size));

  if (serialized.size() != decompressed_size) {
    return td::Status::Error("BOC decompression failed: decompressed size mismatch");
  }

  // Initialize bit reader
  td::BitSlice bit_reader(serialized.as_slice().ubegin(), serialized.as_slice().size() * 8);
  size_t orig_size = bit_reader.size();

  // Read root count
  TRY_RESULT(root_count, read_uint(bit_reader, 32));
  // We assume that each cell should take at least 1 byte, even effectively serialized
  // Otherwise it means that provided root_count is incorrect
  if (root_count < 1 || root_count > decompressed_size) {
    return td::Status::Error("BOC decompression failed: invalid root count");
  }

  std::vector<size_t> root_indexes(root_count);
  for (int i = 0; i < root_count; ++i) {
    TRY_RESULT_ASSIGN(root_indexes[i], read_uint(bit_reader, 32));
  }

  // Read number of nodes from header
  TRY_RESULT(node_count, read_uint(bit_reader, 32));
  if (node_count < 1) {
    return td::Status::Error("BOC decompression failed: invalid node count");
  }

  // We assume that each cell should take at least 1 byte, even effectively serialized
  // Otherwise it means that provided node_count is incorrect
  if (node_count > decompressed_size) {
    return td::Status::Error("BOC decompression failed: incorrect node count provided");
  }


  // Validate root indexes
  for (int i = 0; i < root_count; ++i) {
    if (root_indexes[i] >= node_count) {
      return td::Status::Error("BOC decompression failed: invalid root index");
    }
  }

  // Initialize data structures
  std::vector<size_t> cell_data_length(node_count), is_data_small(node_count);
  std::vector<size_t> is_special(node_count), cell_refs_cnt(node_count);
  std::vector<size_t> prunned_branch_level(node_count, 0);

  std::vector<vm::CellBuilder> cell_builders(node_count);
  std::vector<std::array<size_t, 4>> boc_graph(node_count);

  // Read cell metadata
  for (int i = 0; i < node_count; ++i) {
    // Check enough bits for cell type and refs count
    if (bit_reader.size() < 8) {
      return td::Status::Error("BOC decompression failed: not enough bits for cell metadata");
    }

    size_t cell_type = bit_reader.bits().get_uint(4);
    is_special[i] = bool(cell_type);
    if (is_special[i]) {
      prunned_branch_level[i] = cell_type - 1;
    }
    bit_reader.advance(4);

    cell_refs_cnt[i] = bit_reader.bits().get_uint(4);
    bit_reader.advance(4);
    if (cell_refs_cnt[i] > 4) {
      return td::Status::Error("BOC decompression failed: invalid cell refs count");
    }

    if (prunned_branch_level[i]) {
      size_t coef = std::bitset<4>(prunned_branch_level[i]).count();
      cell_data_length[i] = (256 + 16) * coef;
    } else {
      // Check enough bits for data length metadata
      if (bit_reader.size() < 8) {
        return td::Status::Error("BOC decompression failed: not enough bits for data length");
      }

      is_data_small[i] = bit_reader.bits().get_uint(1);
      bit_reader.advance(1);
      cell_data_length[i] = bit_reader.bits().get_uint(7);
      bit_reader.advance(7);

      if (!is_data_small[i]) {
        cell_data_length[i] *= 8;
        if (!cell_data_length[i]) {
          cell_data_length[i] += 1024;
        }
      }
    }

    // Validate cell data length
    if (cell_data_length[i] > kMaxCellDataLengthBits) {
      return td::Status::Error("BOC decompression failed: invalid cell data length");
    }
  }

  // Read direct edge connections
  for (int i = 0; i < node_count; ++i) {
    for (int j = 0; j < cell_refs_cnt[i]; ++j) {
      TRY_RESULT(edge_connection, read_uint(bit_reader, 1));
      if (edge_connection) {
        boc_graph[i][j] = i + 1;
      }
    }
  }

  // Read initial cell data
  for (int i = 0; i < node_count; ++i) {
    if (prunned_branch_level[i]) {
      cell_builders[i].store_long((1 << 8) + prunned_branch_level[i], 16);
    }

    size_t remainder_bits = cell_data_length[i] % 8;
    if (bit_reader.size() < remainder_bits) {
      return td::Status::Error("BOC decompression failed: not enough bits for initial cell data");
    }
    cell_builders[i].store_bits(bit_reader.subslice(0, remainder_bits));
    bit_reader.advance(remainder_bits);
    cell_data_length[i] -= remainder_bits;
  }

  // Decode remaining edge connections
  for (size_t i = 0; i < node_count; ++i) {
    if (node_count <= i + 3) {
      for (int j = 0; j < cell_refs_cnt[i]; ++j) {
        if (!boc_graph[i][j]) {
          boc_graph[i][j] = i + 2;
        }
      }
      continue;
    }

    for (int j = 0; j < cell_refs_cnt[i]; ++j) {
      if (!boc_graph[i][j]) {
        size_t pref_size = (orig_size - bit_reader.size());
        size_t required_bits = 1 + (31 ^ td::count_leading_zeroes32(node_count - i - 3));

        if (required_bits < 8 - (pref_size + 1) % 8 + 1) {
          TRY_RESULT_ASSIGN(boc_graph[i][j], read_uint(bit_reader, required_bits));
          boc_graph[i][j] += i + 2;
        } else {
          TRY_RESULT(edge_connection, read_uint(bit_reader, 1));
          if (edge_connection) {
            pref_size = (orig_size - bit_reader.size());
            size_t available_bits = 8 - pref_size % 8;
            TRY_RESULT_ASSIGN(boc_graph[i][j], read_uint(bit_reader, available_bits));
            boc_graph[i][j] += i + 2;
          } else {
            TRY_RESULT_ASSIGN(boc_graph[i][j], read_uint(bit_reader, required_bits));
            boc_graph[i][j] += i + 2;
          }
        }
      }
    }
  }

  // Check if all graph connections are valid
  for (int node = 0; node < node_count; ++node) {
    for (int j = 0; j < cell_refs_cnt[node]; ++j) {
      size_t child_node = boc_graph[node][j];
      if (child_node >= node_count) {
        return td::Status::Error("BOC decompression failed: invalid graph connection");
      }
      if (child_node <= node) {
        return td::Status::Error("BOC decompression failed: circular reference in graph");
      }
    }
  }

  // Align to byte boundary
  while ((orig_size - bit_reader.size()) % 8) {
    TRY_RESULT(bit, read_uint(bit_reader, 1));
  }

  // Read remaining cell data
  for (int i = 0; i < node_count; ++i) {
    size_t padding_bits = 0;
    if (!prunned_branch_level[i] && !is_data_small[i]) {
      while (bit_reader.size() > 0 && bit_reader.bits()[0] == 0) {
        ++padding_bits;
        bit_reader.advance(1);
      }
      TRY_RESULT(bit, read_uint(bit_reader, 1));
      ++padding_bits;
    }
    if (cell_data_length[i] < padding_bits) {
      return td::Status::Error("BOC decompression failed: invalid cell data length");
    }
    size_t remaining_data_bits = cell_data_length[i] - padding_bits;
    if (bit_reader.size() < remaining_data_bits) {
      return td::Status::Error("BOC decompression failed: not enough bits for remaining cell data");
    }

    cell_builders[i].store_bits(bit_reader.subslice(0, remaining_data_bits));
    bit_reader.advance(remaining_data_bits);
  }

  // Build cell tree
  std::vector<td::Ref<vm::Cell>> nodes(node_count);
  for (int i = node_count - 1; i >= 0; --i) {
    try {
      for (int child_index = 0; child_index < cell_refs_cnt[i]; ++child_index) {
        size_t child = boc_graph[i][child_index];
        cell_builders[i].store_ref(nodes[child]);
      }
      try {
        nodes[i] = cell_builders[i].finalize(is_special[i]);
      } catch (vm::CellBuilder::CellWriteError& e) {
        return td::Status::Error("BOC decompression failed: write error while finalizing cell.");
      }
    } catch (vm::VmError& e) {
      return td::Status::Error("BOC decompression failed: VM error during cell construction");
    }
  }

  std::vector<td::Ref<vm::Cell>> root_nodes;
  root_nodes.reserve(root_count);
  for (size_t index : root_indexes) {
    root_nodes.push_back(nodes[index]);
  }

  return root_nodes;
}

td::Result<td::BufferSlice> boc_compress(const std::vector<td::Ref<vm::Cell>>& boc_roots, CompressionAlgorithm algo, td::Ref<vm::Cell> state) {
  // Check for empty input
  if (boc_roots.empty()) {
    return td::Status::Error("Cannot compress empty BOC roots");
  }

  td::BufferSlice compressed;
  if (algo == CompressionAlgorithm::BaselineLZ4) {
    TRY_RESULT_ASSIGN(compressed, boc_compress_baseline_lz4(boc_roots));
  } else if (algo == CompressionAlgorithm::ImprovedStructureLZ4) {
    TRY_RESULT_ASSIGN(compressed, boc_compress_improved_structure_lz4(boc_roots, false));
  } else if (algo == CompressionAlgorithm::ImprovedStructureLZ4WithMU) {
    TRY_RESULT_ASSIGN(compressed, boc_compress_improved_structure_lz4(boc_roots, true, state));
  } else {
      return td::Status::Error("Unknown compression algorithm");
  }

  td::BufferSlice compressed_with_algo(compressed.size() + 1);
  compressed_with_algo.data()[0] = int(algo);
  memcpy(compressed_with_algo.data() + 1, compressed.data(), compressed.size());
  return compressed_with_algo;
}

td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress(td::Slice compressed, int max_decompressed_size) {
  if (compressed.size() == 0) {
    return td::Status::Error("Can't decompress empty data");
  }

  int algo = int(compressed[0]);
  compressed.remove_prefix(1);

  switch (algo) {
    case int(CompressionAlgorithm::BaselineLZ4):
      return boc_decompress_baseline_lz4(compressed, max_decompressed_size);
    case int(CompressionAlgorithm::ImprovedStructureLZ4):
      return boc_decompress_improved_structure_lz4(compressed, max_decompressed_size);
    case int(CompressionAlgorithm::ImprovedStructureLZ4WithMU):
      return boc_decompress_improved_structure_lz4(compressed, max_decompressed_size);
  }
  return td::Status::Error("Unknown compression algorithm");
}

}  // namespace vm
