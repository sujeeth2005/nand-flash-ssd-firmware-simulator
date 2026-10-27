#include "nandsim/ftl.hpp"
#include "nandsim/util.hpp"
#include "test_framework.hpp"

using namespace nandsim;

namespace {
struct Rig {
    FlashModel flash;
    Ftl ftl;
    explicit Rig(FtlConfig cfg = {}, FlashGeometry g = {64, 16, 64})
        : flash(g), ftl(flash, cfg) { ftl.mount(); }
    Status write(Lpn l, uint32_t ver) { return ftl.write(l, make_page_data(l, ver, ftl.page_size())); }
    bool expect(Lpn l, uint32_t ver) {
        std::vector<uint8_t> b;
        return ftl.read(l, b) == Status::Ok && b == make_page_data(l, ver, ftl.page_size());
    }
};
}  // namespace

TEST(ftl, geometry_and_overprovisioning) {
    Rig r;
    CHECK_EQ(r.ftl.op_blocks(), 8u);  // 12.5% of 64
    CHECK_EQ(r.ftl.logical_pages(), (64u - 8u) * 16u);
    CHECK_EQ(r.ftl.free_block_count(), 64u);
    CHECK(r.ftl.check_invariants().empty());
}

TEST(ftl, rejects_bad_configuration) {
    FlashModel f(FlashGeometry{8, 16, 64});
    bool threw = false;
    try { Ftl x(f, FtlConfig{}); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);  // 8 blocks cannot satisfy the minimum OP
}

TEST(ftl, write_read_roundtrip) {
    Rig r;
    REQUIRE_EQ(r.write(5, 1), Status::Ok);
    CHECK(r.expect(5, 1));
    CHECK(r.ftl.is_mapped(5));
}

TEST(ftl, overwrite_returns_latest_and_invalidates_old_page) {
    Rig r;
    REQUIRE_EQ(r.write(3, 1), Status::Ok);
    Ppn first = r.ftl.physical_of(3);
    REQUIRE_EQ(r.write(3, 2), Status::Ok);
    CHECK(r.ftl.physical_of(3) != first);
    CHECK(r.expect(3, 2));
    CHECK_EQ(r.ftl.block_info(r.ftl.host_block()).valid_count, 1u);  // only the newest copy is valid
    CHECK(r.ftl.check_invariants().empty());
}

TEST(ftl, unmapped_read_returns_zeros) {
    Rig r;
    std::vector<uint8_t> b;
    CHECK_EQ(r.ftl.read(9, b), Status::Unmapped);
    CHECK(b == std::vector<uint8_t>(64, 0));
}

TEST(ftl, out_of_range_and_bad_size_rejected) {
    Rig r;
    CHECK_EQ(r.write(r.ftl.logical_pages(), 1), Status::ErrInvalidAddress);
    std::vector<uint8_t> b;
    CHECK_EQ(r.ftl.read(r.ftl.logical_pages(), b), Status::ErrInvalidAddress);
    CHECK_EQ(r.ftl.write(0, std::vector<uint8_t>(10)), Status::ErrInvalidArgument);
}

TEST(ftl, trim_unmaps_and_frees_the_page) {
    Rig r;
    REQUIRE_EQ(r.write(4, 1), Status::Ok);
    REQUIRE_EQ(r.ftl.trim(4), Status::Ok);
    std::vector<uint8_t> b;
    CHECK_EQ(r.ftl.read(4, b), Status::Unmapped);
    CHECK_EQ(r.ftl.block_info(r.ftl.host_block()).valid_count, 0u);
    CHECK_EQ(r.ftl.trim(4), Status::Ok);  // idempotent
    CHECK(r.ftl.check_invariants().empty());
}

TEST(ftl, fill_entire_logical_space_sequentially) {
    Rig r;
    uint32_t L = r.ftl.logical_pages();
    for (Lpn l = 0; l < L; ++l) REQUIRE_EQ(r.write(l, l + 1), Status::Ok);
    for (Lpn l = 0; l < L; ++l) REQUIRE(r.expect(l, l + 1));
    CHECK(r.ftl.check_invariants().empty());
    CHECK_EQ(r.ftl.stats().gc_runs, 0u);  // sequential fill never needs GC
    CHECK(r.ftl.write_amplification() > 0.99 && r.ftl.write_amplification() < 1.01);
}

