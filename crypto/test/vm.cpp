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
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/bigint.hpp"
#include "fift/utils.h"
#include "td/utils/ScopeGuard.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/base64.h"
#include "td/utils/tests.h"
#include "vm/boc.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cells/UsageCell.h"
#include "vm/cp0.h"
#include "vm/dict.h"
#include "vm/vm.h"

static_assert(std::is_copy_constructible<vm::CellSlice>::value);
static_assert(std::is_copy_assignable<vm::CellSlice>::value);
static_assert(std::is_nothrow_move_constructible<vm::CellSlice>::value);
static_assert(std::is_nothrow_move_assignable<vm::CellSlice>::value);

std::string run_vm(td::Ref<vm::Cell> cell) {
  vm::init_vm().ensure();
  vm::DictionaryBase::get_empty_dictionary();

  class Logger : public td::LogInterface {
   public:
    void append(td::CSlice slice) override {
      res.append(slice.data(), slice.size());
    }
    std::string res;
  };
  static Logger logger;
  logger.res = "";
  td::set_log_fatal_error_callback([](td::CSlice message) { td::default_log_interface->append(logger.res); });
  vm::VmLog log{&logger, td::LogOptions::plain()};
  log.log_options.level = 4;
  log.log_options.fix_newlines = true;
  log.log_mask |= vm::VmLog::DumpStack;

  auto total_data_cells_before = vm::DataCell::get_total_data_cells();
  SCOPE_EXIT {
    auto total_data_cells_after = vm::DataCell::get_total_data_cells();
    ASSERT_EQ(total_data_cells_before, total_data_cells_after);
  };

  vm::Stack stack;
  try {
    vm::GasLimits gas_limit(1000, 1000);

    vm::run_vm_code(vm::load_cell_slice_ref(cell), stack, 0 /*flags*/, nullptr /*data*/, std::move(log) /*VmLog*/,
                    nullptr, &gas_limit);
  } catch (...) {
    LOG(FATAL) << "catch unhandled exception";
  }
  return logger.res;  // must be a copy
}

td::Ref<vm::Cell> to_cell(const unsigned char* buff, int bits) {
  return vm::CellBuilder().store_bits(buff, bits, 0).finalize();
}
void test_run_vm(td::Ref<vm::Cell> code) {
  auto a = run_vm(code);
  auto b = run_vm(code);
  ASSERT_EQ(a, b);
  REGRESSION_VERIFY(a);
}

void test_run_vm(td::Slice code_hex) {
  unsigned char buff[128];
  int bits = (int)td::bitstring::parse_bitstring_hex_literal(buff, sizeof(buff), code_hex.begin(), code_hex.end());
  CHECK(bits >= 0);
  test_run_vm(to_cell(buff, bits));
}

void test_run_vm_raw(td::Slice code64) {
  auto code = td::base64_decode(code64).move_as_ok();
  if (code.size() > 127) {
    code.resize(127);
  }
  test_run_vm(vm::CellBuilder().store_bytes(code).finalize());
}

TEST(VM, cell_slice_move_preserves_state_and_ownership) {
  auto child = vm::CellBuilder{}.store_long(0x55, 8).finalize_novm();
  auto cell = vm::CellBuilder{}.store_long(0x0123456789abcdefLL, 64).store_ref(child).finalize_novm();

  vm::CellSlice source{vm::NoVm{}, cell};
  ASSERT_TRUE(source.fetch_ulong(5) != vm::CellSlice::fetch_ulong_eof);
  auto expected_pos = source.cur_pos();
  auto expected_size = source.size();
  auto expected_refs = source.size_refs();
  auto expected_bits = source.prefetch_ulong(40);
  auto cell_refcount = cell->get_refcnt();

  vm::CellSlice moved{std::move(source)};
  ASSERT_TRUE(!source.is_valid());
  ASSERT_EQ(cell->get_refcnt(), cell_refcount);
  ASSERT_TRUE(moved.is_valid());
  ASSERT_EQ(moved.cur_pos(), expected_pos);
  ASSERT_EQ(moved.size(), expected_size);
  ASSERT_EQ(moved.size_refs(), expected_refs);
  ASSERT_EQ(moved.prefetch_ulong(40), expected_bits);

  {
    auto refcount_before_copy = cell->get_refcnt();
    vm::CellSlice copy{moved};
    ASSERT_EQ(cell->get_refcnt(), refcount_before_copy + 1);
    ASSERT_TRUE(copy.advance(7));
    ASSERT_EQ(moved.cur_pos(), expected_pos);
  }
  ASSERT_EQ(cell->get_refcnt(), cell_refcount);

  auto old_cell = vm::CellBuilder{}.store_long(0x7f, 7).finalize_novm();
  vm::CellSlice target{vm::NoVm{}, old_cell};
  auto old_refcount = old_cell->get_refcnt();
  auto moved_refcount = cell->get_refcnt();
  target = std::move(moved);
  ASSERT_TRUE(!moved.is_valid());
  ASSERT_EQ(old_cell->get_refcnt(), old_refcount - 1);
  ASSERT_EQ(cell->get_refcnt(), moved_refcount);
  ASSERT_EQ(target.cur_pos(), expected_pos);
  ASSERT_EQ(target.prefetch_ulong(40), expected_bits);

  auto self_move_refcount = cell->get_refcnt();
  auto* target_alias = &target;
  target = std::move(*target_alias);
  ASSERT_TRUE(target.is_valid());
  ASSERT_EQ(cell->get_refcnt(), self_move_refcount);
  ASSERT_EQ(target.cur_pos(), expected_pos);
  ASSERT_EQ(target.size(), expected_size);
  ASSERT_EQ(target.size_refs(), expected_refs);
  ASSERT_EQ(target.prefetch_ulong(40), expected_bits);

  moved.clear();
  ASSERT_TRUE(moved.empty_ext());
  ASSERT_TRUE(moved.load(vm::NoVm{}, old_cell));
  ASSERT_TRUE(moved.is_valid());
}

TEST(VM, cell_slice_move_handles_default_and_zero_bit_slices) {
  vm::CellSlice default_source;
  vm::CellSlice default_moved{std::move(default_source)};
  ASSERT_TRUE(!default_source.is_valid());
  ASSERT_TRUE(default_source.empty_ext());
  ASSERT_TRUE(!default_moved.is_valid());
  ASSERT_TRUE(default_moved.empty_ext());
  auto default_loaded = default_moved.move_as_loaded_cell();
  ASSERT_TRUE(default_loaded.data_cell.is_null());
  ASSERT_EQ(default_loaded.effective_level, 0u);

  auto empty_cell = vm::CellBuilder{}.finalize_novm();
  vm::CellSlice empty_source{vm::NoVm{}, empty_cell};
  vm::CellSlice empty_moved{std::move(empty_source)};
  ASSERT_TRUE(!empty_source.is_valid());
  ASSERT_TRUE(empty_moved.is_valid());
  ASSERT_TRUE(empty_moved.empty_ext());
  ASSERT_EQ(empty_moved.get_base_cell()->get_hash(), empty_cell->get_hash());
  std::ostringstream dump;
  empty_moved.dump(dump, 3, false);
  ASSERT_TRUE(!dump.str().empty());

  vm::CellSlice assigned{vm::NoVm{}, empty_cell};
  auto empty_refcount = empty_cell->get_refcnt();
  assigned = vm::CellSlice{};
  ASSERT_TRUE(!assigned.is_valid());
  ASSERT_TRUE(assigned.empty_ext());
  ASSERT_EQ(empty_cell->get_refcnt(), empty_refcount - 1);
  auto assigned_loaded = assigned.move_as_loaded_cell();
  ASSERT_TRUE(assigned_loaded.data_cell.is_null());
  ASSERT_EQ(assigned_loaded.effective_level, 0u);
}

TEST(VM, cell_slice_move_preserves_usage_tree_context) {
  auto old_child = vm::CellBuilder{}.store_long(0x11, 8).finalize_novm();
  auto old_root = vm::CellBuilder{}.store_ref(old_child).finalize_novm();
  auto source_child = vm::CellBuilder{}.store_long(0x22, 8).finalize_novm();
  auto source_root = vm::CellBuilder{}.store_ref(source_child).finalize_novm();

  auto old_tree = std::make_shared<vm::CellUsageTree>();
  auto source_tree = std::make_shared<vm::CellUsageTree>();
  std::size_t old_loads = 0;
  std::size_t source_loads = 0;
  old_tree->set_cell_load_callback([&](const auto&) { ++old_loads; });
  source_tree->set_cell_load_callback([&](const auto&) { ++source_loads; });
  auto old_usage_root = vm::UsageCell::create(old_root, old_tree->root_ptr());
  auto source_usage_root = vm::UsageCell::create(source_root, source_tree->root_ptr());

  vm::CellSlice target{vm::NoVm{}, old_usage_root};
  vm::CellSlice source{vm::NoVm{}, source_usage_root};
  ASSERT_EQ(old_loads, 1u);
  ASSERT_EQ(source_loads, 1u);
  ASSERT_TRUE(target.empty());
  ASSERT_EQ(target.size_refs(), 1u);
  ASSERT_TRUE(source.empty());
  ASSERT_EQ(source.size_refs(), 1u);

  target = std::move(source);
  ASSERT_TRUE(!source.is_valid());
  ASSERT_EQ(old_loads, 1u);
  ASSERT_EQ(source_loads, 1u);

  auto child_with_usage = target.fetch_ref();
  ASSERT_TRUE(child_with_usage.not_null());
  ASSERT_EQ(old_loads, 1u);
  ASSERT_EQ(source_loads, 1u);
  vm::CellSlice child_slice{vm::NoVm{}, std::move(child_with_usage)};
  ASSERT_EQ(child_slice.get_base_cell()->get_hash(), source_child->get_hash());
  ASSERT_EQ(old_loads, 1u);
  ASSERT_EQ(source_loads, 2u);

  auto old_child_id = old_tree->get_child(old_tree->root_id(), 0);
  auto source_child_id = source_tree->get_child(source_tree->root_id(), 0);
  ASSERT_EQ(old_child_id, 0u);
  ASSERT_TRUE(source_child_id != 0);
  ASSERT_TRUE(source_tree->is_loaded(source_child_id));

  ASSERT_TRUE(source.load(vm::NoVm{}, old_root));
  ASSERT_TRUE(source.is_valid());
}

TEST(VM, simple) {
  test_run_vm("ABCBABABABA");
}

TEST(VM, memory_leak_old) {
  test_run_vm("90787FDB3B");
}

TEST(VM, memory_leak) {
  test_run_vm("90707FDB3B");
}

TEST(VM, bug_div_short_any) {
  test_run_vm("6883FF73A98D");
}
TEST(VM, assert_pfx_dict_lookup) {
  test_run_vm("778B04216D73F43E018B04591277F473");
}

TEST(VM, assert_lookup_prefix) {
  test_run_vm("78E58B008B028B04010000016D90ED5272F43A755D77F4A8");
}

TEST(VM, assert_code_not_null) {
  test_run_vm("76ED40DE");
}

TEST(VM, bug_exec_dict_getnear) {
  test_run_vm("8B048B00006D72F47573655F6D656D6D656D8B007F");
}

TEST(VM, bug_stack_overflow) {
  test_run_vm("72A93AF8");
}

TEST(VM, assert_extract_minmax_key) {
  test_run_vm("6D6DEB21807AF49C2180EB21807AF41C");
}

