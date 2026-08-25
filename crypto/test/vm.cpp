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
#include <sstream>
#include <type_traits>
#include <utility>

#include "common/bigint.hpp"
#include "fift/utils.h"
#include "td/utils/ScopeGuard.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/base64.h"
#include "td/utils/tests.h"
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

td::Ref<vm::Cell> to_cell(const unsigned char *buff, int bits) {
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
  auto *target_alias = &target;
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
  old_tree->set_cell_load_callback([&](const auto &) { ++old_loads; });
  source_tree->set_cell_load_callback([&](const auto &) { ++source_loads; });
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
