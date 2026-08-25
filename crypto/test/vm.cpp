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

#include "common/bigint.hpp"
#include "fift/utils.h"
#include "td/utils/ScopeGuard.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/base64.h"
#include "td/utils/tests.h"
#include "vm/cp0.h"
#include "vm/dict.h"
#include "vm/opctable.h"
#include "vm/vm.h"

namespace vm {

class OpcodeTableTestAccess {
 public:
  struct SliceLookup {
    const OpcodeInstr* instr;
    unsigned opcode;
    unsigned bits;
  };

  static const OpcodeInstr* lookup_fast(const OpcodeTable& table, unsigned opcode, unsigned bits = max_opcode_bits) {
    return table.lookup_instr(opcode, bits);
  }

  // This is the predecessor search used before the prefix index was added.
  static const OpcodeInstr* lookup_legacy(const OpcodeTable& table, unsigned opcode) {
    std::size_t i = 0;
    std::size_t j = table.instruction_list.size();
    while (j - i > 1) {
      auto k = ((j + i) >> 1);
      if (table.instruction_list[k].first <= opcode) {
        i = k;
      } else {
        j = k;
      }
    }
    return table.instruction_list[i].second;
  }

  static SliceLookup lookup_slice(const OpcodeTable& table, const CellSlice& cs) {
    unsigned opcode;
    unsigned bits;
    auto instr = table.lookup_instr(cs, opcode, bits);
    return {instr, opcode, bits};
  }

  static std::size_t interval_count(const OpcodeTable& table) {
    return table.instruction_list.size();
  }
};

}  // namespace vm

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

namespace {

void assert_opcode_table_matches_legacy_exhaustively(const vm::OpcodeTable& table) {
  ASSERT_TRUE(table.is_final());
  ASSERT_TRUE(vm::OpcodeTableTestAccess::interval_count(table) != 0);
  for (unsigned opcode = 0; opcode < vm::top_opcode; ++opcode) {
    auto fast = vm::OpcodeTableTestAccess::lookup_fast(table, opcode);
    auto legacy = vm::OpcodeTableTestAccess::lookup_legacy(table, opcode);
    if (fast != legacy) {
      LOG(FATAL) << "opcode prefix lookup mismatch at 24-bit opcode " << opcode;
    }
  }
}

const vm::OpcodeTable* get_adversarial_opcode_table() {
  static const auto* table = [] {
    auto* result = new vm::OpcodeTable{"ADVERSARIAL TEST CODEPAGE", static_cast<vm::Codepage>(0x6ffd)};
    constexpr std::pair<unsigned, unsigned> ranges[] = {
        {0x000000, 0x000001},       {0x000007, 0x00000a}, {0x000ffd, 0x000fff}, {0x000fff, 0x001000},
        {0x001000, 0x001001},       {0x001001, 0x001003}, {0x0017ff, 0x001801}, {0x001ffe, 0x002002},
        {0x002002, 0x002003},       {0x123000, 0x123001}, {0x123001, 0x123002}, {0x1237ff, 0x123801},
        {0x123ffe, 0x124002},       {0x7ffffe, 0x800002}, {0xffeffe, 0xfff002}, {0xfffffe, 0xffffff},
        {0xffffff, vm::top_opcode},
    };
    for (const auto& [begin, end] : ranges) {
      result->insert(new vm::OpcodeInstrDummy{begin, end});
    }
    ASSERT_EQ(result->finalize(), result);
    ASSERT_EQ(result->finalize(), result);
    return result;
  }();
  return table;
}

const vm::OpcodeTable* get_empty_opcode_table() {
  static const auto* table = [] {
    auto* result = new vm::OpcodeTable{"EMPTY TEST CODEPAGE", static_cast<vm::Codepage>(0x6ffc)};
    ASSERT_EQ(result->finalize(), result);
    return result;
  }();
  return table;
}

void assert_truncated_opcode_lookup(const vm::OpcodeTable& table, unsigned bits, unsigned pattern) {
  ASSERT_TRUE(bits <= vm::max_opcode_bits);
  ASSERT_TRUE(pattern < (1U << bits));
  unsigned opcode = pattern << (vm::max_opcode_bits - bits);
  std::array<unsigned char, 3> bytes{
      static_cast<unsigned char>(opcode >> 16),
      static_cast<unsigned char>(opcode >> 8),
      static_cast<unsigned char>(opcode),
  };
  vm::CellSlice cs{vm::NoVm{}, to_cell(bytes.data(), static_cast<int>(bits))};
  auto observed = vm::OpcodeTableTestAccess::lookup_slice(table, cs);
  ASSERT_EQ(observed.bits, bits);
  ASSERT_EQ(observed.opcode, opcode);
  ASSERT_EQ(observed.instr, vm::OpcodeTableTestAccess::lookup_legacy(table, opcode));
}

}  // namespace

TEST(VM, opcode_table_prefix_index_matches_legacy_cp0_exhaustively) {
  const auto* cp0 = vm::init_op_cp0();
  ASSERT_TRUE(cp0 != nullptr);
  assert_opcode_table_matches_legacy_exhaustively(*cp0);
}

TEST(VM, opcode_table_prefix_index_handles_adversarial_and_empty_tables) {
  const auto* adversarial = get_adversarial_opcode_table();
  assert_opcode_table_matches_legacy_exhaustively(*adversarial);

  const auto* empty = get_empty_opcode_table();
  ASSERT_EQ(vm::OpcodeTableTestAccess::interval_count(*empty), 1u);
  constexpr unsigned probes[] = {0, 1, 0x000fff, 0x001000, 0x7fffff, 0x800000, 0xffefff, 0xfff000, 0xfffffe, 0xffffff};
  for (unsigned opcode : probes) {
    ASSERT_EQ(vm::OpcodeTableTestAccess::lookup_fast(*empty, opcode),
              vm::OpcodeTableTestAccess::lookup_legacy(*empty, opcode));
  }
}

TEST(VM, opcode_table_prefix_index_preserves_truncated_slice_lookup) {
  const auto& cp0 = *vm::init_op_cp0();

  // Exhaust every bit pattern through the prefix width, including the empty slice.
  for (unsigned bits = 0; bits <= 12; ++bits) {
    for (unsigned pattern = 0; pattern < (1U << bits); ++pattern) {
      assert_truncated_opcode_lookup(cp0, bits, pattern);
    }
  }

  // For longer slices, exercise both ends of every 12-bit prefix. Full 24-bit
  // opcode selection is already exhaustive in the test above.
  for (unsigned bits = 13; bits <= vm::max_opcode_bits; ++bits) {
    unsigned suffix_bits = bits - 12;
    unsigned suffix_mask = (1U << suffix_bits) - 1;
    for (unsigned prefix = 0; prefix < (1U << 12); ++prefix) {
      unsigned pattern = prefix << suffix_bits;
      assert_truncated_opcode_lookup(cp0, bits, pattern);
      assert_truncated_opcode_lookup(cp0, bits, pattern | suffix_mask);
    }
  }
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