TEST(VM, memory_leak_new) {
  test_run_vm("72E5ED40DB3603");
}

TEST(VM, unhandled_exception_1) {
  test_run_vm("70EDA2ED00");
}

TEST(VM, unhandled_exception_2) {
  // infinite loop now
  test_run_vm("EBEDB4");
}

TEST(VM, unhandled_exception_3) {
  // infinite loop now
  test_run_vm("EBEDC0");
}

TEST(VM, unhandled_exception_4) {
  test_run_vm("7F853EA1C8CB3E");
}

TEST(VM, unhandled_exception_5) {
  test_run_vm("738B04016D21F41476A721F49F");
}

TEST(VM, infinity_loop_1) {
  test_run_vm_raw("f3r4AJGQ6rDraIQ=");
}
TEST(VM, infinity_loop_2) {
  test_run_vm_raw("kpTt7ZLrig==");
}

TEST(VM, oom_1) {
  test_run_vm_raw("bXflX/BvDw==");
}

TEST(VM, bigint) {
  td::StringBuilder sb({}, true);

  using word_t = td::BigIntInfo::word_t;
  std::vector<word_t> numbers{1,
                              -1,
                              2,
                              -2,
                              100,
                              -100,
                              std::numeric_limits<word_t>::max(),
                              std::numeric_limits<word_t>::min(),
                              std::numeric_limits<word_t>::max() - 1,
                              std::numeric_limits<word_t>::min() + 1};
  for (auto x : numbers) {
    for (auto y : numbers) {
      word_t a;
      word_t b;
      td::BigIntInfo::set_mul(&a, &b, x, y);
      sb << "set_mul " << x << " * " << y << " = " << a << " " << b << "\n";
      td::BigIntInfo::add_mul(&a, &b, x, y);
      sb << "add_mul " << x << " " << y << " = " << a << " " << b << "\n";
      td::BigIntInfo::sub_mul(&a, &b, x, y);
      sb << "sub_mul " << x << " " << y << " = " << a << " " << b << "\n";
    }
  }
  auto base = td::BigIntInfo::Base;
  std::vector<word_t> lo_numbers{1, -1, 2, -2, 100, -100, base - 1, base - 2, -base + 1, -base + 2};
  for (auto x : numbers) {
    for (auto y : lo_numbers) {
      for (auto z : numbers) {
        word_t a;
        word_t b;
        td::BigIntInfo::dbl_divmod(&a, &b, x, y, z);
        sb << "dbl_divmod " << x << " " << y << " / " << z << " = " << a << " " << b << "\n";
      }
    }
  }

  REGRESSION_VERIFY(sb.as_cslice());
}

TEST(VM, report3_1) {
  //WA: expect (1, 2, 6, 3)
  td::Slice test1 =
      R"A(
CONT:<{
DEPTH
}>
3 SETNUMARGS
c0 POPCTR
1 INT
2 INT
3 INT
4 INT
5 INT
6 INT
4 RETURNARGS
)A";
  test_run_vm(fift::compile_asm(test1).move_as_ok());
}

TEST(VM, report3_2) {
  td::Slice test1 =
      R"A(
CONT:<{
DEPTH
}>
2 SETNUMARGS
c0 POPCTR
1 INT
2 INT
3 INT
4 INT
2 RETARGS
)A";
  test_run_vm(fift::compile_asm(test1).move_as_ok());
}

TEST(VM, report3_3) {
  // WA: expect (9)
  td::Slice test1 =
      R"A(
CONT:<{
 8 INT
}>
c0 POPCTR
CONT:<{
 9 INT
}>
c1 POPCTR
0 INT
BRANCH
)A";
  test_run_vm(fift::compile_asm(test1).move_as_ok());
}

TEST(VM, report3_4) {
  td::Slice test1 =
      R"A(
CONT:<{
1 INT
2 INT
3 INT
2 RETARGS
}>
CALLX
ADD
)A";
  test_run_vm(fift::compile_asm(test1).move_as_ok());
}

TEST(VM, report3_6) {
  // WA: expect StackOverflow
  td::Slice test1 =
      R"A(
10 INT
20 INT
30 INT
CONT:<{
  DEPTH
  40 INT
  SWAP
}>
2 SETNUMARGS
3 1 CALLXARGS
)A";
  test_run_vm(fift::compile_asm(test1).move_as_ok());
}

//TEST(VM, report3_ce) {
//td::Slice test1 =
//R"A(
//s16 POP
//s16 PUSH
//s0 s16 XCHG
//)A";
//test_run_vm(fift::compile_asm(test1).move_as_ok());
//}

TEST(VM, report3_int_overflow_1) {
  td::Slice test1 =
      R"A(
4 INT
16 INT
-115792089237316195423570985008687907853269984665640564039457584007913129639936 INT
MULDIVMOD
)A";
  test_run_vm(fift::compile_asm(test1).move_as_ok());
}
TEST(VM, report3_int_overflow_2) {
  td::Slice test1 =
      R"A(
4 INT
16 INT
-115792089237316195423570985008687907853269984665640564039457584007913129639936 INT
MULDIVR
)A";
  test_run_vm(fift::compile_asm(test1).move_as_ok());
}

TEST(VM, report3_qnot) {
  td::Slice test1 =
      R"A(
PUSHNAN
QNOT
)A";
  test_run_vm(fift::compile_asm(test1).move_as_ok());
}

TEST(VM, report3_loop_1) {
  //WA
  td::Slice test1 =
      R"A(
CONT:<{
  2 INT
}>
ATEXITALT
CONT:<{
 1 INT
 RETALT
 -1 INT
}>
AGAIN
3 INT
)A";
  test_run_vm(fift::compile_asm(test1).move_as_ok());
}

TEST(VM, report3_loop_2) {
  //WA
  td::Slice test1 =
      R"A(
CONT:<{
  2 INT
}>
ATEXITALT
CONT:<{
 1 INT
 RETALT
 -1 INT
}>
UNTIL
3 INT
)A";
  test_run_vm(fift::compile_asm(test1).move_as_ok());
}

TEST(VM, report3_loop_3) {
  //WA
  td::Slice test1 =
      R"A(
1 INT
CONT:<{
  UNTILEND
  RET
  -1 INT
}>
CALLX
)A";
  test_run_vm(fift::compile_asm(test1).move_as_ok());
}

TEST(VM, report3_loop_4) {
  //WA
  td::Slice test1 =
      R"A(
CONT:<{
  2 INT
}>
ATEXITALT
CONT:<{
  1 INT
  RETALT
  -1 PUSHINT
}>
CONT:<{
  -1 INT
}>
WHILE
3 INT
)A";
  test_run_vm(fift::compile_asm(test1).move_as_ok());
}
TEST(VM, report3_loop_5) {
  //WA
  td::Slice test1 =
      R"A(
CONT:<{
  1 INT
  2 INT
}>
ATEXITALT
3 INT
AGAINEND
DEC
DUP
IFRET
DROP
RETALT
)A";
  test_run_vm(fift::compile_asm(test1).move_as_ok());
}

TEST(VM, report3_loop_6) {
  //WA
  td::Slice test1 =
      R"A(
CONT:<{
  1 INT
  2 INT
}>
3 INT
AGAINEND
DEC
DUP
IFRET
DROP
ATEXITALT
RETALT
)A";
  test_run_vm(fift::compile_asm(test1).move_as_ok());
}

namespace {

td::Ref<vm::CellSlice> legacy_dictionary_lookup(td::Ref<vm::Cell> cell, td::ConstBitPtr key, int key_len,
                                                int label_mode) {
  int n = key_len;
  while (true) {
    vm::dict::LabelParser label{std::move(cell), n, label_mode};
    if (!label.is_prefix_of(key, n)) {
      return {};
    }
    n -= label.l_bits;
    if (n <= 0) {
      CHECK(!n);
      label.skip_label();
      return std::move(label.remainder);
    }
    key += label.l_bits;
    bool branch = *key++;
    --n;
    cell = label.remainder->prefetch_ref(static_cast<unsigned>(branch));
  }
}

td::Ref<vm::Cell> legacy_dictionary_lookup_ref(td::Ref<vm::Cell> root, td::ConstBitPtr key, int key_len) {
  auto value = legacy_dictionary_lookup(std::move(root), key, key_len, vm::dict::LabelParser::chk_all);
  if (value.is_null()) {
    return {};
  }
  if (!value->size() && value->size_refs() == 1) {
    return value->prefetch_ref();
  }
  throw vm::VmError{vm::Excno::dict_err, "dictionary value does not consist of exactly one reference"};
}

void assert_same_lookup_slice(const td::Ref<vm::CellSlice>& expected, const td::Ref<vm::CellSlice>& actual) {
  ASSERT_EQ(expected.is_null(), actual.is_null());
  if (expected.is_null()) {
    return;
  }
  ASSERT_EQ(expected->cur_pos(), actual->cur_pos());
  ASSERT_EQ(expected->size(), actual->size());
  ASSERT_EQ(expected->size_refs(), actual->size_refs());
  ASSERT_EQ(td::bitstring::bits_memcmp(expected->data_bits(), actual->data_bits(), expected->size()), 0);
  ASSERT_EQ(expected->get_base_cell()->get_hash(), actual->get_base_cell()->get_hash());
  for (unsigned i = 0; i < expected->size_refs(); ++i) {
    ASSERT_EQ(expected->prefetch_ref(i)->get_hash(), actual->prefetch_ref(i)->get_hash());
  }
}

class LookupTestAugmentation final : public vm::dict::AugmentationData {
 public:
  bool skip_extra(vm::CellSlice& cs) const override {
    return cs.advance(16);
  }

  bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice& value) const override {
    return cb.store_long_bool((value.size() << 3) | value.size_refs(), 16);
  }

  bool eval_fork(vm::CellBuilder& cb, vm::CellSlice& left, vm::CellSlice& right) const override {
    return left.have(16) && right.have(16) &&
           cb.store_long_bool(left.prefetch_ulong(16) ^ right.prefetch_ulong(16), 16);
  }

  bool eval_empty(vm::CellBuilder& cb) const override {
    return cb.store_zeroes_bool(16);
  }
};

struct LookupErrorObservation {
  bool threw{false};
  int error{0};
  std::string message;
};

template <class F>
LookupErrorObservation observe_lookup_error(F&& function) {
  try {
    static_cast<void>(function());
    return {};
  } catch (const vm::VmError& error) {
    return {true, error.get_errno(), error.get_msg()};
  }
}

void assert_same_lookup_error(const LookupErrorObservation& expected, const LookupErrorObservation& actual) {
  ASSERT_EQ(expected.threw, actual.threw);
  ASSERT_EQ(expected.error, actual.error);
  ASSERT_TRUE(expected.message == actual.message);
}

class LookupLazyCell final : public vm::Cell {
 private:
  struct PrivateTag {};

 public:
  using Trace = std::shared_ptr<std::vector<vm::CellHash>>;

  static td::Ref<vm::Cell> create(td::Ref<vm::DataCell> data_cell, Trace trace) {
    return td::Ref<LookupLazyCell>{true, std::move(data_cell), std::move(trace), PrivateTag{}};
  }

  LookupLazyCell(td::Ref<vm::DataCell> data_cell, Trace trace, PrivateTag)
      : data_cell_(std::move(data_cell)), trace_(std::move(trace)) {
  }

  td::Status set_data_cell(td::Ref<vm::DataCell>&& data_cell) const override {
    return data_cell->get_hash() == data_cell_->get_hash() ? td::Status::OK()
                                                           : td::Status::Error("wrong lookup test cell");
  }

