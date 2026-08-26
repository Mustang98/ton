#include <array>
#include <string>
#include <utility>
#include <vector>

#include "block/block-parse.h"
#include "td/utils/tests.h"
#include "validator/impl/augmentation-replay.h"
#include "vm/dict.h"

namespace {

using td::Ref;

Ref<vm::Cell> make_cell(unsigned value) {
  return vm::CellBuilder{}.store_long(value, 8).finalize_novm();
}

class TracedField final : public tlb::TLB {
 public:
  TracedField(std::string name, int bits, std::vector<std::string>& trace)
      : name_(std::move(name)), bits_(bits), trace_(trace) {
  }

  int get_size(const vm::CellSlice&) const override {
    return bits_;
  }

  bool skip(vm::CellSlice& cs) const override {
    trace_.push_back(name_ + ".skip");
    return cs.advance(bits_);
  }

  bool validate_skip(int*, vm::CellSlice& cs, bool) const override {
    trace_.push_back(name_ + ".validate");
    return cs.advance(bits_);
  }

  bool null_value(vm::CellBuilder& cb) const override {
    return cb.store_zeroes_bool(bits_);
  }

 private:
  std::string name_;
  int bits_;
  std::vector<std::string>& trace_;
};

class ReplayAugmentation final : public block::tlb::AugmentationCheckData {
 public:
  ReplayAugmentation(const tlb::TLB& value_type, const tlb::TLB& extra_type, std::vector<std::string>& trace)
      : AugmentationCheckData(value_type, extra_type), trace_(trace) {
  }

  bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice& value) const override {
    trace_.push_back("leaf");
    return value.have(8) && cb.store_long_bool(value.prefetch_ulong(8), 16);
  }

  bool eval_fork(vm::CellBuilder& cb, vm::CellSlice& left, vm::CellSlice& right) const override {
    trace_.push_back("fork");
    return left.have(16) && right.have(16) &&
           cb.store_long_bool((left.prefetch_ulong(16) + right.prefetch_ulong(16)) & 0xffff, 16);
  }

  bool eval_empty(vm::CellBuilder& cb) const override {
    trace_.push_back("empty");
    return cb.store_zeroes_bool(16);
  }

  bool validate_value_dependencies(int*, vm::CellSlice value) const override {
    trace_.push_back("dependency");
    return value.size_ext() == 8;
  }

 private:
  std::vector<std::string>& trace_;
};

struct ReplayDictionary {
  std::vector<std::string> trace;
  TracedField value_type{"value", 8, trace};
  TracedField extra_type{"extra", 16, trace};
  ReplayAugmentation augmentation{value_type, extra_type, trace};
  block::tlb::HashmapAugE type{8, augmentation};
  Ref<vm::Cell> wrapped;
  int inner_cells{0};

  explicit ReplayDictionary(std::initializer_list<std::pair<unsigned, unsigned>> entries) {
    vm::AugmentedDictionary dictionary{8, augmentation};
    for (auto [key_value, value] : entries) {
      td::BitArray<8> key{key_value};
      ASSERT_TRUE(dictionary.set_builder(key, vm::CellBuilder{}.store_long(value, 8), vm::Dictionary::SetMode::Add));
    }
    inner_cells = count_cells(dictionary.get_root_cell());
    vm::CellBuilder wrapper;
    ASSERT_TRUE(dictionary.append_dict_to_bool(wrapper));
    wrapped = wrapper.finalize_novm();
    trace.clear();
  }

  ReplayDictionary(const ReplayDictionary&) = delete;
  ReplayDictionary& operator=(const ReplayDictionary&) = delete;

 private:
  static int count_cells(const Ref<vm::Cell>& root) {
    if (root.is_null()) {
      return 0;
    }
    auto data_cell = root->load_cell().move_as_ok().data_cell;
    int result = 1;
    for (unsigned i = 0; i < data_cell->get_refs_cnt(); ++i) {
      result += count_cells(data_cell->get_ref(i));
    }
    return result;
  }
};

