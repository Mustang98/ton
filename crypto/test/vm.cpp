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
#include <memory>
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