  td::Result<LoadedCell> load_cell() const override {
    trace_->push_back(data_cell_->get_hash());
    ++load_count_;
    return LoadedCell{data_cell_, data_cell_->get_level(), {}};
  }

  bool is_virtualized() const override {
    return false;
  }

  vm::CellUsageTree::NodePtr get_tree_node() const override {
    return {};
  }

  bool is_loaded() const override {
    return load_count_ != 0;
  }

  LevelMask get_level_mask() const override {
    return data_cell_->get_level_mask();
  }

 protected:
  const Hash do_get_hash(td::uint32 level) const override {
    return data_cell_->get_hash(level);
  }

  td::uint16 do_get_depth(td::uint32 level) const override {
    return data_cell_->get_depth(level);
  }

 private:
  td::Ref<vm::DataCell> data_cell_;
  Trace trace_;
  mutable std::size_t load_count_{0};
};

td::Ref<vm::Cell> make_lazy_lookup_tree(const td::Ref<vm::Cell>& cell, const LookupLazyCell::Trace& trace) {
  vm::CellSlice slice{vm::NoVm{}, cell};
  vm::CellBuilder builder;
  CHECK(builder.store_bits_bool(slice.data_bits(), slice.size()));
  for (unsigned i = 0; i < slice.size_refs(); ++i) {
    CHECK(builder.store_ref_bool(make_lazy_lookup_tree(slice.prefetch_ref(i), trace)));
  }
  auto data_cell = builder.finalize_novm(slice.is_special());
  CHECK(data_cell->get_hash() == cell->get_hash());
  return LookupLazyCell::create(std::move(data_cell), trace);
}

td::Ref<vm::Cell> make_malformed_right_branch(const LookupLazyCell::Trace& trace) {
  td::BitArray<8> left_key{0LL};
  td::BitArray<8> right_key{0x80LL};
  vm::Dictionary dictionary{8};
  vm::CellBuilder left_value;
  left_value.store_long(0x11, 8);
  ASSERT_TRUE(dictionary.set_builder(left_key, left_value, vm::Dictionary::SetMode::Add));
  vm::CellBuilder right_value;
  right_value.store_long(0x22, 8);
  ASSERT_TRUE(dictionary.set_builder(right_key, right_value, vm::Dictionary::SetMode::Add));

  vm::CellSlice root{vm::NoVm{}, dictionary.get_root_cell()};
  ASSERT_EQ(root.size_refs(), 2u);
  auto malformed_data = vm::CellBuilder{}.store_long(2, 2).finalize_novm();
  auto malformed = LookupLazyCell::create(std::move(malformed_data), trace);
  vm::CellBuilder builder;
  CHECK(builder.store_bits_bool(root.data_bits(), root.size()));
  CHECK(builder.store_ref_bool(root.prefetch_ref(0)));
  CHECK(builder.store_ref_bool(std::move(malformed)));
  return LookupLazyCell::create(builder.finalize_novm(), trace);
}

}  // namespace

TEST(VM, dictionary_stack_lookup_matches_legacy_single_key_apis) {
  constexpr std::array<unsigned, 7> present_keys{0x00, 0x01, 0x17, 0x55, 0x80, 0xfe, 0xff};
  vm::Dictionary values{8};
  for (unsigned key_value : present_keys) {
    td::BitArray<8> key{key_value};
    vm::CellBuilder value;
    value.store_long(key_value * 3 + 1, 12);
    value.store_ref(vm::CellBuilder{}.store_long(key_value, 8).finalize_novm());
    ASSERT_TRUE(values.set_builder(key, value, vm::Dictionary::SetMode::Add));
  }

  for (unsigned key_value = 0; key_value < 256; ++key_value) {
    td::BitArray<8> key{key_value};
    auto expected = legacy_dictionary_lookup(values.get_root_cell(), key.bits(), 8, vm::dict::LabelParser::chk_all);
    auto actual = values.lookup(key);
    assert_same_lookup_slice(expected, actual);
    ASSERT_EQ(values.key_exists(key), expected.not_null());
  }

  vm::Dictionary refs{8};
  std::array<td::Ref<vm::Cell>, present_keys.size()> referenced;
  for (std::size_t i = 0; i < present_keys.size(); ++i) {
    td::BitArray<8> key{present_keys[i]};
    referenced[i] = vm::CellBuilder{}.store_long(0x100 + static_cast<long long>(i), 12).finalize_novm();
    ASSERT_TRUE(refs.set_ref(key.bits(), 8, referenced[i], vm::Dictionary::SetMode::Add));
  }
  for (unsigned key_value = 0; key_value < 256; ++key_value) {
    td::BitArray<8> key{key_value};
    auto expected = legacy_dictionary_lookup_ref(refs.get_root_cell(), key.bits(), 8);
    auto actual = refs.lookup_ref(key);
    ASSERT_EQ(expected.is_null(), actual.is_null());
    if (expected.not_null()) {
      ASSERT_EQ(expected.get(), actual.get());
      ASSERT_EQ(expected->get_hash(), actual->get_hash());
    }
  }

  LookupTestAugmentation augmentation;
  vm::AugmentedDictionary augmented{8, augmentation};
  for (std::size_t i = 0; i < present_keys.size(); ++i) {
    td::BitArray<8> key{present_keys[i]};
    ASSERT_TRUE(augmented.set_ref(key.bits(), 8, referenced[i], vm::Dictionary::SetMode::Add));
  }
  for (unsigned key_value = 0; key_value < 256; ++key_value) {
    td::BitArray<8> key{key_value};
    auto expected_full =
        legacy_dictionary_lookup(augmented.get_root_cell(), key.bits(), 8, vm::dict::LabelParser::chk_size);
    assert_same_lookup_slice(expected_full, augmented.lookup_with_extra(key.bits(), 8));
    ASSERT_EQ(augmented.key_exists(key), expected_full.not_null());

    auto expected_value = augmented.extract_value(
        legacy_dictionary_lookup(augmented.get_root_cell(), key.bits(), 8, vm::dict::LabelParser::chk_size));
    assert_same_lookup_slice(expected_value, augmented.lookup(key));

    auto expected_ref = augmented.extract_value_ref(
        legacy_dictionary_lookup(augmented.get_root_cell(), key.bits(), 8, vm::dict::LabelParser::chk_size));
    auto actual_ref = augmented.lookup_ref(key);
    ASSERT_EQ(expected_ref.is_null(), actual_ref.is_null());
    if (expected_ref.not_null()) {
      ASSERT_EQ(expected_ref.get(), actual_ref.get());
    }

    auto expected_extra = augmented.decompose_value_extra(
        legacy_dictionary_lookup(augmented.get_root_cell(), key.bits(), 8, vm::dict::LabelParser::chk_size));
    auto actual_extra = augmented.lookup_extra(key.bits(), 8);
    assert_same_lookup_slice(expected_extra.first, actual_extra.first);
    assert_same_lookup_slice(expected_extra.second, actual_extra.second);

    auto expected_ref_extra = augmented.decompose_value_ref_extra(
        legacy_dictionary_lookup(augmented.get_root_cell(), key.bits(), 8, vm::dict::LabelParser::chk_size));
    auto actual_ref_extra = augmented.lookup_ref_extra(key.bits(), 8);
    ASSERT_EQ(expected_ref_extra.first.is_null(), actual_ref_extra.first.is_null());
    if (expected_ref_extra.first.not_null()) {
      ASSERT_EQ(expected_ref_extra.first.get(), actual_ref_extra.first.get());
    }
    assert_same_lookup_slice(expected_ref_extra.second, actual_ref_extra.second);
  }
}

TEST(VM, dictionary_stack_lookup_preserves_malformed_error_order) {
  td::BitArray<8> key{0x80LL};
  auto malformed_label = vm::CellBuilder{}.store_long(2, 2).finalize_novm();
  auto malformed_fork = vm::CellBuilder{}.store_long(0, 2).finalize_novm();

  auto check_ordinary = [&](const td::Ref<vm::Cell>& root, vm::Excno expected_error, td::Slice expected_message) {
    auto legacy = observe_lookup_error(
        [&] { return legacy_dictionary_lookup(root, key.bits(), 8, vm::dict::LabelParser::chk_all); });
    vm::Dictionary dictionary{root, 8};
    auto lookup = observe_lookup_error([&] { return dictionary.lookup(key); });
    auto exists = observe_lookup_error([&] { return dictionary.key_exists(key); });
    auto lookup_ref = observe_lookup_error([&] { return dictionary.lookup_ref(key); });
    ASSERT_TRUE(legacy.threw);
    ASSERT_EQ(legacy.error, static_cast<int>(expected_error));
    ASSERT_TRUE(legacy.message == expected_message.str());
    assert_same_lookup_error(legacy, lookup);
    assert_same_lookup_error(legacy, exists);
    assert_same_lookup_error(legacy, lookup_ref);
  };
  check_ordinary(malformed_label, vm::Excno::cell_und, "error while parsing a dictionary node label");
  check_ordinary(malformed_fork, vm::Excno::dict_err, "invalid dictionary fork node");

  LookupTestAugmentation augmentation;
  auto legacy_augmented = observe_lookup_error(
      [&] { return legacy_dictionary_lookup(malformed_fork, key.bits(), 8, vm::dict::LabelParser::chk_size); });
  vm::AugmentedDictionary augmented{malformed_fork, 8, augmentation};
  auto actual_augmented = observe_lookup_error([&] { return augmented.lookup_with_extra(key.bits(), 8); });
  auto actual_augmented_exists = observe_lookup_error([&] { return augmented.key_exists(key); });
  assert_same_lookup_error(legacy_augmented, actual_augmented);
  assert_same_lookup_error(legacy_augmented, actual_augmented_exists);

  vm::Dictionary malformed_value{8};
  ASSERT_TRUE(malformed_value.set_builder(key, vm::CellBuilder{}.store_long(0x5a, 8), vm::Dictionary::SetMode::Add));
  auto legacy_value_error = observe_lookup_error(
      [&] { return legacy_dictionary_lookup_ref(malformed_value.get_root_cell(), key.bits(), 8); });
  auto actual_value_error = observe_lookup_error([&] { return malformed_value.lookup_ref(key); });
  ASSERT_TRUE(legacy_value_error.threw);
  ASSERT_EQ(legacy_value_error.error, static_cast<int>(vm::Excno::dict_err));
  ASSERT_TRUE(legacy_value_error.message == "dictionary value does not consist of exactly one reference");
  assert_same_lookup_error(legacy_value_error, actual_value_error);

  auto legacy_trace = std::make_shared<std::vector<vm::CellHash>>();
  auto actual_trace = std::make_shared<std::vector<vm::CellHash>>();
  auto legacy_root = make_malformed_right_branch(legacy_trace);
  auto actual_root = make_malformed_right_branch(actual_trace);
  td::BitArray<8> good_key{0LL};
  auto legacy_good = legacy_dictionary_lookup(legacy_root, good_key.bits(), 8, vm::dict::LabelParser::chk_all);
  vm::Dictionary actual_dictionary{actual_root, 8};
  auto actual_good = actual_dictionary.lookup(good_key);
  assert_same_lookup_slice(legacy_good, actual_good);
  ASSERT_TRUE(*legacy_trace == *actual_trace);
  ASSERT_EQ(legacy_trace->size(), 1u);

  legacy_trace = std::make_shared<std::vector<vm::CellHash>>();
  actual_trace = std::make_shared<std::vector<vm::CellHash>>();
  legacy_root = make_malformed_right_branch(legacy_trace);
  actual_root = make_malformed_right_branch(actual_trace);
  auto legacy_bad = observe_lookup_error(
      [&] { return legacy_dictionary_lookup(legacy_root, key.bits(), 8, vm::dict::LabelParser::chk_all); });
  vm::Dictionary actual_bad_dictionary{actual_root, 8};
  auto actual_bad = observe_lookup_error([&] { return actual_bad_dictionary.lookup(key); });
  assert_same_lookup_error(legacy_bad, actual_bad);
  ASSERT_TRUE(*legacy_trace == *actual_trace);
  ASSERT_EQ(legacy_trace->size(), 2u);

  auto mismatch_trace = std::make_shared<std::vector<vm::CellHash>>();
  vm::Dictionary mismatch{make_malformed_right_branch(mismatch_trace), 8};
  ASSERT_TRUE(mismatch.lookup(key.bits(), 7).is_null());
  ASSERT_TRUE(!mismatch.key_exists(key.bits(), 7));
  ASSERT_TRUE(mismatch.lookup_ref(key.bits(), 7).is_null());
  ASSERT_TRUE(mismatch_trace->empty());
}