Ref<vm::Cell> replace_root_extra(const Ref<vm::Cell>& wrapped, unsigned extra) {
  auto cs = vm::load_cell_slice(wrapped);
  ASSERT_EQ(cs.fetch_ulong(1), 1u);
  auto inner = cs.fetch_ref();
  vm::CellBuilder builder;
  ASSERT_TRUE(builder.store_bool_bool(true));
  ASSERT_TRUE(builder.store_ref_bool(std::move(inner)));
  ASSERT_TRUE(builder.store_long_bool(extra, 16));
  return builder.finalize_novm();
}

}  // namespace

TEST(AugmentationReplay, certificate_requires_exact_complete_root_set) {
  using ton::validator::detail::GeneratedAugmentationCertificate;
  auto in = make_cell(1);
  auto out = make_cell(2);
  auto accounts = make_cell(3);
  GeneratedAugmentationCertificate certificate;

  ASSERT_TRUE(certificate.is_target(&block::gen::t_InMsgDescr));
  ASSERT_TRUE(!certificate.is_target(&block::gen::t_Transaction));
  certificate.record(&block::gen::t_InMsgDescr, in);
  certificate.record(&block::gen::t_OutMsgDescr, out);
  ASSERT_TRUE(!certificate.complete());
  ASSERT_TRUE(!certificate.matches(in, out, accounts));
  certificate.record(&block::gen::t_ShardAccountBlocks, accounts);
  ASSERT_TRUE(certificate.complete());
  ASSERT_TRUE(certificate.matches(in, out, accounts));

  // Equal hashes are insufficient: a separately owned occurrence falls back.
  auto equal_in = make_cell(1);
  ASSERT_EQ(equal_in->get_hash(), in->get_hash());
  ASSERT_TRUE(equal_in.get() != in.get());
  ASSERT_TRUE(!certificate.matches(equal_in, out, accounts));

  GeneratedAugmentationCertificate duplicate;
  duplicate.record(&block::gen::t_InMsgDescr, in);
  duplicate.record(&block::gen::t_InMsgDescr, in);
  duplicate.record(&block::gen::t_OutMsgDescr, out);
  duplicate.record(&block::gen::t_ShardAccountBlocks, accounts);
  ASSERT_TRUE(!duplicate.complete());
  ASSERT_TRUE(!duplicate.matches(in, out, accounts));
}

TEST(AugmentationReplay, exact_ops_budget_matches_dictionary_cells) {
  ReplayDictionary dictionary{{{0x10, 7}, {0x40, 11}, {0x80, 13}, {0xf0, 17}}};
  int expected_ops = dictionary.inner_cells + 1;  // HashmapAugE wrapper plus every inner node.

  int short_ops = expected_ops - 1;
  ASSERT_TRUE(!dictionary.type.validate_augmentations_only_ref(&short_ops, dictionary.wrapped));
  ASSERT_EQ(short_ops, 0);

  dictionary.trace.clear();
  int exact_ops = expected_ops;
  ASSERT_TRUE(dictionary.type.validate_augmentations_only_ref(&exact_ops, dictionary.wrapped));
  ASSERT_EQ(exact_ops, 0);

  int legacy_ops = expected_ops;
  ASSERT_TRUE(dictionary.type.validate_ref(&legacy_ops, dictionary.wrapped));
  ASSERT_EQ(legacy_ops, 0);
}

TEST(AugmentationReplay, rejects_malformed_augmentation_and_preserves_leaf_order) {
  ReplayDictionary dictionary{{{0x55, 23}}};
  int ops = 100;
  ASSERT_TRUE(dictionary.type.validate_augmentations_only_ref(&ops, dictionary.wrapped));
  ASSERT_EQ(dictionary.trace, (std::vector<std::string>{"extra.skip", "value.skip", "dependency", "leaf"}));

  auto malformed = replace_root_extra(dictionary.wrapped, 24);
  auto malformed_cs = vm::load_cell_slice(malformed);
  ASSERT_TRUE(!dictionary.type.validate_augmentations_only(&ops, malformed_cs));
  ASSERT_TRUE(!dictionary.type.validate_ref(100, malformed));
}
