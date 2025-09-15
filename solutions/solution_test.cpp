#include <iostream>
#include <fstream>
#include <queue>
#include <random>
#include <numeric>
#include "td/utils/lz4.h"
#include "td/utils/base64.h"
#include "vm/boc.h"
#include "td/utils/Gzip.h"
#include "vm/cells/CellBuilder.h"
#include "vm/cells/CellSlice.h"
#include "vm/cellslice.h"
#include "td/utils/common.h"
#include "tqdm.hpp"
#include "evaluation.cpp"
#include "vm/boc-compression.h"
// #include "validator/db/rootdb.hpp"
// #include "validator/fabric.h"
// #include "validator/db/archiver.hpp"

// #include "td/db/RocksDb.h"
#include "ton/ton-tl.hpp"
// #include "td/utils/overloaded.h"
// #include "common/checksum.h"
#include "tl-utils/common-utils.hpp"
#include "validator/stats-merger.h"
#include "block/block-auto.h"
#include "block/block-parse.h"
// #include "td/actor/MultiPromise.h"

using namespace std;

const int max_decompressed_size = 16 << 20;

void parse_tlb_info(td::Ref<vm::Cell> root) {
  block::gen::Block::Record block_rec;
  block::gen::Block().cell_unpack(root, block_rec);
  bool is_special = false;
  vm::CellSlice state_update = vm::load_cell_slice_special(block_rec.state_update, is_special);
  vm::CellSlice new_state = vm::load_cell_slice_special(state_update.prefetch_ref(1), is_special);
  //std::cout << new_state.as_bitslice().to_hex() << std::endl;

  block::gen::ShardStateUnsplit::Record shard_state_rec;
  block::gen::ShardStateUnsplit().cell_unpack(state_update.prefetch_ref(1), shard_state_rec);

  vm::CellSlice out_msg_queue = vm::load_cell_slice_special(shard_state_rec.out_msg_queue_info, is_special);
  std::cout << "Out msg queue: " << out_msg_queue.as_bitslice().to_hex() << std::endl;

  vm::CellSlice account = vm::load_cell_slice_special(shard_state_rec.accounts, is_special);
  std::cout << "Accounts: " << account.as_bitslice().to_hex() << std::endl;
  exit(0);

}

void evaluate_contest200(vm::CompressionAlgorithm algo) {
  // int i = 0;
  for (string filename : tq::tqdm(CONTEST200_FILES)) {
    ifstream cin(CONTEST200_PATH + filename);
    std::string mode;
    cin >> mode;
    assert(mode == "compress");
    std::string base64_data;
    cin >> base64_data;
    CHECK(!base64_data.empty());
    td::BufferSlice data(td::base64_decode(base64_data).move_as_ok());
    auto root = vm::std_boc_deserialize(data).move_as_ok();
    vector<td::Ref<vm::Cell>> roots{root};

    // parse_tlb_info(root);

    start_timer();
    td::BufferSlice compressed_data = vm::boc_compress(roots, algo).move_as_ok();
    end_timer("compress");

    update_score(compressed_data, filename, data.size());

    continue;

    start_timer();
    vector<td::Ref<vm::Cell>> decoded_roots = vm::boc_decompress(compressed_data, max_decompressed_size).move_as_ok();
    end_timer("decompress");

    auto decompressed_data = vm::std_boc_serialize(decoded_roots[0], 31).move_as_ok();
    if (decompressed_data != data) {
      std::cerr << "Decompressed data does not match original data for file: " << filename << std::endl;
      exit(1);
    }
  }
  report();
}

void evaluate_big500(vm::CompressionAlgorithm algo) {
  for (string filename : tq::tqdm(BIG500_FILES)) {
    ifstream cin(BIG500_PATH + filename);
    std::string mode;
    cin >> mode;
    assert(mode == "compress");
    std::string base64_data;
    cin >> base64_data;
    CHECK(!base64_data.empty());
    td::BufferSlice data(td::base64_decode(base64_data).move_as_ok());
    auto root = vm::std_boc_deserialize(data).move_as_ok();
    vector<td::Ref<vm::Cell>> roots{root};

    start_timer();
    td::BufferSlice compressed_data = vm::boc_compress(roots, algo).move_as_ok();
    end_timer("compress");

    update_score(compressed_data, filename, data.size());

    continue;

    start_timer();
    vector<td::Ref<vm::Cell>> decoded_roots = vm::boc_decompress(compressed_data, max_decompressed_size).move_as_ok();
    end_timer("decompress");

    auto decompressed_data = vm::std_boc_serialize(decoded_roots[0], 31).move_as_ok();
    if (decompressed_data != data) {
      std::cerr << "Decompressed data does not match original data for file: " << filename << std::endl;
      exit(1);
    }
  }
  report();
}