TEST(VM, dictionary_stack_lookup_preserves_lazy_virtual_and_usage_context) {
  vm::Dictionary dictionary{8};
  constexpr std::array<unsigned, 5> keys{0x00, 0x20, 0x55, 0x80, 0xff};
  std::array<td::Ref<vm::Cell>, keys.size()> values;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    td::BitArray<8> key{keys[i]};
    values[i] = vm::CellBuilder{}.store_long(0xa0 + static_cast<long long>(i), 8).finalize_novm();
    ASSERT_TRUE(dictionary.set_ref(key.bits(), 8, values[i], vm::Dictionary::SetMode::Add));
  }
  td::BitArray<8> key{0x55};

  auto proof_usage_tree = std::make_shared<vm::CellUsageTree>();
  auto proof_usage_root = vm::UsageCell::create(dictionary.get_root_cell(), proof_usage_tree->root_ptr());
  ASSERT_TRUE(
      legacy_dictionary_lookup(std::move(proof_usage_root), key.bits(), 8, vm::dict::LabelParser::chk_all).not_null());
  auto proof = vm::MerkleProof::generate(dictionary.get_root_cell(), proof_usage_tree.get()).move_as_ok();
  auto virtual_root = vm::MerkleProof::virtualize(std::move(proof)).move_as_ok();
  ASSERT_TRUE(virtual_root->is_virtualized());
  auto legacy_virtual = legacy_dictionary_lookup(virtual_root, key.bits(), 8, vm::dict::LabelParser::chk_all);
  vm::Dictionary virtual_dictionary{virtual_root, 8};
  auto actual_virtual = virtual_dictionary.lookup(key);
  assert_same_lookup_slice(legacy_virtual, actual_virtual);
  vm::CellSlice legacy_virtual_copy{*legacy_virtual};
  vm::CellSlice actual_virtual_copy{*actual_virtual};
  auto legacy_virtual_loaded = legacy_virtual_copy.move_as_loaded_cell();
  auto actual_virtual_loaded = actual_virtual_copy.move_as_loaded_cell();
  ASSERT_EQ(legacy_virtual_loaded.effective_level, actual_virtual_loaded.effective_level);
  ASSERT_EQ(legacy_virtual_loaded.data_cell->get_hash(), actual_virtual_loaded.data_cell->get_hash());

  struct ContextObservation {
    td::Ref<vm::CellSlice> slice;
    td::Ref<vm::Cell> value_ref;
    LookupLazyCell::Trace lazy_loads;
    std::vector<vm::CellHash> usage_loads;
    std::shared_ptr<vm::CellUsageTree> usage_tree;
  };
  auto observe_context = [&](bool stack_lookup, bool return_ref) {
    ContextObservation observation;
    observation.lazy_loads = std::make_shared<std::vector<vm::CellHash>>();
    auto lazy_root = make_lazy_lookup_tree(dictionary.get_root_cell(), observation.lazy_loads);
    observation.usage_tree = std::make_shared<vm::CellUsageTree>();
    observation.usage_tree->set_cell_load_callback(
        [&](const vm::LoadedCell& loaded) { observation.usage_loads.push_back(loaded.data_cell->get_hash()); });
    auto usage_root = vm::UsageCell::create(std::move(lazy_root), observation.usage_tree->root_ptr());
    if (return_ref) {
      if (stack_lookup) {
        vm::Dictionary lookup_dictionary{usage_root, 8};
        observation.value_ref = lookup_dictionary.lookup_ref(key);
      } else {
        observation.value_ref = legacy_dictionary_lookup_ref(std::move(usage_root), key.bits(), 8);
      }
    } else if (stack_lookup) {
      vm::Dictionary lookup_dictionary{usage_root, 8};
      observation.slice = lookup_dictionary.lookup(key);
    } else {
      observation.slice =
          legacy_dictionary_lookup(std::move(usage_root), key.bits(), 8, vm::dict::LabelParser::chk_all);
    }
    return observation;
  };

  auto legacy_slice_context = observe_context(false, false);
  auto stack_slice_context = observe_context(true, false);
  assert_same_lookup_slice(legacy_slice_context.slice, stack_slice_context.slice);
  ASSERT_TRUE(*legacy_slice_context.lazy_loads == *stack_slice_context.lazy_loads);
  ASSERT_TRUE(legacy_slice_context.usage_loads == stack_slice_context.usage_loads);
  ASSERT_TRUE(legacy_slice_context.usage_loads.size() > 1);
  vm::CellSlice legacy_usage_copy{*legacy_slice_context.slice};
  vm::CellSlice stack_usage_copy{*stack_slice_context.slice};
  auto legacy_usage_loaded = legacy_usage_copy.move_as_loaded_cell();
  auto stack_usage_loaded = stack_usage_copy.move_as_loaded_cell();
  ASSERT_TRUE(legacy_usage_loaded.tree_node.is_from_tree(legacy_slice_context.usage_tree.get()));
  ASSERT_TRUE(stack_usage_loaded.tree_node.is_from_tree(stack_slice_context.usage_tree.get()));
  ASSERT_EQ(legacy_usage_loaded.effective_level, stack_usage_loaded.effective_level);

  auto legacy_ref_context = observe_context(false, true);
  auto stack_ref_context = observe_context(true, true);
  ASSERT_TRUE(legacy_ref_context.value_ref.not_null());
  ASSERT_TRUE(stack_ref_context.value_ref.not_null());
  ASSERT_EQ(legacy_ref_context.value_ref->get_hash(), stack_ref_context.value_ref->get_hash());
  ASSERT_TRUE(*legacy_ref_context.lazy_loads == *stack_ref_context.lazy_loads);
  ASSERT_TRUE(legacy_ref_context.usage_loads == stack_ref_context.usage_loads);
  ASSERT_TRUE(legacy_ref_context.value_ref->get_tree_node().is_from_tree(legacy_ref_context.usage_tree.get()));
  ASSERT_TRUE(stack_ref_context.value_ref->get_tree_node().is_from_tree(stack_ref_context.usage_tree.get()));
}

namespace {

struct TraversalSliceFingerprint {
  bool present{false};
  unsigned bits{0};
  unsigned refs{0};
  std::string data;
  std::string base_hash;
  std::vector<std::string> ref_hashes;

  bool operator==(const TraversalSliceFingerprint&) const = default;
};

TraversalSliceFingerprint fingerprint_traversal_slice(const td::Ref<vm::CellSlice>& value) {
  if (value.is_null()) {
    return {};
  }
  TraversalSliceFingerprint result;
  result.present = true;
  result.bits = value->size();
  result.refs = value->size_refs();
  result.data = value->data_bits().to_hex(value->size());
  result.base_hash = value->get_base_cell()->get_hash().to_hex();
  for (unsigned i = 0; i < value->size_refs(); ++i) {
    result.ref_hashes.push_back(value->prefetch_ref(i)->get_hash().to_hex());
  }
  return result;
}

struct TraversalEvent {
  unsigned key{0};
  TraversalSliceFingerprint first;
  TraversalSliceFingerprint second;

  bool operator==(const TraversalEvent&) const = default;
};

unsigned traversal_key(td::ConstBitPtr key, int key_len) {
  CHECK(key_len == 8);
  return static_cast<unsigned>(td::bitstring::bits_load_ulong(key, static_cast<unsigned>(key_len)));
}

struct TraversalRun {
  bool result{false};
  std::vector<TraversalEvent> events;
};

class ToggleTraversalAugmentation final : public vm::dict::AugmentationData {
 public:
  bool skip_extra(vm::CellSlice& cs) const override {
    return allow_reads && cs.advance(16);
  }

  bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice& value) const override {
    return cb.store_long_bool((value.size() << 3) | value.size_refs(), 16);
  }

  bool eval_fork(vm::CellBuilder& cb, vm::CellSlice& left, vm::CellSlice& right) const override {
    return left.have(16) && right.have(16) &&
           cb.store_long_bool(left.prefetch_ulong(16) ^ right.prefetch_ulong(16), 16);
  }

  bool eval_empty(vm::CellBuilder& cb) const override {
    return cb.store_zeroes_bool(16);
  }

  bool allow_reads{true};
};

}  // namespace

TEST(VM, dictionary_stack_for_each_matches_legacy_order_values_and_short_circuit) {
  vm::Dictionary dictionary{8};
  constexpr std::array<unsigned, 7> keys{0x00, 0x01, 0x17, 0x55, 0x80, 0xfe, 0xff};
  for (unsigned key_value : keys) {
    td::BitArray<8> key{key_value};
    vm::CellBuilder value;
    value.store_long(key_value * 5 + 3, 12);
    value.store_ref(vm::CellBuilder{}.store_long(key_value, 8).finalize_novm());
    ASSERT_TRUE(dictionary.set_builder(key, value, vm::Dictionary::SetMode::Add));
  }

  auto run = [&](bool stack, bool invert_first, unsigned stop_key) {
    TraversalRun observation;
    auto callback = [&](td::Ref<vm::CellSlice> value, td::ConstBitPtr key, int key_len) {
      auto key_value = traversal_key(key, key_len);
      observation.events.push_back({key_value, fingerprint_traversal_slice(value), {}});
      return key_value != stop_key;
    };
    observation.result = stack ? dictionary.check_for_each_stack(callback, invert_first)
                               : dictionary.check_for_each(callback, invert_first);
    return observation;
  };

  auto legacy_forward = run(false, false, 0x100);
  auto stack_forward = run(true, false, 0x100);
  ASSERT_TRUE(legacy_forward.result && stack_forward.result);
  ASSERT_TRUE(legacy_forward.events == stack_forward.events);
  ASSERT_EQ(legacy_forward.events.size(), keys.size());

  auto legacy_inverted = run(false, true, 0x100);
  auto stack_inverted = run(true, true, 0x100);
  ASSERT_TRUE(legacy_inverted.result && stack_inverted.result);
  ASSERT_TRUE(legacy_inverted.events == stack_inverted.events);

  auto legacy_stopped = run(false, false, 0x55);
  auto stack_stopped = run(true, false, 0x55);
  ASSERT_TRUE(!legacy_stopped.result && !stack_stopped.result);
  ASSERT_TRUE(legacy_stopped.events == stack_stopped.events);
  ASSERT_EQ(legacy_stopped.events.back().key, 0x55u);
}

