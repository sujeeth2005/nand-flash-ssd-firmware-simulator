#include "nandsim/flash_model.hpp"
#include "test_framework.hpp"

using namespace nandsim;

namespace {
FlashGeometry small_geo() { return FlashGeometry{8, 4, 64}; }
std::vector<uint8_t> pat(uint8_t v, size_t n = 64) { return std::vector<uint8_t>(n, v); }
}  // namespace

TEST(flash, fresh_pages_read_as_erased) {
    FlashModel f(small_geo());
    std::vector<uint8_t> d; Oob o;
    REQUIRE_EQ(f.read_page(0, &d, &o), Status::Ok);
    CHECK(d == pat(0xFF));
    CHECK_EQ(o.lpn, kInvalid);
    CHECK(!f.is_page_programmed(0));
}

TEST(flash, program_then_read_roundtrip) {
    FlashModel f(small_geo());
    auto data = pat(0xAB);
    REQUIRE_EQ(f.program_page(0, data.data(), data.size(), Oob{7, 42}), Status::Ok);
    std::vector<uint8_t> d; Oob o;
    REQUIRE_EQ(f.read_page(0, &d, &o), Status::Ok);
    CHECK(d == data);
    CHECK_EQ(o.lpn, 7u);
    CHECK_EQ(o.seq, 42u);
}

TEST(flash, cannot_program_twice_without_erase) {
    FlashModel f(small_geo());
    auto d = pat(1);
    REQUIRE_EQ(f.program_page(0, d.data(), d.size(), Oob{0, 1}), Status::Ok);
    CHECK_EQ(f.program_page(0, d.data(), d.size(), Oob{0, 2}), Status::ErrNotErased);
}

TEST(flash, pages_must_be_programmed_in_order) {
    FlashModel f(small_geo());
    auto d = pat(1);
    CHECK_EQ(f.program_page(2, d.data(), d.size(), Oob{0, 1}), Status::ErrOutOfOrder);
    REQUIRE_EQ(f.program_page(0, d.data(), d.size(), Oob{0, 1}), Status::Ok);
    CHECK_EQ(f.program_page(2, d.data(), d.size(), Oob{0, 2}), Status::ErrOutOfOrder);
    CHECK_EQ(f.program_page(1, d.data(), d.size(), Oob{0, 3}), Status::Ok);
}

TEST(flash, invalid_addresses_and_sizes) {
    FlashModel f(small_geo());
    auto d = pat(1);
    std::vector<uint8_t> out;
    CHECK_EQ(f.read_page(32, &out, nullptr), Status::ErrInvalidAddress);
    CHECK_EQ(f.program_page(32, d.data(), d.size(), Oob{}), Status::ErrInvalidAddress);
    CHECK_EQ(f.erase_block(8), Status::ErrInvalidAddress);
    CHECK_EQ(f.program_page(0, d.data(), 10, Oob{}), Status::ErrInvalidArgument);
}

TEST(flash, erase_resets_block_and_counts_cycles) {
    FlashModel f(small_geo());
    auto d = pat(5);
    for (uint32_t i = 0; i < 4; ++i) REQUIRE_EQ(f.program_page(i, d.data(), d.size(), Oob{i, i + 1}), Status::Ok);
    REQUIRE_EQ(f.erase_block(0), Status::Ok);
    CHECK_EQ(f.erase_count(0), 1u);
    CHECK_EQ(f.programmed_pages(0), 0u);
    CHECK_EQ(f.program_page(0, d.data(), d.size(), Oob{0, 9}), Status::Ok);
    CHECK_EQ(f.erase_count(1), 0u);  // other blocks untouched
}

TEST(flash, block_wears_out_and_goes_bad) {
    FlashFaultConfig fc; fc.max_pe_cycles = 3;
    FlashModel f(small_geo(), {}, fc);
    for (int i = 0; i < 3; ++i) REQUIRE_EQ(f.erase_block(2), Status::Ok);
    CHECK_EQ(f.erase_block(2), Status::ErrEraseFail);
    CHECK(f.is_bad(2));
    auto d = pat(1);
    CHECK_EQ(f.program_page(2 * 4, d.data(), d.size(), Oob{}), Status::ErrBadBlock);
    CHECK_EQ(f.erase_block(2), Status::ErrBadBlock);
}

TEST(flash, factory_bad_block_rejects_io) {
    FlashModel f(small_geo());
    f.mark_factory_bad(1);
    auto d = pat(1);
    std::vector<uint8_t> out;
    CHECK_EQ(f.program_page(4, d.data(), d.size(), Oob{}), Status::ErrBadBlock);
    CHECK_EQ(f.read_page(4, &out, nullptr), Status::ErrBadBlock);
}

