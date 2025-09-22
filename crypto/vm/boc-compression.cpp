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
#include "openssl/digest.hpp"
#include <fstream>

namespace vm {

CellHash get_cell_data_hash(td::BitSlice data) {
  digest::SHA256 hasher;
  td::BitString data_bits;
  data_bits.reserve_bitslice(data.size()) = data;
  while (data_bits.size() % 8) {
    data_bits.reserve_bitslice(1).bits().store_uint(0, 1);
  }
  if (data_bits.bits().offs != 0) exit(199);
  hasher.feed(data_bits.bits().get_byte_ptr(), data_bits.size() / 8);
  CellHash hash_;
  hasher.extract(hash_.as_slice());
  return hash_;
}

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

std::map<int, int> cnt_types;
int TESTS = 0;
td::HashMap<vm::Cell::Hash, size_t> local_cache;
td::HashMap<std::string, size_t> data_cache;
int cnt_v = 0;
std::vector<int> sourced_vertices;
std::vector<int> matching_data_vertices;
std::vector<std::pair<int, int>> direct_edges;
std::vector<std::pair<int, int>> back_edges;
std::map<int, std::string> vertex_data;
std::map<int, int> subtree_size;

void clr(bool clear_cache = true) {
  if (clear_cache) {
    local_cache.clear();
    data_cache.clear();
  }
  // cnt_v = 0;
  // sourced_vertices.clear();
  // direct_edges.clear();
  back_edges.clear();
  cnt_types.clear();
  TESTS = 0;
}

void print_edges() {
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

int calc_sub_size_local_cache(td::Ref<vm::Cell> cell, bool right_tree, td::Ref<vm::Cell> left_cell, int pr = -1) {
  // std::cout << data_cache.size() << std::endl;
  auto cell_hash = cell->get_hash();
  auto cache_it = local_cache.find(cell_hash);
  int current_id = cnt_v++;
  if (pr != -1) {
    direct_edges.push_back({pr, current_id});
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
  if (left_cell != td::Ref<vm::Cell>()) {
    auto left_cell_hash = left_cell->get_hash();
    auto it_left = local_cache.find(left_cell_hash);
    if (it_left == local_cache.end()) {
      exit(228);
    }
    int matched_id = it_left->second;
    matching_data_vertices.emplace_back(matched_id);
    matching_data_vertices.emplace_back(current_id);
  }

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
  if (right_tree) {
    for (int i = 1; i <= cell_bitslice.size(); ++i) {
      std::string hash = cell_bitslice.subslice(0, i).to_binary();
      if (data_cache.find(hash) != data_cache.end()) {
        longest_common_prefix = i;
      }
    }
  }

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
  sum_size += (cell_bitslice.size() - longest_common_prefix);
  if (cell_slice.special_type() == vm::CellTraits::SpecialType::PrunnedBranch) {
    sum_size -= 16;
  }

  // 7801
  // Process cell references
  if (left_cell != td::Ref<vm::Cell>()) {
    vm::CellSlice left_cell_slice = vm::load_cell_slice_special(left_cell, is_special);
    if (left_cell_slice.size_refs() == cell_slice.size_refs()) {
      for (int i = 0; i < cell_slice.size_refs(); ++i) {
        sum_size += calc_sub_size_local_cache(
          cell_slice.prefetch_ref(i),
          right_tree,
          left_cell_slice.prefetch_ref(i),
          current_id
        );
      }
      subtree_size[current_id] = sum_size;
      return sum_size;
    }
  }
  for (int i = 0; i < cell_slice.size_refs(); ++i) {
    sum_size += calc_sub_size_local_cache(cell_slice.prefetch_ref(i), right_tree, td::Ref<vm::Cell>(), current_id);
  }
  subtree_size[current_id] = sum_size;
  return sum_size;
};

std::map<int, int> copy_data;


const int MAX_N = 10000;
std::map<int, int> mu_right_data_sizes;
td::Result<td::BufferSlice> boc_compress_improved_structure_lz4(const std::vector<td::Ref<vm::Cell>>& boc_roots) {
  auto left_restored = boc_roots[1];
  local_cache.clear();
  // mu_right_data_sizes.clear();
  // Input validation
  if (boc_roots.empty()) {
    return td::Status::Error("No root cells were provided for serialization");
  }
  if (boc_roots.size() > BagOfCells::default_max_roots) {
    return td::Status::Error("Too many root cells were provided for serialization");
  }
  for (const auto& root : boc_roots) {
    if (root.is_null()) {
      return td::Status::Error("Cannot serialize a null cell reference into a bag of cells");
    }
  }
  int counter = MAX_N;
  // Initialize data structures for graph representation
  td::HashMap<vm::Cell::Hash, size_t> cell_hashes;
  td::HashMap<std::string, size_t> mu_left_cell_data_hashes;
  std::vector<std::array<size_t, 4>> boc_graph;
  std::vector<size_t> refs_cnt;
  std::vector<td::BitSlice> cell_data;
  std::vector<size_t> cell_type;
  std::vector<size_t> prunned_branch_level;
  std::vector<size_t> root_indexes;
  size_t total_size_estimate = 0;
  int total = 0, rem = 0, prunned = 0;
  // Build graph representation using recursive lambda
  const auto build_graph = [&](auto&& self, td::Ref<vm::Cell> cell, bool is_merkle_left, bool is_merkle_right) -> td::Result<size_t> {
    if (cell.is_null()) {
      return td::Status::Error("Error while importing a cell during serialization: cell is null");
    }

    auto cell_hash = cell->get_hash();
    auto it = cell_hashes.find(cell_hash);
    if (it != cell_hashes.end()) {
      return it->second;
    }

    size_t current_cell_id = boc_graph.size();
    cell_hashes.emplace(cell_hash, current_cell_id);

    bool is_special = false;
    vm::CellSlice cell_slice = vm::load_cell_slice_special(cell, is_special);
    if (!cell_slice.is_valid()) {
      return td::Status::Error("Invalid loaded cell data");
    }
    td::BitSlice cell_bitslice = cell_slice.as_bitslice();

    if (is_merkle_left) {
      for (int i = 1; i <= cell_bitslice.size(); ++i) {
        std::string hash = cell_bitslice.subslice(0, i).to_binary();
        mu_left_cell_data_hashes[hash] = current_cell_id;
      }
    }
    int longest_common_prefix = 0;
    if (is_merkle_right) {
      for (int i = 1; i <= cell_bitslice.size(); ++i) {
        std::string hash = cell_bitslice.subslice(0, i).to_binary();
        if (mu_left_cell_data_hashes.find(hash) != mu_left_cell_data_hashes.end()) {
          longest_common_prefix = i;
        }
      }
      if (longest_common_prefix >= 16) {
        longest_common_prefix = cell_bitslice.size();
      }
    }

    // Initialize new cell in graph
    boc_graph.emplace_back();
    refs_cnt.emplace_back(cell_slice.size_refs());
    cell_type.emplace_back(size_t(cell_slice.special_type()));
    prunned_branch_level.push_back(0);


    DCHECK(cell_slice.size_refs() <= 4);
    if (cell_slice.special_type() == vm::CellTraits::SpecialType::MerkleUpdate) {
      clr();
      // std::cout << "real L: " << calc_sub_size_local_cache(cell_slice.prefetch_ref(0), false, td::Ref<vm::Cell>())  / 8 << std::endl;
      std::cout << "real L: " << calc_sub_size_local_cache(left_restored, false, td::Ref<vm::Cell>())  / 8 << std::endl;
      // print_edges();
      clr(false);
      // std::cout << "R with L in cache: " << calc_sub_size_local_cache(cell_slice.prefetch_ref(1), true, cell_slice.prefetch_ref(0))  / 8 << std::endl;
      std::cout << "R with L in cache: " << calc_sub_size_local_cache(cell_slice.prefetch_ref(1), true, left_restored)  / 8 << std::endl;
      print_edges();
      // local_cache.clear();
      // local_cache2.clear();
      // std::cout << "real R: " << calc_sub_size_local_cache(cell_slice.prefetch_ref(1))  / 8 << std::endl;
      exit(0);
    }

    // Process special cell of type PrunnedBranch
    if (is_merkle_left) {
      cell_data.emplace_back();
    } else {
      if (cell_slice.special_type() == vm::CellTraits::SpecialType::PrunnedBranch) {
        DCHECK(cell_slice.size() >= 16);
        if (is_merkle_right) {
          cell_data.emplace_back();
        } else {
          cell_data.emplace_back(cell_bitslice.subslice(16, cell_bitslice.size() - 16));
        }
        prunned_branch_level.back() = cell_slice.data()[1];
      } else {
        cell_data.emplace_back(cell_bitslice.subslice(longest_common_prefix, cell_bitslice.size() - longest_common_prefix));
        if (is_merkle_right) {
          mu_right_data_sizes[cell_bitslice.size()]++;
        }
      }
    }
    // if (cell_slice.special_type() == vm::CellTraits::SpecialType::PrunnedBranch) {
    //   DCHECK(cell_slice.size() >= 16);
    //   cell_data.emplace_back(cell_bitslice.subslice(16, cell_bitslice.size() - 16));
    //   prunned_branch_level.back() = cell_slice.data()[1];
    // } else {
    //   cell_data.emplace_back(cell_bitslice);
    // }

    total_size_estimate += cell_bitslice.size();

    // Process cell references
    int skipped = 0;
    for (int i = 0; i < cell_slice.size_refs(); ++i) {
      int child_id;
      if (cell_slice.special_type() == vm::CellTraits::SpecialType::MerkleUpdate && i == 0) {
        is_merkle_left = true;
        TRY_RESULT_ASSIGN(child_id, self(self, cell_slice.prefetch_ref(i), is_merkle_left, is_merkle_right));
        is_merkle_left = false;
        // skipped++;
        // refs_cnt.back()--;
        // continue;
      } else if (cell_slice.special_type() == vm::CellTraits::SpecialType::MerkleUpdate && i == 1) {
        is_merkle_right = true;
        TRY_RESULT_ASSIGN(child_id, self(self, cell_slice.prefetch_ref(i), is_merkle_left, is_merkle_right));
        is_merkle_right = false;
        // skipped++;
        // refs_cnt.back()--;
        // continue;
      } else {
        TRY_RESULT_ASSIGN(child_id, self(self, cell_slice.prefetch_ref(i), is_merkle_left, is_merkle_right));
      }
      boc_graph[current_cell_id][i - skipped] = child_id;
    }

    return current_cell_id;
  };

  // Build the graph starting from roots
  for (auto root : boc_roots) {
    TRY_RESULT(root_cell_id, build_graph(build_graph, root, 0, 0));
    root_indexes.push_back(root_cell_id);
    std::cout << "CAREFUL!!!!!! BREAK AFTER THE FIRST ROOT IN BOC" << std::endl;
    break;
  }
  // std::cout << "-----------: " << total << " " << rem << " " << prunned << std::endl;
  // if (TESTS == 200) {
  //   for (auto c : cnt_types) {
  //     std::cout << c.first << " " << c.second  / TESTS << std::endl;
  //   }
  // }
  // for (auto c : mu_right_data_sizes) {
  //   if (c.second > 1000)
  //   std::cout << c.first << " " << c.second << std::endl;
  // }
  // std::cout << "==========================" << std::endl;

  // Check graph properties
  const size_t node_count = boc_graph.size();
  std::vector<std::vector<size_t>> reverse_graph(node_count);
  size_t edge_count = 0;

  // Build reverse graph
  for (int i = 0; i < node_count; ++i) {
    for (size_t child_index = 0; child_index < refs_cnt[i]; ++child_index) {
      size_t child = boc_graph[i][child_index];
      ++edge_count;
      if (child < MAX_N)
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
      for (int to : boc_graph[i]) {
        if (to >= MAX_N) --in_degree[i];
      }
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
      return td::Status::Error("Invalid graph structure node count");
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
  if (root_indexes.size() >= (1 << 16)) {
    return td::Status::Error("Too many root cells");
  }
  append_uint(result, root_indexes.size(), 16);
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

      int delta = rank[boc_graph[node][j]] - i - 2;  // Always >= 0 because of above check
      size_t required_bits = 1 + (31 ^ __builtin_clz(node_count - i - 3));

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
  td::BufferSlice serialized((const char*)result.bits().get_byte_ptr(), result.size() / 8);

  td::BufferSlice compressed = td::lz4_compress(serialized);

  // Add decompressed size at the beginning
  td::BufferSlice compressed_with_size(compressed.size() + kDecompressedSizeBytes);
  auto size_slice = td::BitSliceWrite(compressed_with_size.as_slice().ubegin(), kDecompressedSizeBytes * 8);
  size_slice.bits().store_uint(serialized.size(), kDecompressedSizeBytes * 8);
  memcpy(compressed_with_size.data() + kDecompressedSizeBytes, compressed.data(), compressed.size());

  return compressed_with_size;
}



td::Status cache_cell_hashes(const std::vector<td::Ref<vm::Cell>>& boc_roots,
                             td::HashMap<vm::Cell::Hash, size_t>& cache,
                             std::vector<td::Ref<vm::Cell>>& hash_by_ind) {
  const auto dfs = [&](auto&& self, td::Ref<vm::Cell> cell) -> td::Result<size_t> {
    if (cell.is_null()) {
      return td::Status::Error("Error while importing a cell during serialization: cell is null");
    }

    auto cell_hash = cell->get_hash();
    auto it = cache.find(cell_hash);
    if (it != cache.end()) {
      return it->second;
    }

    size_t current_cell_id = cache.size();
    cache.emplace(cell_hash, current_cell_id);
    hash_by_ind.push_back(cell);

    bool is_special = false;

    vm::CellSlice cell_slice = vm::load_cell_slice_special(cell, is_special);
    if (!cell_slice.is_valid()) {
      return td::Status::Error("Invalid loaded cell data");
    }
    cache.emplace(get_cell_data_hash(cell_slice.as_bitslice()), current_cell_id);

    // Process cell references
    for (int i = 0; i < cell_slice.size_refs(); ++i) {
      TRY_RESULT(child_id, self(self, cell_slice.prefetch_ref(i)));
    }

    return current_cell_id;
  };
  for (auto root : boc_roots) {
    TRY_RESULT(ind, dfs(dfs, root));
  }
  return td::Status::OK();
}

int DATA_FOUND = 0;


td::HashMap<vm::Cell::Hash, size_t> cur_cell_hashes;
int calc_sub_size(td::Ref<vm::Cell> cell, const td::HashMap<vm::Cell::Hash, size_t>& cache) {
  auto cell_hash = cell->get_hash();
  auto it = cur_cell_hashes.find(cell_hash);
  if (it != cur_cell_hashes.end()) {
    return 0;
  }

  auto cache_it = cache.find(cell_hash);
  if (cache_it != cache.end()) {
    return 0;
  }

  size_t current_cell_id = cur_cell_hashes.size();
  cur_cell_hashes.emplace(cell_hash, current_cell_id);

  bool is_special = false;
  int ANS = 0;
  vm::CellSlice cell_slice = vm::load_cell_slice_special(cell, is_special);
  CellHash current_data_hash = get_cell_data_hash(cell_slice.as_bitslice());
  int empty_data = 0;
  if (cache.find(current_data_hash) != cache.end()) {
    empty_data = 1;
  }
  td::BitSlice cell_bitslice = cell_slice.as_bitslice();

  if (empty_data) {

  } else if (cell_slice.special_type() == vm::CellTraits::SpecialType::PrunnedBranch) {
    DCHECK(cell_slice.size() >= 16);
    ANS += cell_bitslice.size() - 16;
  } else {
    ANS += cell_bitslice.size();
  }

  // Process cell references
  for (int i = 0; i < cell_slice.size_refs(); ++i) {
    ANS += calc_sub_size(cell_slice.prefetch_ref(i), cache);
  }
  return ANS;
};


td::Result<td::BufferSlice> boc_compress_improved_structure_lz4_prev(const std::vector<td::Ref<vm::Cell>>& boc_roots,
                                                                     const td::HashMap<vm::Cell::Hash, size_t>& cache) {
  // Input validation
  if (boc_roots.empty()) {
    return td::Status::Error("No root cells were provided for serialization");
  }
  if (boc_roots.size() > BagOfCells::default_max_roots) {
    return td::Status::Error("Too many root cells were provided for serialization");
  }
  for (const auto& root : boc_roots) {
    if (root.is_null()) {
      return td::Status::Error("Cannot serialize a null cell reference into a bag of cells");
    }
  }

  // for (auto x : boc_roots) {
  //   cur_cell_hashes.clear();
  //   int sz = calc_sub_size(x, cache);
  //   std::cout << sz << "          \t\t";
  // }
  // std::cout << std::endl;


  // Initialize data structures for graph representation
  td::HashMap<vm::Cell::Hash, size_t> cell_hashes;
  std::vector<std::array<size_t, 4>> boc_graph;
  std::vector<size_t> refs_cnt;
  std::vector<td::BitSlice> cell_data;
  std::vector<size_t> cell_type;
  std::vector<size_t> prunned_branch_level;
  std::vector<size_t> root_indexes;
  size_t total_size_estimate = 0;
  int COEF = 20000;

  int cnt_in_prev = 0;
  td::HashMap<vm::Cell::Hash, int> cur_hashes;
  // Build graph representation using recursive lambda
  const auto build_graph = [&](auto&& self, td::Ref<vm::Cell> cell) -> td::Result<size_t> {
    if (cell.is_null()) {
      return td::Status::Error("Error while importing a cell during serialization: cell is null");
    }

    auto cell_hash = cell->get_hash();
    auto it = cell_hashes.find(cell_hash);
    if (it != cell_hashes.end()) {
      return it->second;
    }

    auto cache_it = cache.find(cell_hash);
    if (cache_it != cache.end()) {
      return COEF + cache_it->second;
    }

    size_t current_cell_id = boc_graph.size();
    cell_hashes.emplace(cell_hash, current_cell_id);
    int ind_in_prev = -1;
    // cache_it = cache.find(cell_hash);
    // if (cache_it != cache.end()) {
    //   ++cnt_in_prev;
    //   ind_in_prev = cache_it->second;
    // }

    bool is_special = false;
    vm::CellSlice cell_slice = vm::load_cell_slice_special(cell, is_special);
    if (!cell_slice.is_valid()) {
      return td::Status::Error("Invalid loaded cell data");
    }
    CellHash current_data_hash = get_cell_data_hash(cell_slice.as_bitslice());
    if (cache.find(current_data_hash) != cache.end()) {
      ind_in_prev = cache.find(current_data_hash)->second;
      copy_data[cell_slice.as_bitslice().size()]++;
    }
    td::BitSlice cell_bitslice = cell_slice.as_bitslice();

    // Initialize new cell in graph
    boc_graph.emplace_back();
    refs_cnt.emplace_back(cell_slice.size_refs());
    cell_type.emplace_back(size_t(cell_slice.special_type()));
    prunned_branch_level.push_back(0);

    DCHECK(cell_slice.size_refs() <= 4);

    // Process special cell of type PrunnedBranch
    if (ind_in_prev != -1) {
      prunned_branch_level.back() = 9;
      // td::BitArray<24> prunned_branch_data;
      // prunned_branch_data.bits().store_uint(ind_in_prev, 24);
      // cell_data.emplace_back(prunned_branch_data.as_bitslice().subslice(0, 24));
      cell_data.emplace_back();
    } else if (cell_slice.special_type() == vm::CellTraits::SpecialType::PrunnedBranch) {
      DCHECK(cell_slice.size() >= 16);
      cell_data.emplace_back(cell_bitslice.subslice(16, cell_bitslice.size() - 16));
      prunned_branch_level.back() = cell_slice.data()[1];
    } else {
      cell_data.emplace_back(cell_bitslice);
    }
    total_size_estimate += cell_bitslice.size();

    // Process cell references
    for (int i = 0; i < cell_slice.size_refs(); ++i) {
      TRY_RESULT(child_id, self(self, cell_slice.prefetch_ref(i)));
      boc_graph[current_cell_id][i] = child_id;
    }

    return current_cell_id;
  };

  // Build the graph starting from roots
  for (auto root : boc_roots) {
    TRY_RESULT(root_cell_id, build_graph(build_graph, root));
    root_indexes.push_back(root_cell_id);
  }
  for (auto x : copy_data) {
    if (x.second > 500)
    std::cout << x.first << " " << x.second << std::endl;
  }
  std::cout << "=======" << std::endl;


  for (int i = 0; i < boc_graph.size(); ++i) {
    if (prunned_branch_level[i] == 9) continue;

    td::Ref<td::BitString> result_wo_hashes = td::make_ref<td::BitString>();
    int cnt_app = 0;
    for (int start = 0; start < cell_data[i].size(); ++start) {
      if (start + 256 <= cell_data[i].size()) {
        td::BitArray<256> hash(cell_data[i].subslice(start, 256).bits());
        CellHash c = CellHash::from_slice(hash.as_slice());
        if (cache.find(c) != cache.end()) {
          start += 256;
          --start;
          continue;
        }
        // if (prev_hashes.find(c) != prev_hashes.end()) {
        //   ++CNT_REP_HASHES_PREV;
        //   start += 256;
        //   --start;
        //   continue;
        // }
        // if (cell_hashes.find(c) != cell_hashes.end()) {
        //   ++CNT_REP_HASHES_PREV;
        //   start += 256;
        //   --start;
        //   continue;
        // }
        // auto it = cur_hashes.find(c);
        // if (it != cur_hashes.end() && it->second != i) {
        //   ++CNT_REP_HASHES_PREV;
        //   start += 256;
        //   --start;
        // }
      }
      result_wo_hashes.write().append(cell_data[i].subslice(start, 1));
      ++cnt_app;
    }
    cell_data[i] = result_wo_hashes->subslice(0, cnt_app);
  }


  // std::cout << "N: " << boc_graph.size() << " CNT_REP_HASHES_PREV: " << CNT_REP_HASHES_PREV << std::endl;
  // std::cout << "N: " << boc_graph.size() << " TRANS: " << TRANS << std::endl;

  // std::cout << "PR: " << cell_hashes_prev.size() << " CUR: " << cell_hashes.size() << " IN_PREV: " << cnt_in_prev << std::endl;

  // Check graph properties
  const size_t node_count = boc_graph.size();
  std::vector<std::vector<size_t>> reverse_graph(node_count);
  size_t edge_count = 0;

  std::vector<size_t> non_cache_refs_cnt = refs_cnt;
  // Build reverse graph
  for (int i = 0; i < node_count; ++i) {
    for (size_t child_index = 0; child_index < refs_cnt[i]; ++child_index) {
      size_t child = boc_graph[i][child_index];
      ++edge_count;
      if (child >= node_count) {
        --non_cache_refs_cnt[i];
        continue;
      }
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
      in_degree[i] = non_cache_refs_cnt[i];
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
  if (root_indexes.size() >= (1 << 16)) {
    return td::Status::Error("Too many root cells");
  }
  append_uint(result, root_indexes.size(), 16);
  for (int root_ind : root_indexes) {
    if (root_ind >= node_count) {
      append_uint(result, root_ind, 32);
    } else {
      append_uint(result, rank[root_ind], 32);
    }
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
      edge_bits.store_uint(child >= node_count || rank[child] == i + 1, 1);
      ++edge_bits;
    }
  }
  for (int i = 0; i < node_count; ++i) {
    size_t node = topo_order[i];
    for (size_t child_index = 0; child_index < refs_cnt[node]; ++child_index) {
      size_t child = boc_graph[node][child_index];
      if (child < node_count && rank[child] != i + 1) {
        continue;
      }
      append_uint(result, child >= node_count, 1);
    }
  }
  while (result.size() % 8) {
    append_uint(result, 0, 1);
  }
  for (int i = 0; i < node_count; ++i) {
    size_t node = topo_order[i];
    for (size_t child_index = 0; child_index < refs_cnt[node]; ++child_index) {
      size_t child = boc_graph[node][child_index];
      if (child < node_count) {
        continue;
      }
      append_uint(result, child, 24);
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
      if (boc_graph[node][j] >= node_count || rank[boc_graph[node][j]] <= i + 1)
        continue;

      int delta = rank[boc_graph[node][j]] - i - 2; // Always >= 0 because of above check
      size_t required_bits = 1 + (31 ^ __builtin_clz(node_count - i - 3));

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
  TRY_RESULT(root_count, read_uint(bit_reader, 16));
  if (root_count < 1 || root_count > BagOfCells::default_max_roots) {
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
        size_t required_bits = 1 + (31 ^ __builtin_clz(node_count - i - 3));

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

td::Result<std::vector<td::Ref<vm::Cell>>> boc_decompress_improved_structure_lz4_prev(td::Slice compressed, int max_decompressed_size, std::vector<td::Ref<vm::Cell>>& hash_by_ind) {
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
  TRY_RESULT(root_count, read_uint(bit_reader, 16));
  if (root_count < 1 || root_count > BagOfCells::default_max_roots) {
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
  // for (int i = 0; i < root_count; ++i) {
  //   if (root_indexes[i] >= node_count) {
  //     std::cout << "Invalid root index: " << i << " " << root_indexes[i] << " " << node_count << std::endl;
  //     return td::Status::Error("BOC decompression failed: invalid root index");
  //   }
  // }

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
      } else {
        boc_graph[i][j] = 0;
      }
    }
  }
  for (int i = 0; i < node_count; ++i) {
    for (int j = 0; j < cell_refs_cnt[i]; ++j) {
      if (boc_graph[i][j] == i + 1) {
        TRY_RESULT(is_outer_edge, read_uint(bit_reader, 1));
        if (is_outer_edge) {
          boc_graph[i][j] = 20000;
        }
      }
    }
  }
  while ((orig_size - bit_reader.size()) % 8) {
    TRY_RESULT(bit, read_uint(bit_reader, 1));
  }
  for (int i = 0; i < node_count; ++i) {
    for (int j = 0; j < cell_refs_cnt[i]; ++j) {
      if (boc_graph[i][j] == 20000) {
        TRY_RESULT_ASSIGN(boc_graph[i][j], read_uint(bit_reader, 24));
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
        size_t required_bits = 1 + (31 ^ __builtin_clz(node_count - i - 3));

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
      if (child_node >= 20000) continue;
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
        if (child >= 20000) {
          cell_builders[i].store_ref(hash_by_ind[child - 20000]);
        } else {
          cell_builders[i].store_ref(nodes[child]);
        }
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
    if (index > 20000) {
      root_nodes.push_back(hash_by_ind[index - 20000]);
    } else {
      root_nodes.push_back(nodes[index]);
    }
  }

  return root_nodes;
}


td::Result<td::BufferSlice> boc_compress(const std::vector<td::Ref<vm::Cell>>& boc_roots, CompressionAlgorithm algo) {
  // Check for empty input
  if (boc_roots.empty()) {
    return td::Status::Error("Cannot compress empty BOC roots");
  }

  td::BufferSlice compressed;
  if (algo == CompressionAlgorithm::BaselineLZ4) {
    TRY_RESULT_ASSIGN(compressed, boc_compress_baseline_lz4(boc_roots));
  } else if (algo == CompressionAlgorithm::ImprovedStructureLZ4) {
    TRY_RESULT_ASSIGN(compressed, boc_compress_improved_structure_lz4(boc_roots));
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
  }
  return td::Status::Error("Unknown compression algorithm");
}

}  // namespace vm