TEST(VM, augmented_dictionary_value_stack_matches_legacy_extra_traversal) {
  ToggleTraversalAugmentation augmentation;
  vm::AugmentedDictionary dictionary{8, augmentation};
  constexpr std::array<unsigned, 6> keys{0x00, 0x10, 0x40, 0x55, 0x80, 0xff};
  for (unsigned key_value : keys) {
    td::BitArray<8> key{key_value};
    auto value = vm::CellBuilder{}.store_long(0x100 + key_value, 16).finalize_novm();
    ASSERT_TRUE(dictionary.set_ref(key, std::move(value), vm::Dictionary::SetMode::Add));
  }

  auto run = [&](bool stack, unsigned stop_key) {
    TraversalRun observation;
    if (stack) {
      observation.result =
          dictionary.check_for_each_value_stack([&](td::Ref<vm::CellSlice> value, td::ConstBitPtr key, int key_len) {
            auto key_value = traversal_key(key, key_len);
            observation.events.push_back({key_value, fingerprint_traversal_slice(value), {}});
            return key_value != stop_key;
          });
    } else {
      observation.result = dictionary.check_for_each_extra(
          [&](td::Ref<vm::CellSlice> value, td::Ref<vm::CellSlice> extra, td::ConstBitPtr key, int key_len) {
            CHECK(extra.not_null() && extra->size() == 16);
            auto key_value = traversal_key(key, key_len);
            observation.events.push_back({key_value, fingerprint_traversal_slice(value), {}});
            return key_value != stop_key;
          });
    }
    return observation;
  };

  auto legacy = run(false, 0x100);
  auto stack = run(true, 0x100);
  ASSERT_TRUE(legacy.result && stack.result);
  ASSERT_TRUE(legacy.events == stack.events);

  auto legacy_stopped = run(false, 0x55);
  auto stack_stopped = run(true, 0x55);
  ASSERT_TRUE(!legacy_stopped.result && !stack_stopped.result);
  ASSERT_TRUE(legacy_stopped.events == stack_stopped.events);

  augmentation.allow_reads = false;
  std::size_t legacy_callbacks = 0;
  std::size_t stack_callbacks = 0;
  ASSERT_TRUE(
      !dictionary.check_for_each_extra([&](td::Ref<vm::CellSlice>, td::Ref<vm::CellSlice>, td::ConstBitPtr, int) {
        ++legacy_callbacks;
        return true;
      }));
  ASSERT_TRUE(!dictionary.check_for_each_value_stack([&](td::Ref<vm::CellSlice>, td::ConstBitPtr, int) {
    ++stack_callbacks;
    return true;
  }));
  ASSERT_EQ(legacy_callbacks, 0u);
  ASSERT_EQ(stack_callbacks, legacy_callbacks);
}

TEST(VM, dictionary_stack_scan_diff_matches_legacy_order_values_and_short_circuit) {
  vm::Dictionary old_dictionary{8};
  vm::Dictionary new_dictionary{8};
  constexpr std::array<std::pair<unsigned, unsigned>, 4> old_values{std::pair{0x10u, 1u}, std::pair{0x30u, 3u},
                                                                    std::pair{0x50u, 5u}, std::pair{0x70u, 7u}};
  constexpr std::array<std::pair<unsigned, unsigned>, 5> new_values{
      std::pair{0x00u, 10u}, std::pair{0x10u, 11u}, std::pair{0x40u, 4u}, std::pair{0x50u, 5u}, std::pair{0x60u, 6u}};
  for (auto [key_value, value] : old_values) {
    td::BitArray<8> key{key_value};
    ASSERT_TRUE(old_dictionary.set_builder(key, vm::CellBuilder{}.store_long(value, 16), vm::Dictionary::SetMode::Add));
  }
  for (auto [key_value, value] : new_values) {
    td::BitArray<8> key{key_value};
    ASSERT_TRUE(new_dictionary.set_builder(key, vm::CellBuilder{}.store_long(value, 16), vm::Dictionary::SetMode::Add));
  }

  auto run = [&](bool stack, unsigned stop_key) {
    vm::Dictionary old_run{old_dictionary.get_root_cell(), 8};
    vm::Dictionary new_run{new_dictionary.get_root_cell(), 8};
    TraversalRun observation;
    auto callback = [&](td::ConstBitPtr key, int key_len, td::Ref<vm::CellSlice> old_value,
                        td::Ref<vm::CellSlice> new_value) {
      auto key_value = traversal_key(key, key_len);
      observation.events.push_back(
          {key_value, fingerprint_traversal_slice(old_value), fingerprint_traversal_slice(new_value)});
      return key_value != stop_key;
    };
    observation.result = stack ? old_run.scan_diff_stack(new_run, callback) : old_run.scan_diff(new_run, callback);
    return observation;
  };

  auto legacy = run(false, 0x100);
  auto stack = run(true, 0x100);
  ASSERT_TRUE(legacy.result && stack.result);
  ASSERT_TRUE(legacy.events == stack.events);
  constexpr std::array<unsigned, 6> expected_keys{0x00, 0x10, 0x30, 0x40, 0x60, 0x70};
  ASSERT_EQ(legacy.events.size(), expected_keys.size());
  for (std::size_t i = 0; i < expected_keys.size(); ++i) {
    ASSERT_EQ(legacy.events[i].key, expected_keys[i]);
  }

  auto legacy_stopped = run(false, 0x40);
  auto stack_stopped = run(true, 0x40);
  ASSERT_TRUE(!legacy_stopped.result && !stack_stopped.result);
  ASSERT_TRUE(legacy_stopped.events == stack_stopped.events);
  ASSERT_EQ(legacy_stopped.events.back().key, 0x40u);
}

TEST(VM, dictionary_stack_scan_diff_matches_legacy_generated_shapes) {
  for (unsigned round = 0; round < 48; ++round) {
    vm::Dictionary old_dictionary{8};
    vm::Dictionary new_dictionary{8};
    for (unsigned key_value = 0; key_value < 256; ++key_value) {
      bool in_old = ((key_value * 17 + round * 13) % 11) < 4;
      bool in_new = ((key_value * 29 + round * 7) % 13) < 5;
      unsigned old_value = (key_value << 4) ^ (round * 37 + 1);
      unsigned new_value = ((key_value + round) % 4 == 0) ? old_value : old_value ^ 0x5a5a;
      td::BitArray<8> key{key_value};
      if (in_old) {
        ASSERT_TRUE(
            old_dictionary.set_builder(key, vm::CellBuilder{}.store_long(old_value, 20), vm::Dictionary::SetMode::Add));
      }
      if (in_new) {
        ASSERT_TRUE(
            new_dictionary.set_builder(key, vm::CellBuilder{}.store_long(new_value, 20), vm::Dictionary::SetMode::Add));
      }
    }

    auto run = [&](bool stack, std::size_t callback_limit) {
      vm::Dictionary old_run{old_dictionary.get_root_cell(), 8};
      vm::Dictionary new_run{new_dictionary.get_root_cell(), 8};
      TraversalRun observation;
      auto callback = [&](td::ConstBitPtr key, int key_len, td::Ref<vm::CellSlice> old_value,
                          td::Ref<vm::CellSlice> new_value) {
        observation.events.push_back({traversal_key(key, key_len), fingerprint_traversal_slice(old_value),
                                      fingerprint_traversal_slice(new_value)});
        return observation.events.size() < callback_limit;
      };
      observation.result = stack ? old_run.scan_diff_stack(new_run, callback) : old_run.scan_diff(new_run, callback);
      return observation;
    };

    auto legacy_full = run(false, 257);
    auto stack_full = run(true, 257);
    ASSERT_TRUE(legacy_full.result && stack_full.result);
    ASSERT_TRUE(legacy_full.events == stack_full.events);

    std::size_t limit = 3 + round % 7;
    auto legacy_stopped = run(false, limit);
    auto stack_stopped = run(true, limit);
    ASSERT_EQ(legacy_stopped.result, stack_stopped.result);
    ASSERT_TRUE(legacy_stopped.events == stack_stopped.events);
  }
}

TEST(VM, augmented_dictionary_stack_scan_diff_matches_legacy_checked_scan) {
  ToggleTraversalAugmentation augmentation;
  vm::AugmentedDictionary old_dictionary{8, augmentation};
  vm::AugmentedDictionary new_dictionary{8, augmentation};
  constexpr std::array<unsigned, 5> old_keys{0x00, 0x20, 0x55, 0x80, 0xf0};
  constexpr std::array<unsigned, 5> new_keys{0x00, 0x10, 0x55, 0x80, 0xff};
  for (unsigned key_value : old_keys) {
    td::BitArray<8> key{key_value};
    auto value = vm::CellBuilder{}.store_long(0x200 + key_value, 16).finalize_novm();
    ASSERT_TRUE(old_dictionary.set_ref(key, std::move(value), vm::Dictionary::SetMode::Add));
  }
  for (unsigned key_value : new_keys) {
    td::BitArray<8> key{key_value};
    unsigned payload = key_value == 0x55 ? 0x755 : 0x200 + key_value;
    auto value = vm::CellBuilder{}.store_long(payload, 16).finalize_novm();
    ASSERT_TRUE(new_dictionary.set_ref(key, std::move(value), vm::Dictionary::SetMode::Add));
  }

  auto run = [&](bool stack) {
    vm::AugmentedDictionary old_run{old_dictionary.get_root_cell(), 8, augmentation};
    vm::AugmentedDictionary new_run{new_dictionary.get_root_cell(), 8, augmentation};
    TraversalRun observation;
    auto callback = [&](td::ConstBitPtr key, int key_len, td::Ref<vm::CellSlice> old_value,
                        td::Ref<vm::CellSlice> new_value) {
      observation.events.push_back({traversal_key(key, key_len), fingerprint_traversal_slice(old_value),
                                    fingerprint_traversal_slice(new_value)});
      return true;
    };
    observation.result =
        stack ? old_run.scan_diff_stack(new_run, callback, 3) : old_run.scan_diff(new_run, callback, 3);
    return observation;
  };

  auto legacy = run(false);
  auto stack = run(true);
  ASSERT_TRUE(legacy.result && stack.result);
  ASSERT_TRUE(!legacy.events.empty());
  ASSERT_TRUE(legacy.events == stack.events);
}