TEST(flash, injected_program_failure_burns_the_page) {
    FlashModel f(small_geo());
    auto d = pat(1);
    f.inject_program_fail(0);
    CHECK_EQ(f.program_page(0, d.data(), d.size(), Oob{0, 1}), Status::ErrProgramFail);
    CHECK(f.is_page_programmed(0));
    std::vector<uint8_t> out;
    CHECK_EQ(f.read_page(0, &out, nullptr), Status::ErrUncorrectable);
    CHECK_EQ(f.program_page(1, d.data(), d.size(), Oob{0, 2}), Status::Ok);  // block still usable
}

TEST(flash, power_loss_torn_program_leaves_unreadable_page) {
    FlashModel f(small_geo());
    auto d = pat(0x11);
    f.arm_power_loss(1, TearMode::Torn);
    REQUIRE_EQ(f.program_page(0, d.data(), d.size(), Oob{0, 1}), Status::Ok);
    CHECK_EQ(f.program_page(1, d.data(), d.size(), Oob{1, 2}), Status::ErrPowerLoss);
    CHECK(!f.powered());
    std::vector<uint8_t> out;
    CHECK_EQ(f.read_page(0, &out, nullptr), Status::ErrPowerLoss);  // device is off
    f.power_cycle();
    CHECK_EQ(f.read_page(0, &out, nullptr), Status::Ok);
    CHECK(out == d);
    CHECK_EQ(f.read_page(1, &out, nullptr), Status::ErrUncorrectable);  // torn
}

TEST(flash, power_loss_lost_program_leaves_page_erased) {
    FlashModel f(small_geo());
    auto d = pat(0x11);
    f.arm_power_loss(0, TearMode::Lost);
    CHECK_EQ(f.program_page(0, d.data(), d.size(), Oob{0, 1}), Status::ErrPowerLoss);
    f.power_cycle();
    CHECK(!f.is_page_programmed(0));
}

TEST(flash, power_loss_torn_erase_requires_reerase) {
    FlashModel f(small_geo());
    auto d = pat(0x22);
    REQUIRE_EQ(f.program_page(0, d.data(), d.size(), Oob{0, 1}), Status::Ok);
    f.arm_power_loss(0, TearMode::Torn);
    CHECK_EQ(f.erase_block(0), Status::ErrPowerLoss);
    f.power_cycle();
    std::vector<uint8_t> out;
    CHECK_EQ(f.read_page(0, &out, nullptr), Status::ErrUncorrectable);
    CHECK_EQ(f.program_page(0, d.data(), d.size(), Oob{}), Status::ErrNotErased);
    CHECK_EQ(f.erase_block(0), Status::Ok);
    CHECK_EQ(f.program_page(0, d.data(), d.size(), Oob{0, 5}), Status::Ok);
}

TEST(flash, ecc_corrects_small_errors_and_fails_large_ones) {
    FlashFaultConfig fc; fc.raw_bit_error_prob = 0.5; fc.ecc_correctable_bits = 2; fc.seed = 3;
    FlashModel f(small_geo(), {}, fc);
    auto d = pat(0x33);
    REQUIRE_EQ(f.program_page(0, d.data(), d.size(), Oob{0, 1}), Status::Ok);
    int ok_reads = 0, bad_reads = 0;
    std::vector<uint8_t> out;
    for (int i = 0; i < 400; ++i) {
        Status s = f.read_page(0, &out, nullptr);
        if (s == Status::Ok) { ++ok_reads; CHECK(out == d); }
        else if (s == Status::ErrUncorrectable) ++bad_reads;
    }
    CHECK(ok_reads > 0);
    CHECK(bad_reads > 0);
    CHECK(f.stats().ecc_corrected_reads > 0);
    CHECK_EQ(f.stats().ecc_uncorrectable_reads, uint64_t(bad_reads));
}

TEST(flash, simulated_time_accumulates) {
    FlashTiming t; t.read_ns = 10; t.program_ns = 100; t.erase_ns = 1000;
    FlashModel f(small_geo(), t);
    auto d = pat(1);
    std::vector<uint8_t> out;
    f.program_page(0, d.data(), d.size(), Oob{0, 1});
    f.read_page(0, &out, nullptr);
    f.erase_block(0);
    CHECK_EQ(f.stats().busy_ns, 1110u);
    f.reset_stats();
    CHECK_EQ(f.stats().busy_ns, 0u);
    CHECK_EQ(f.busy_ns(), 1110u);
}