void evaluate_compress_candidate_data(vm::CompressionAlgorithm algo) {
  for (string filename : tq::tqdm(COMPRESS_CANDIDATE_DATA_FILES)) {
    ifstream cin(COMPRESS_CANDIDATE_DATA_PATH + filename);
    std::string block_base64;
    std::string collated_data_base64;
    cin >> block_base64;
    cin >> collated_data_base64;
    CHECK(!block_base64.empty());
    CHECK(!collated_data_base64.empty());

    td::BufferSlice block(td::base64_decode(block_base64).move_as_ok());
    td::BufferSlice collated_data(td::base64_decode(collated_data_base64).move_as_ok());

    vm::BagOfCells boc1, boc2;
    boc1.deserialize(block);
    if (boc1.get_root_count() != 1) {
      // return td::Status::Error("block candidate should have exactly one root");
      cerr << "block candidate should have exactly one root" << endl;
      exit(1);
    }
    std::vector<td::Ref<vm::Cell>> roots = {boc1.get_root_cell()};
    boc2.deserialize(collated_data);
    for (int i = 0; i < boc2.get_root_count(); ++i) {
      roots.push_back(boc2.get_root_cell(i));
    }

    start_timer();
    td::BufferSlice compressed_data = vm::boc_compress(roots, algo).move_as_ok();
    end_timer("compress");

    update_score(compressed_data, filename, block.size() + collated_data.size());

    // continue;

    start_timer();
    vector<td::Ref<vm::Cell>> decoded_roots = vm::boc_decompress(compressed_data, max_decompressed_size).move_as_ok();
    end_timer("decompress");

    td::BufferSlice block_decompressed = vm::std_boc_serialize(decoded_roots[0], 31).move_as_ok();
    decoded_roots.erase(decoded_roots.begin());
    int collated_data_mode = 2;  //proto_version >= 5 ? 2 : 31;
    td::BufferSlice collated_data_decompressed =
        vm::std_boc_serialize_multi(std::move(decoded_roots), collated_data_mode).move_as_ok();

    if (block != block_decompressed || collated_data != collated_data_decompressed) {
      std::cerr << "Decompressed data does not match original data for file: " << filename << std::endl;
      exit(1);
    }
  }
  report();
}

void evaluate_serialize_block_broadcast(vm::CompressionAlgorithm algo) {
  for (string filename : tq::tqdm(SERIALIZE_BLOCK_BROADCAST_FILES)) {
    ifstream cin(SERIALIZE_BLOCK_BROADCAST_PATH + filename);
    std::string proof_base64;
    std::string data_base64;
    cin >> proof_base64;
    cin >> data_base64;
    CHECK(!proof_base64.empty());
    CHECK(!data_base64.empty());

    td::BufferSlice proof(td::base64_decode(proof_base64).move_as_ok());
    td::BufferSlice data(td::base64_decode(data_base64).move_as_ok());

    auto proof_root = vm::std_boc_deserialize(proof).move_as_ok();
    auto data_root = vm::std_boc_deserialize(data).move_as_ok();

    std::vector<td::Ref<vm::Cell>> roots = {proof_root, data_root};

    start_timer();
    td::BufferSlice compressed_data = vm::boc_compress(roots, algo).move_as_ok();
    end_timer("compress");

    update_score(compressed_data, filename, proof.size() + data.size());

    // continue;

    start_timer();
    vector<td::Ref<vm::Cell>> decoded_roots = vm::boc_decompress(compressed_data, max_decompressed_size).move_as_ok();
    end_timer("decompress");

    td::BufferSlice proof_decompressed = vm::std_boc_serialize(decoded_roots[0], 0).move_as_ok();
    decoded_roots.erase(decoded_roots.begin());
    td::BufferSlice data_decompressed = vm::std_boc_serialize_multi(std::move(decoded_roots), 31).move_as_ok();

    if (proof != proof_decompressed || data != data_decompressed) {
      std::cerr << "Decompressed data does not match original data for file: " << filename << std::endl;
      exit(1);
    }
  }
  report();
}