TEST(VM, dictionary_stack_traversals_preserve_malformed_load_and_error_order) {
  struct Observation {
    LookupErrorObservation error;
    std::vector<unsigned> callbacks;
    std::vector<vm::CellHash> loads;
  };

  auto run_for_each = [&](bool stack, bool stop_after_first) {
    Observation observation;
    auto trace = std::make_shared<std::vector<vm::CellHash>>();
    vm::Dictionary dictionary{make_malformed_right_branch(trace), 8};
    try {
      auto callback = [&](td::Ref<vm::CellSlice>, td::ConstBitPtr key, int key_len) {
        observation.callbacks.push_back(traversal_key(key, key_len));
        return !stop_after_first;
      };
      static_cast<void>(stack ? dictionary.check_for_each_stack(callback) : dictionary.check_for_each(callback));
    } catch (const vm::VmError& error) {
      observation.error = {true, error.get_errno(), error.get_msg()};
    }
    observation.loads = *trace;
    return observation;
  };

  auto legacy_for_each = run_for_each(false, false);
  auto stack_for_each = run_for_each(true, false);
  assert_same_lookup_error(legacy_for_each.error, stack_for_each.error);
  ASSERT_TRUE(legacy_for_each.error.threw);
  ASSERT_TRUE(legacy_for_each.callbacks == stack_for_each.callbacks);
  ASSERT_TRUE(legacy_for_each.loads == stack_for_each.loads);

  auto legacy_stopped = run_for_each(false, true);
  auto stack_stopped = run_for_each(true, true);
  ASSERT_TRUE(!legacy_stopped.error.threw && !stack_stopped.error.threw);
  ASSERT_TRUE(legacy_stopped.callbacks == stack_stopped.callbacks);
  ASSERT_EQ(legacy_stopped.callbacks.size(), 1u);
  ASSERT_TRUE(legacy_stopped.loads == stack_stopped.loads);

  auto run_scan = [&](bool stack) {
    Observation observation;
    auto trace = std::make_shared<std::vector<vm::CellHash>>();
    vm::Dictionary empty{8};
    vm::Dictionary malformed{make_malformed_right_branch(trace), 8};
    try {
      auto callback = [&](td::ConstBitPtr key, int key_len, td::Ref<vm::CellSlice>, td::Ref<vm::CellSlice>) {
        observation.callbacks.push_back(traversal_key(key, key_len));
        return true;
      };
      static_cast<void>(stack ? empty.scan_diff_stack(malformed, callback) : empty.scan_diff(malformed, callback));
    } catch (const vm::VmError& error) {
      observation.error = {true, error.get_errno(), error.get_msg()};
    }
    observation.loads = *trace;
    return observation;
  };

  auto legacy_scan = run_scan(false);
  auto stack_scan = run_scan(true);
  assert_same_lookup_error(legacy_scan.error, stack_scan.error);
  ASSERT_TRUE(legacy_scan.error.threw);
  ASSERT_TRUE(legacy_scan.callbacks == stack_scan.callbacks);
  ASSERT_TRUE(legacy_scan.loads == stack_scan.loads);
}

TEST(VM, dictionary_stack_scan_diff_preserves_lazy_and_usage_context) {
  td::BitArray<8> key{0x5aLL};
  vm::Dictionary old_dictionary{8};
  vm::Dictionary new_dictionary{8};
  ASSERT_TRUE(old_dictionary.set_builder(key, vm::CellBuilder{}.store_long(0x111, 12), vm::Dictionary::SetMode::Add));
  ASSERT_TRUE(new_dictionary.set_builder(key, vm::CellBuilder{}.store_long(0x222, 12), vm::Dictionary::SetMode::Add));

  struct Observation {
    TraversalRun traversal;
    LookupLazyCell::Trace lazy_loads;
    std::vector<vm::CellHash> usage_loads;
    bool callback_has_tree{false};
  };
  auto run = [&](bool stack) {
    Observation observation;
    observation.lazy_loads = std::make_shared<std::vector<vm::CellHash>>();
    auto lazy_root = make_lazy_lookup_tree(old_dictionary.get_root_cell(), observation.lazy_loads);
    auto usage_tree = std::make_shared<vm::CellUsageTree>();
    usage_tree->set_cell_load_callback(
        [&](const vm::LoadedCell& loaded) { observation.usage_loads.push_back(loaded.data_cell->get_hash()); });
    auto usage_root = vm::UsageCell::create(std::move(lazy_root), usage_tree->root_ptr());
    vm::Dictionary old_run{std::move(usage_root), 8};
    vm::Dictionary new_run{new_dictionary.get_root_cell(), 8};
    auto callback = [&](td::ConstBitPtr callback_key, int key_len, td::Ref<vm::CellSlice> old_value,
                        td::Ref<vm::CellSlice> new_value) {
      observation.traversal.events.push_back({traversal_key(callback_key, key_len),
                                              fingerprint_traversal_slice(old_value),
                                              fingerprint_traversal_slice(new_value)});
      vm::CellSlice copy{*old_value};
      observation.callback_has_tree = copy.move_as_loaded_cell().tree_node.is_from_tree(usage_tree.get());
      return true;
    };
    observation.traversal.result =
        stack ? old_run.scan_diff_stack(new_run, callback) : old_run.scan_diff(new_run, callback);
    return observation;
  };

  auto legacy = run(false);
  auto stack = run(true);
  ASSERT_TRUE(legacy.traversal.result && stack.traversal.result);
  ASSERT_TRUE(legacy.traversal.events == stack.traversal.events);
  ASSERT_EQ(legacy.traversal.events.size(), 1u);
  ASSERT_TRUE(*legacy.lazy_loads == *stack.lazy_loads);
  ASSERT_TRUE(legacy.usage_loads == stack.usage_loads);
  ASSERT_TRUE(legacy.callback_has_tree && stack.callback_has_tree);
}

TEST(VM, dictionary_stack_for_each_preserves_lazy_virtual_and_usage_context) {
  vm::Dictionary dictionary{8};
  constexpr std::array<unsigned, 5> keys{0x00, 0x20, 0x55, 0x80, 0xff};
  for (unsigned key_value : keys) {
    td::BitArray<8> key{key_value};
    ASSERT_TRUE(
        dictionary.set_builder(key, vm::CellBuilder{}.store_long(key_value + 1, 12), vm::Dictionary::SetMode::Add));
  }

  auto proof_usage_tree = std::make_shared<vm::CellUsageTree>();
  auto proof_usage_root = vm::UsageCell::create(dictionary.get_root_cell(), proof_usage_tree->root_ptr());
  vm::Dictionary proof_dictionary{std::move(proof_usage_root), 8};
  td::BitArray<8> first_key{0LL};
  ASSERT_TRUE(proof_dictionary.lookup(first_key).not_null());
  auto proof = vm::MerkleProof::generate(dictionary.get_root_cell(), proof_usage_tree.get()).move_as_ok();
  auto virtual_root = vm::MerkleProof::virtualize(std::move(proof)).move_as_ok();
  ASSERT_TRUE(virtual_root->is_virtualized());

  auto collect_virtual = [&](bool stack) {
    vm::Dictionary virtual_dictionary{virtual_root, 8};
    TraversalRun observation;
    auto callback = [&](td::Ref<vm::CellSlice> value, td::ConstBitPtr key, int key_len) {
      observation.events.push_back({traversal_key(key, key_len), fingerprint_traversal_slice(value), {}});
      return false;
    };
    observation.result =
        stack ? virtual_dictionary.check_for_each_stack(callback) : virtual_dictionary.check_for_each(callback);
    return observation;
  };
  auto legacy_virtual = collect_virtual(false);
  auto stack_virtual = collect_virtual(true);
  ASSERT_TRUE(!legacy_virtual.result && !stack_virtual.result);
  ASSERT_TRUE(legacy_virtual.events == stack_virtual.events);
  ASSERT_EQ(legacy_virtual.events.size(), 1u);
  ASSERT_EQ(legacy_virtual.events.front().key, 0u);

  struct ContextObservation {
    TraversalRun traversal;
    LookupLazyCell::Trace lazy_loads;
    std::vector<vm::CellHash> usage_loads;
    std::vector<bool> callback_has_tree;
  };
  auto collect_context = [&](bool stack) {
    ContextObservation observation;
    observation.lazy_loads = std::make_shared<std::vector<vm::CellHash>>();
    auto lazy_root = make_lazy_lookup_tree(dictionary.get_root_cell(), observation.lazy_loads);
    auto usage_tree = std::make_shared<vm::CellUsageTree>();
    usage_tree->set_cell_load_callback(
        [&](const vm::LoadedCell& loaded) { observation.usage_loads.push_back(loaded.data_cell->get_hash()); });
    auto usage_root = vm::UsageCell::create(std::move(lazy_root), usage_tree->root_ptr());
    vm::Dictionary usage_dictionary{std::move(usage_root), 8};
    auto callback = [&](td::Ref<vm::CellSlice> value, td::ConstBitPtr key, int key_len) {
      observation.traversal.events.push_back({traversal_key(key, key_len), fingerprint_traversal_slice(value), {}});
      vm::CellSlice copy{*value};
      auto loaded = copy.move_as_loaded_cell();
      observation.callback_has_tree.push_back(loaded.tree_node.is_from_tree(usage_tree.get()));
      return true;
    };
    observation.traversal.result =
        stack ? usage_dictionary.check_for_each_stack(callback) : usage_dictionary.check_for_each(callback);
    return observation;
  };

  auto legacy_context = collect_context(false);
  auto stack_context = collect_context(true);
  ASSERT_TRUE(legacy_context.traversal.result && stack_context.traversal.result);
  ASSERT_TRUE(legacy_context.traversal.events == stack_context.traversal.events);
  ASSERT_TRUE(*legacy_context.lazy_loads == *stack_context.lazy_loads);
  ASSERT_TRUE(legacy_context.usage_loads == stack_context.usage_loads);
  ASSERT_TRUE(legacy_context.callback_has_tree == stack_context.callback_has_tree);
  ASSERT_EQ(legacy_context.callback_has_tree.size(), keys.size());
  for (bool has_tree : stack_context.callback_has_tree) {
    ASSERT_TRUE(has_tree);
  }
}

namespace {

class BatchSumAugmentation final : public vm::dict::AugmentationData {
 public:
  bool skip_extra(vm::CellSlice& slice) const override {
    return slice.advance(64);
  }

  bool eval_leaf(vm::CellBuilder& builder, vm::CellSlice& value) const override {
    return value.have(32) && builder.store_long_bool(value.prefetch_ulong(32), 64);
  }

  bool eval_fork(vm::CellBuilder& builder, vm::CellSlice& left, vm::CellSlice& right) const override {
    return left.have(64) && right.have(64) &&
           builder.store_long_bool(left.prefetch_ulong(64) + right.prefetch_ulong(64), 64);
  }

  bool eval_empty(vm::CellBuilder& builder) const override {
    return builder.store_zeroes_bool(64);
  }
};

class FailingBatchAugmentation final : public vm::dict::AugmentationData {
 public:
  void fail_after(std::size_t successful_evaluations) {
    remaining_ = successful_evaluations;
  }

  void allow_all() {
    remaining_ = std::numeric_limits<std::size_t>::max();
  }

  bool skip_extra(vm::CellSlice& slice) const override {
    return slice.advance(64);
  }

  bool eval_leaf(vm::CellBuilder& builder, vm::CellSlice& value) const override {
    return permit() && value.have(32) && builder.store_long_bool(value.prefetch_ulong(32), 64);
  }

  bool eval_fork(vm::CellBuilder& builder, vm::CellSlice& left, vm::CellSlice& right) const override {
    return permit() && left.have(64) && right.have(64) &&
           builder.store_long_bool(left.prefetch_ulong(64) + right.prefetch_ulong(64), 64);
  }

  bool eval_empty(vm::CellBuilder& builder) const override {
    return permit() && builder.store_zeroes_bool(64);
  }

 private:
  bool permit() const {
    if (!remaining_) {
      return false;
    }
    --remaining_;
    return true;
  }

  mutable std::size_t remaining_{std::numeric_limits<std::size_t>::max()};
};

class BatchLoadSequenceCounter final : public vm::VmStateInterface {
 public:
  void register_cell_load(const vm::CellHash& hash) override {
    loads.push_back(hash);
  }