TEST(ftl, write_stream_is_sequential_inside_blocks) {
    // The flash model rejects out-of-order programming, so a long run without errors proves
    // the FTL honours NAND's program-order rule across blocks and GC.
    Rig r;
    Rng rng(5);
    uint32_t L = r.ftl.logical_pages();
    for (int i = 0; i < 20000; ++i) REQUIRE_EQ(r.write(Lpn(rng.below(L)), i + 1), Status::Ok);
}

TEST(ftl, mount_on_fresh_flash_is_empty) {
    Rig r;
    CHECK_EQ(r.ftl.recovery_report().valid_pages, 0u);
    CHECK_EQ(r.ftl.recovery_report().pages_scanned, 0u);
}

TEST(ftl, program_failure_is_retried_on_another_block) {
    Rig r;
    REQUIRE_EQ(r.write(0, 1), Status::Ok);
    BlockId hb = r.ftl.host_block();
    r.flash.inject_program_fail(hb);
    REQUIRE_EQ(r.write(1, 1), Status::Ok);  // first attempt fails, retry succeeds
    CHECK_EQ(r.ftl.stats().program_retries, 1u);
    CHECK(r.ftl.host_block() != hb);
    CHECK(r.expect(0, 1));
    CHECK(r.expect(1, 1));
    CHECK(r.ftl.check_invariants().empty());
}

TEST(ftl, uncorrectable_read_is_reported_not_hidden) {
    Rig r;
    REQUIRE_EQ(r.write(0, 1), Status::Ok);
    r.flash.set_raw_bit_error_prob(1.0);
    r.flash.set_ecc_correctable_bits(0);
    std::vector<uint8_t> b;
    CHECK_EQ(r.ftl.read(0, b), Status::ErrUncorrectable);
    CHECK_EQ(r.ftl.stats().read_errors, 1u);
}

TEST(ftl, factory_bad_blocks_are_never_used) {
    FlashModel flash(FlashGeometry{64, 16, 64});
    flash.mark_factory_bad(3);
    flash.mark_factory_bad(40);
    Ftl ftl(flash, FtlConfig{});
    REQUIRE_EQ(ftl.mount(), Status::Ok);
    Rng rng(2);
    uint32_t L = ftl.logical_pages();
    for (int i = 0; i < 15000; ++i)
        REQUIRE_EQ(ftl.write(Lpn(rng.below(L)), make_page_data(0, 1, 64)), Status::Ok);
    CHECK(ftl.check_invariants().empty());
    CHECK_EQ(flash.programmed_pages(3), 0u);
    CHECK_EQ(flash.programmed_pages(40), 0u);
}

TEST(ftl, worn_out_device_degrades_gracefully_without_data_loss) {
    FlashFaultConfig fc; fc.max_pe_cycles = 12;
    FlashModel flash(FlashGeometry{32, 16, 64}, {}, fc);
    Ftl ftl(flash, FtlConfig{0.25});
    REQUIRE_EQ(ftl.mount(), Status::Ok);
    uint32_t L = ftl.logical_pages();
    std::vector<uint32_t> ver(L, 0);
    Rng rng(9);
    Status last = Status::Ok;
    uint32_t v = 0;
    for (int i = 0; i < 200000 && last == Status::Ok; ++i) {
        Lpn l = Lpn(rng.below(L));
        last = ftl.write(l, make_page_data(l, ++v, 64));
        if (last == Status::Ok) ver[l] = v;
    }
    CHECK_EQ(last, Status::ErrNoSpace);  // terminates with a clean error, no hang / crash
    CHECK(ftl.stats().bad_blocks_retired > 0);
    CHECK(ftl.check_invariants().empty());
    for (Lpn l = 0; l < L; ++l) {  // everything that was acknowledged is still readable
        if (!ver[l]) continue;
        std::vector<uint8_t> b;
        REQUIRE_EQ(ftl.read(l, b), Status::Ok);
        REQUIRE(b == make_page_data(l, ver[l], 64));
    }
}