void evaluate_serialize_block_candidate_broadcast(vm::CompressionAlgorithm algo) {
  for (string filename : tq::tqdm(SERIALIZE_BLOCK_CANDIDATE_BROADCAST_FILES)) {
    ifstream cin(SERIALIZE_BLOCK_CANDIDATE_BROADCAST_PATH + filename);
    std::string base64_data;
    cin >> base64_data;
    CHECK(!base64_data.empty());
    td::BufferSlice data(td::base64_decode(base64_data).move_as_ok());
    auto root = vm::std_boc_deserialize(data).move_as_ok();
    vector<td::Ref<vm::Cell>> roots{root};

    start_timer();
    td::BufferSlice compressed_data = vm::boc_compress(roots, algo).move_as_ok();
    end_timer("compress");

    update_score(compressed_data, filename, data.size());

    // continue;

    start_timer();
    vector<td::Ref<vm::Cell>> decoded_roots = vm::boc_decompress(compressed_data, max_decompressed_size).move_as_ok();
    end_timer("decompress");

    auto decompressed_data = vm::std_boc_serialize(decoded_roots[0], 31).move_as_ok();
    if (decompressed_data != data) {
      std::cerr << "Decompressed data does not match original data for file: " << filename << std::endl;
      exit(1);
    }
  }
  report();
}

void evaluate_wa(vm::CompressionAlgorithm algo) {
  for (string filename : tq::tqdm(vector<std::string>{"f3.txt", "f4.txt", "f5.txt"})) {
    ifstream cin("../../tests/wa/" + filename);
    std::string base64_data;
    cin >> base64_data;
    CHECK(!base64_data.empty());
    td::BufferSlice data(td::base64_decode(base64_data).move_as_ok());
    auto root = vm::std_boc_deserialize(data, 0, 1).move_as_ok();
    vector<td::Ref<vm::Cell>> roots{root};

    start_timer();
    td::BufferSlice compressed_data = vm::boc_compress(roots, algo).move_as_ok();
    end_timer("compress");

    update_score(compressed_data, filename, data.size());

    // continue;

    start_timer();
    vector<td::Ref<vm::Cell>> decoded_roots = vm::boc_decompress(compressed_data, max_decompressed_size).move_as_ok();
    end_timer("decompress");

    auto decompressed_data = vm::std_boc_serialize(decoded_roots[0], 0).move_as_ok();
    if (decompressed_data != data) {
      std::cerr << "Decompressed data does not match original data for file: " << filename << std::endl;
      exit(1);
    }
  }
  report();
}