  std::vector<vm::CellHash> loads;
};

struct BatchUpdateStorage {
  td::BitArray<16> key;
  bool erase{false};
  std::uint32_t value{0};
  vm::Dictionary::SetMode mode{vm::Dictionary::SetMode::Set};
};

td::Ref<vm::CellBuilder> make_batch_value(std::uint32_t value) {
  td::Ref<vm::CellBuilder> builder{true};
  CHECK(builder.write().store_long_bool(value, 32));
  return builder;
}

void assert_same_batch_root(const td::Ref<vm::Cell>& expected, const td::Ref<vm::Cell>& actual) {
  ASSERT_EQ(expected.is_null(), actual.is_null());
  if (expected.is_null()) {
    return;
  }
  ASSERT_EQ(expected->get_hash(), actual->get_hash());
  auto expected_boc = vm::std_boc_serialize(expected, 31).move_as_ok();
  auto actual_boc = vm::std_boc_serialize(actual, 31).move_as_ok();
  ASSERT_TRUE(expected_boc.as_slice() == actual_boc.as_slice());
}

bool apply_augmented_serial(vm::AugmentedDictionary& dictionary, const vm::AugmentedDictionary::BatchSetEntry& update) {
  if (update.value.is_null()) {
    return dictionary.lookup_delete(update.key, 16).not_null();
  }
  return dictionary.set_builder(update.key, 16, *update.value, update.mode);
}

void add_initial_batch_values(vm::Dictionary& plain, vm::AugmentedDictionary& augmented,
                              const std::map<std::uint16_t, std::uint32_t>& state) {
  for (const auto& [key_value, value] : state) {
    td::BitArray<16> key{key_value};
    ASSERT_TRUE(plain.set_builder(key, make_batch_value(value), vm::Dictionary::SetMode::Add));
    ASSERT_TRUE(augmented.set_builder(key, *make_batch_value(value), vm::Dictionary::SetMode::Add));
  }
}

}  // namespace

TEST(VM, augmented_dictionary_multiset_matches_plain_and_serial_roots) {
  BatchSumAugmentation augmentation;
  std::mt19937 random{0x51ced123};

  for (int round = 0; round < 32; ++round) {
    std::map<std::uint16_t, std::uint32_t> state;
    while (state.size() < 192) {
      state.emplace(static_cast<std::uint16_t>(random()), random());
    }
    vm::Dictionary plain_base{16};
    vm::AugmentedDictionary augmented_base{16, augmentation};
    add_initial_batch_values(plain_base, augmented_base, state);

    vm::Dictionary plain_serial{plain_base.get_root_cell(), 16};
    vm::Dictionary plain_batch{plain_base.get_root_cell(), 16};
    vm::AugmentedDictionary augmented_serial{augmented_base.get_root_cell(), 16, augmentation};
    vm::AugmentedDictionary augmented_batch{augmented_base.get_root_cell(), 16, augmentation};

    std::vector<BatchUpdateStorage> storage;
    storage.reserve(96);
    std::set<std::uint16_t> used;
    auto existing = state.begin();
    for (int i = 0; i < 24; ++i, ++existing) {
      used.insert(existing->first);
      storage.push_back({td::BitArray<16>{existing->first}, true, 0});
    }
    for (int i = 0; i < 24; ++i, ++existing) {
      used.insert(existing->first);
      storage.push_back({td::BitArray<16>{existing->first}, false, random()});
    }
    while (storage.size() < 96) {
      auto key = static_cast<std::uint16_t>(random());
      if (state.count(key) || !used.insert(key).second) {
        continue;
      }
      storage.push_back({td::BitArray<16>{key}, false, random()});
    }
    std::shuffle(storage.begin(), storage.end(), random);

    std::vector<std::pair<td::ConstBitPtr, td::Ref<vm::CellBuilder>>> plain_updates;
    std::vector<std::pair<td::ConstBitPtr, td::Ref<vm::CellBuilder>>> augmented_updates;
    plain_updates.reserve(storage.size());
    augmented_updates.reserve(storage.size());
    for (const auto& update : storage) {
      if (update.erase) {
        ASSERT_TRUE(plain_serial.lookup_delete(update.key).not_null());
        ASSERT_TRUE(augmented_serial.lookup_delete(update.key).not_null());
        plain_updates.emplace_back(update.key.bits(), td::Ref<vm::CellBuilder>{});
        augmented_updates.emplace_back(update.key.bits(), td::Ref<vm::CellBuilder>{});
      } else {
        ASSERT_TRUE(plain_serial.set_builder(update.key, make_batch_value(update.value)));
        ASSERT_TRUE(augmented_serial.set_builder(update.key, *make_batch_value(update.value)));
        plain_updates.emplace_back(update.key.bits(), make_batch_value(update.value));
        augmented_updates.emplace_back(update.key.bits(), make_batch_value(update.value));
      }
    }

    ASSERT_TRUE(plain_batch.multiset(plain_updates));
    ASSERT_TRUE(augmented_batch.multiset(augmented_updates));
    assert_same_batch_root(plain_serial.get_root_cell(), plain_batch.get_root_cell());
    assert_same_batch_root(augmented_serial.get_root_cell(), augmented_batch.get_root_cell());
    ASSERT_TRUE(augmented_batch.validate_all());
  }
}

TEST(VM, augmented_dictionary_multiset_modes_match_serial_and_cover_empty_tree) {
  BatchSumAugmentation augmentation;
  std::mt19937 random{0xadd5e7};

  for (int round = 0; round < 32; ++round) {
    std::map<std::uint16_t, std::uint32_t> state;
    while (state.size() < 160) {
      state.emplace(static_cast<std::uint16_t>(random()), random());
    }
    vm::Dictionary unused_plain{16};
    vm::AugmentedDictionary base{16, augmentation};
    add_initial_batch_values(unused_plain, base, state);
    vm::AugmentedDictionary serial{base.get_root_cell(), 16, augmentation};
    vm::AugmentedDictionary batch{base.get_root_cell(), 16, augmentation};

    std::vector<BatchUpdateStorage> storage;
    storage.reserve(112);
    std::set<std::uint16_t> used;
    auto existing = state.begin();
    for (int i = 0; i < 24; ++i, ++existing) {
      used.insert(existing->first);
      storage.push_back({td::BitArray<16>{existing->first}, true, 0, vm::Dictionary::SetMode::Replace});
    }
    for (int i = 0; i < 24; ++i, ++existing) {
      used.insert(existing->first);
      storage.push_back({td::BitArray<16>{existing->first}, false, random(), vm::Dictionary::SetMode::Replace});
    }
    for (int i = 0; i < 24; ++i, ++existing) {
      used.insert(existing->first);
      storage.push_back({td::BitArray<16>{existing->first}, false, random(), vm::Dictionary::SetMode::Set});
    }
    while (storage.size() < 112) {
      auto key = static_cast<std::uint16_t>(random());
      if (state.count(key) || !used.insert(key).second) {
        continue;
      }
      auto mode = storage.size() & 1 ? vm::Dictionary::SetMode::Add : vm::Dictionary::SetMode::Set;
      storage.push_back({td::BitArray<16>{key}, false, random(), mode});
    }
    std::shuffle(storage.begin(), storage.end(), random);

    std::vector<vm::AugmentedDictionary::BatchSetEntry> updates;
    updates.reserve(storage.size());
    for (const auto& update : storage) {
      updates.push_back(
          {update.key.bits(), update.erase ? td::Ref<vm::CellBuilder>{} : make_batch_value(update.value), update.mode});
    }
    for (const auto& update : updates) {
      ASSERT_TRUE(apply_augmented_serial(serial, update));
    }
    ASSERT_TRUE(batch.multiset(updates));
    assert_same_batch_root(serial.get_root_cell(), batch.get_root_cell());
    ASSERT_TRUE(batch.validate_all());
  }

  std::array<td::BitArray<16>, 4> keys{td::BitArray<16>{0LL}, td::BitArray<16>{0x4000}, td::BitArray<16>{0x8000},
                                       td::BitArray<16>{0xffff}};
  vm::AugmentedDictionary serial{16, augmentation};
  vm::AugmentedDictionary batch{16, augmentation};
  std::vector<vm::AugmentedDictionary::BatchSetEntry> inserts;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    inserts.push_back(
        {keys[i].bits(), make_batch_value(static_cast<std::uint32_t>(i + 1)), vm::Dictionary::SetMode::Add});
    ASSERT_TRUE(apply_augmented_serial(serial, inserts.back()));
  }
  ASSERT_TRUE(batch.multiset(inserts));
  assert_same_batch_root(serial.get_root_cell(), batch.get_root_cell());
  for (auto& update : inserts) {
    ASSERT_TRUE(serial.lookup_delete(update.key, 16).not_null());
    update.value.clear();
  }
  ASSERT_TRUE(batch.multiset(inserts));
  ASSERT_TRUE(serial.is_empty());
  ASSERT_TRUE(batch.is_empty());
}

TEST(VM, augmented_dictionary_multiset_has_deterministic_empty_build_load_order) {
  BatchSumAugmentation augmentation;
  std::array<td::BitArray<8>, 4> keys{td::BitArray<8>{0x10}, td::BitArray<8>{0x40}, td::BitArray<8>{0x80},
                                      td::BitArray<8>{0xf0}};

  struct Observation {
    td::Ref<vm::Cell> root;
    std::vector<vm::CellHash> loads;
  };
  auto observe = [&](bool reverse) {
    vm::AugmentedDictionary dictionary{8, augmentation};
    std::vector<std::pair<td::ConstBitPtr, td::Ref<vm::CellBuilder>>> updates;
    for (std::size_t i = 0; i < keys.size(); ++i) {
      updates.emplace_back(keys[i].bits(), make_batch_value(static_cast<std::uint32_t>(i + 1)));
    }
    if (reverse) {
      std::reverse(updates.begin(), updates.end());
    }
    BatchLoadSequenceCounter counter;
    {
      vm::VmStateInterface::Guard guard{&counter};
      ASSERT_TRUE(dictionary.multiset(updates));
    }
    return Observation{dictionary.get_root_cell(), std::move(counter.loads)};
  };

  auto sorted = observe(false);
  auto reversed = observe(true);
  assert_same_batch_root(sorted.root, reversed.root);
  ASSERT_TRUE(sorted.loads == reversed.loads);
  ASSERT_EQ(sorted.loads.size(), 10u);
  for (const auto& hash : sorted.loads) {
    ASSERT_TRUE(hash != sorted.root->get_hash());
  }
}

TEST(VM, augmented_dictionary_multiset_failures_are_atomic) {
  BatchSumAugmentation augmentation;
  vm::AugmentedDictionary base{16, augmentation};
  td::BitArray<16> key1{0x1000};
  td::BitArray<16> key2{0x2000};
  td::BitArray<16> key3{0x3000};
  td::BitArray<16> missing{0xf000};
  ASSERT_TRUE(base.set_builder(key1, *make_batch_value(1), vm::Dictionary::SetMode::Add));
  ASSERT_TRUE(base.set_builder(key2, *make_batch_value(2), vm::Dictionary::SetMode::Add));
  auto initial_root = base.get_root_cell();

  auto check_pair_failure = [&](std::vector<std::pair<td::ConstBitPtr, td::Ref<vm::CellBuilder>>> updates) {
    vm::AugmentedDictionary dictionary{initial_root, 16, augmentation};
    ASSERT_TRUE(!dictionary.multiset(updates));
    for (std::size_t i = 1; i < updates.size(); ++i) {
      ASSERT_TRUE(td::bitstring::bits_memcmp(updates[i - 1].first, updates[i].first, 16) <= 0);
    }
    ASSERT_TRUE(dictionary.get_root_cell().get() == initial_root.get());
    assert_same_batch_root(initial_root, dictionary.get_root_cell());
  };
  check_pair_failure({{key3.bits(), make_batch_value(3)}, {key3.bits(), make_batch_value(4)}});
  check_pair_failure({{missing.bits(), {}}, {key1.bits(), make_batch_value(10)}});

  auto check_mode_failure = [&](std::vector<vm::AugmentedDictionary::BatchSetEntry> updates) {
    vm::AugmentedDictionary dictionary{initial_root, 16, augmentation};
    ASSERT_TRUE(!dictionary.multiset(updates));
    for (std::size_t i = 1; i < updates.size(); ++i) {
      ASSERT_TRUE(td::bitstring::bits_memcmp(updates[i - 1].key, updates[i].key, 16) <= 0);
    }
    ASSERT_TRUE(dictionary.get_root_cell().get() == initial_root.get());
    assert_same_batch_root(initial_root, dictionary.get_root_cell());
  };
  check_mode_failure({{key3.bits(), make_batch_value(3), vm::Dictionary::SetMode::Add},
                      {key1.bits(), make_batch_value(10), vm::Dictionary::SetMode::Add}});
  check_mode_failure({{missing.bits(), make_batch_value(9), vm::Dictionary::SetMode::Replace},
                      {key1.bits(), make_batch_value(10), vm::Dictionary::SetMode::Replace}});
  check_mode_failure({{missing.bits(), {}, vm::Dictionary::SetMode::Set},
                      {key1.bits(), make_batch_value(10), vm::Dictionary::SetMode::Replace}});
  check_mode_failure({{key3.bits(), make_batch_value(3), vm::Dictionary::SetMode::Add},
                      {key3.bits(), make_batch_value(4), vm::Dictionary::SetMode::Set}});

  vm::AugmentedDictionary no_op{initial_root, 16, augmentation};
  std::vector<std::pair<td::ConstBitPtr, td::Ref<vm::CellBuilder>>> no_pair_updates;
  std::vector<vm::AugmentedDictionary::BatchSetEntry> no_mode_updates;
  ASSERT_TRUE(no_op.multiset(no_pair_updates));
  ASSERT_TRUE(no_op.get_root_cell().get() == initial_root.get());
  ASSERT_TRUE(no_op.multiset(no_mode_updates));
  ASSERT_TRUE(no_op.get_root_cell().get() == initial_root.get());

  FailingBatchAugmentation failing;
  vm::AugmentedDictionary failure_base{16, failing};
  ASSERT_TRUE(failure_base.set_builder(key1, *make_batch_value(1), vm::Dictionary::SetMode::Add));
  ASSERT_TRUE(failure_base.set_builder(key2, *make_batch_value(2), vm::Dictionary::SetMode::Add));
  auto failure_root = failure_base.get_root_cell();
  std::vector<vm::AugmentedDictionary::BatchSetEntry> updates{
      {key1.bits(), make_batch_value(11), vm::Dictionary::SetMode::Replace},
      {key3.bits(), make_batch_value(3), vm::Dictionary::SetMode::Add}};
  failing.fail_after(1);
  bool threw = false;
  try {
    static_cast<void>(failure_base.multiset(updates));
  } catch (const vm::VmError& error) {
    threw = true;
    ASSERT_EQ(error.get_errno(), static_cast<int>(vm::Excno::dict_err));
  }
  ASSERT_TRUE(threw);
  ASSERT_TRUE(failure_base.get_root_cell().get() == failure_root.get());
  failing.allow_all();
  assert_same_batch_root(failure_root, failure_base.get_root_cell());
}

TEST(VM, augmented_dictionary_multiset_preserves_lazy_usage_and_malformed_load_order) {
  BatchSumAugmentation augmentation;
  vm::AugmentedDictionary base{16, augmentation};
  const std::pair<std::uint16_t, std::uint32_t> initial_values[] = {
      {0x1000, 10}, {0x4000, 40}, {0x8000, 80}, {0xf000, 150}};
  for (const auto& [key_value, value] : initial_values) {
    td::BitArray<16> key{key_value};
    ASSERT_TRUE(base.set_builder(key, *make_batch_value(value), vm::Dictionary::SetMode::Add));
  }
  auto raw_root = base.get_root_cell();
  td::BitArray<16> deleted_key{0x4000};
  td::BitArray<16> added_key{0x6000};
  td::BitArray<16> replaced_key{0x8000};
  auto make_updates = [&] {
    return std::vector<vm::AugmentedDictionary::BatchSetEntry>{
        {replaced_key.bits(), make_batch_value(81), vm::Dictionary::SetMode::Replace},
        {deleted_key.bits(), {}, vm::Dictionary::SetMode::Set},
        {added_key.bits(), make_batch_value(60), vm::Dictionary::SetMode::Add}};
  };

  vm::AugmentedDictionary expected{raw_root, 16, augmentation};
  auto expected_updates = make_updates();
  ASSERT_TRUE(expected.multiset(expected_updates));

  auto lazy_loads = std::make_shared<std::vector<vm::CellHash>>();
  auto lazy_root = make_lazy_lookup_tree(raw_root, lazy_loads);
  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  std::vector<vm::CellHash> usage_loads;
  usage_tree->set_cell_load_callback(
      [&](const vm::LoadedCell& loaded) { usage_loads.push_back(loaded.data_cell->get_hash()); });
  auto usage_root = vm::UsageCell::create(std::move(lazy_root), usage_tree->root_ptr());
  vm::AugmentedDictionary contextual{usage_root, 16, augmentation};
  auto contextual_updates = make_updates();
  ASSERT_TRUE(contextual.multiset(contextual_updates));
  assert_same_batch_root(expected.get_root_cell(), contextual.get_root_cell());
  ASSERT_TRUE(!lazy_loads->empty());
  std::vector<vm::CellHash> first_lazy_loads;
  for (const auto& hash : *lazy_loads) {
    if (std::find(first_lazy_loads.begin(), first_lazy_loads.end(), hash) == first_lazy_loads.end()) {
      first_lazy_loads.push_back(hash);
    }
  }
  ASSERT_TRUE(first_lazy_loads == usage_loads);
  auto root_id = usage_tree->root_id();
  ASSERT_TRUE(usage_tree->is_loaded(root_id));
  ASSERT_TRUE(usage_tree->is_loaded(usage_tree->get_child(root_id, 0)));
  ASSERT_TRUE(usage_tree->is_loaded(usage_tree->get_child(root_id, 1)));

  auto malformed_data = vm::CellBuilder{}.store_long(2, 2).finalize_novm();
  auto malformed_loads = std::make_shared<std::vector<vm::CellHash>>();
  auto malformed_lazy = LookupLazyCell::create(std::move(malformed_data), malformed_loads);
  auto malformed_usage_tree = std::make_shared<vm::CellUsageTree>();
  std::vector<vm::CellHash> malformed_usage_loads;
  malformed_usage_tree->set_cell_load_callback(
      [&](const vm::LoadedCell& loaded) { malformed_usage_loads.push_back(loaded.data_cell->get_hash()); });
  auto malformed_root = vm::UsageCell::create(malformed_lazy, malformed_usage_tree->root_ptr());
  vm::AugmentedDictionary malformed{malformed_root, 16, augmentation};
  std::vector<vm::AugmentedDictionary::BatchSetEntry> malformed_updates{
      {added_key.bits(), make_batch_value(1), vm::Dictionary::SetMode::Add}};
  bool threw = false;
  try {
    static_cast<void>(malformed.multiset(malformed_updates));
  } catch (const vm::VmError& error) {
    threw = true;
    ASSERT_EQ(error.get_errno(), static_cast<int>(vm::Excno::cell_und));
    ASSERT_TRUE(std::string{error.get_msg()} == "error while parsing a dictionary node label");
  }
  ASSERT_TRUE(threw);
  ASSERT_TRUE(malformed.get_root_cell().get() == malformed_root.get());
  ASSERT_EQ(malformed_loads->size(), 1u);
  ASSERT_TRUE(*malformed_loads == malformed_usage_loads);
}

TEST(VM, augmented_dictionary_multiset_mutates_virtualized_proof_paths) {
  BatchSumAugmentation augmentation;
  vm::AugmentedDictionary base{16, augmentation};
  auto make_proof_value = [](std::uint32_t value) {
    auto payload_tail = vm::CellBuilder{}.store_long(value ^ 0x5a5a5a5aU, 32).finalize_novm();
    auto payload =
        vm::CellBuilder{}.store_long(value ^ 0xa5a5a5a5U, 32).store_ref(std::move(payload_tail)).finalize_novm();
    auto builder = make_batch_value(value);
    CHECK(builder.write().store_ref_bool(std::move(payload)));
    return builder;
  };
  constexpr std::array<std::uint16_t, 8> initial_keys{0x0000, 0x1000, 0x2000, 0x4000, 0x8000, 0xa000, 0xe000, 0xffff};
  for (std::size_t i = 0; i < initial_keys.size(); ++i) {
    td::BitArray<16> key{initial_keys[i]};
    ASSERT_TRUE(
        base.set_builder(key, *make_proof_value(static_cast<std::uint32_t>(i + 1)), vm::Dictionary::SetMode::Add));
  }
  td::BitArray<16> replaced{0x2000};
  td::BitArray<16> deleted{0xa000};
  auto make_updates = [&] {
    return std::vector<vm::AugmentedDictionary::BatchSetEntry>{
        {replaced.bits(), make_proof_value(99), vm::Dictionary::SetMode::Replace},
        {deleted.bits(), {}, vm::Dictionary::SetMode::Set}};
  };

  auto pruned_payload_tail = vm::CellBuilder{}.store_long(1U ^ 0x5a5a5a5aU, 32).finalize_novm();
  auto pruned_payload =
      vm::CellBuilder{}.store_long(1U ^ 0xa5a5a5a5U, 32).store_ref(std::move(pruned_payload_tail)).finalize_novm();
  std::size_t pruned_matches = 0;
  auto proof = vm::MerkleProof::generate(base.get_root_cell(), [&](const td::Ref<vm::Cell>& cell) {
                 if (cell->get_hash() != pruned_payload->get_hash()) {
                   return false;
                 }
                 ++pruned_matches;
                 return true;
               }).move_as_ok();
  ASSERT_EQ(pruned_matches, 1u);
  auto full_proof =
      vm::MerkleProof::generate(base.get_root_cell(), [](const td::Ref<vm::Cell>&) { return false; }).move_as_ok();
  auto proof_boc = vm::std_boc_serialize(proof, 31).move_as_ok();
  auto full_proof_boc = vm::std_boc_serialize(std::move(full_proof), 31).move_as_ok();
  ASSERT_TRUE(proof_boc.as_slice() != full_proof_boc.as_slice());
  auto virtual_root = vm::MerkleProof::virtualize(std::move(proof)).move_as_ok();

  vm::AugmentedDictionary full{base.get_root_cell(), 16, augmentation};
  vm::AugmentedDictionary virtualized{virtual_root, 16, augmentation};
  auto full_updates = make_updates();
  auto virtual_updates = make_updates();
  ASSERT_TRUE(full.multiset(full_updates));
  ASSERT_TRUE(virtualized.multiset(virtual_updates));
  ASSERT_EQ(full.get_root_cell()->get_hash(), virtualized.get_root_cell()->get_hash());
}