unsigned get_gen_catchain_seqno(td::Ref<vm::Cell> root) {
  block::gen::Block::Record block_tlb;
  block::gen::Block().cell_unpack(root, block_tlb);
  block::gen::BlockInfo::Record block_info_tlb;
  block::gen::BlockInfo().cell_unpack(block_tlb.info, block_info_tlb);
  return block_info_tlb.gen_catchain_seqno;
}
//
// void evaluate_mirrornet(vm::CompressionAlgorithm algo) {
//   std::vector<td::Ref<vm::Cell>> prev_roots;
//   td::HashMap<vm::Cell::Hash, size_t> cache;
//   std::vector<td::Ref<vm::Cell>> hash_by_ind;
//   unsigned last_gen_catchain_seqno = 0;
//   int round_size = 0;
//   for (string filename : tq::tqdm(MIRRORNET_FILES)) {
//     ifstream cin(MIRRORNET_PATH + filename);
//     std::string base64_data;
//     cin >> base64_data;
//     CHECK(!base64_data.empty());
//     td::BufferSlice data(td::base64_decode(base64_data).move_as_ok());
//     auto candidate = ton::fetch_tl_object<ton::ton_api::db_candidate>(data, true).move_as_ok();
//     // std::cout << candidate->id_->seqno_ << ' ' << candidate->id_->workchain_ << ' ' << candidate->id_->shard_ << ' '
//               // << candidate->id_->ID << std::endl;
//     td::BufferSlice& block = candidate->data_;
//
//     td::BufferSlice& collated_data = candidate->collated_data_;
//
//     vm::BagOfCells boc1, boc2;
//     boc1.deserialize(block);
//     if (boc1.get_root_count() != 1) {
//       // return td::Status::Error("block candidate should have exactly one root");
//       cerr << "blockk candidate should have exactly one root" << endl;
//       exit(1);
//     }
//     // std::vector<td::Ref<vm::Cell>> roots = {};
//     std::vector<td::Ref<vm::Cell>> all_roots = {boc1.get_root_cell()};
//     std::vector<td::Ref<vm::Cell>> roots = {boc1.get_root_cell()};
//     unsigned cur_gen_catchain_seqno = get_gen_catchain_seqno(boc1.get_root_cell());
//     if (cur_gen_catchain_seqno != last_gen_catchain_seqno) {
//       std::cout << "Round size: " << round_size << " Cache size: " << cache.size() << std::endl;
//       // if (round_size) exit(0);
//       round_size = 0;
//       cache.clear();
//       hash_by_ind.clear();
//       last_gen_catchain_seqno = cur_gen_catchain_seqno;
//     } else {
//       ++round_size;
//     }
//     boc2.deserialize(collated_data);
//     for (int i = 0; i < boc2.get_root_count(); ++i) {
//       roots.push_back(boc2.get_root_cell(i));
//       all_roots.push_back(boc2.get_root_cell(i));
//     }
//
//     start_timer();
//     td::BufferSlice compressed_data;
//     if (algo == vm::CompressionAlgorithm::ImprovedStructureLZ4_v2) {
//       compressed_data = vm::boc_compress_improved_structure_lz4_prev(roots, cache).move_as_ok();
//     } else {
//       compressed_data = vm::boc_compress(roots, algo).move_as_ok();
//     }
//     end_timer("compress");
//     cache_cell_hashes(all_roots, cache, hash_by_ind);
//     // update_score(compressed_data, filename, block.size());
//     // update_score(compressed_data, filename, collated_data.size());
//     update_score(compressed_data, filename, block.size() + collated_data.size());
//
//     continue;
//
//     start_timer();
//     vector<td::Ref<vm::Cell>> decoded_roots;
//     if (algo == vm::CompressionAlgorithm::ImprovedStructureLZ4_v2) {
//       decoded_roots = vm::boc_decompress_improved_structure_lz4_prev(compressed_data, max_decompressed_size, hash_by_ind).move_as_ok();
//     } else {
//       decoded_roots = vm::boc_decompress(compressed_data, max_decompressed_size).move_as_ok();
//     }
//     end_timer("decompress");
//
//     td::BufferSlice block_decompressed = vm::std_boc_serialize(decoded_roots[0], 31).move_as_ok();
//     decoded_roots.erase(decoded_roots.begin());
//     int collated_data_mode = 2;  //proto_version >= 5 ? 2 : 31;
//     td::BufferSlice collated_data_decompressed =
//         vm::std_boc_serialize_multi(std::move(decoded_roots), collated_data_mode).move_as_ok();
//
//     if (block != block_decompressed || collated_data != collated_data_decompressed) {
//       std::cerr << "Decompressed data does not match original data for file: " << filename << std::endl;
//       exit(1);
//     }
//   }
//   // std::cout << "AVG block size: " << sum_block_size / MIRRORNET_FILES.size() << " AVG collated data size: " << sum_collated_data_size / MIRRORNET_FILES.size() << std::endl;
//   // std::cout << "Round size: " << round_size << " Cache size: " << cache.size() << std::endl;
//   report();
// }

int main() {
  // evaluate_contest200(vm::CompressionAlgorithm::BaselineLZ4);
  evaluate_contest200(vm::CompressionAlgorithm::ImprovedStructureLZ4);
  // evaluate_big500(vm::CompressionAlgorithm::BaselineLZ4);
  // evaluate_big500(vm::CompressionAlgorithm::ImprovedStructureLZ4);
  // evaluate_compress_candidate_data(vm::CompressionAlgorithm::ImprovedStructureLZ4);
  // evaluate_serialize_block_broadcast(vm::CompressionAlgorithm::ImprovedStructureLZ4);
  // evaluate_serialize_block_candidate_broadcast(vm::CompressionAlgorithm::ImprovedStructureLZ4);
  // evaluate_mirrornet(vm::CompressionAlgorithm::BaselineLZ4);
  // evaluate_mirrornet(vm::CompressionAlgorithm::BaselineLZ4);
  // evaluate_mirrornet(vm::CompressionAlgorithm::ImprovedStructureLZ4);
  // evaluate_mirrornet(vm::CompressionAlgorithm::ImprovedStructureLZ4_v2);
  return 0;
}

// Improved compression: 27.9952%
