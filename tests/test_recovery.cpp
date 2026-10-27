#include "nandsim/crash_harness.hpp"
#include "nandsim/ftl.hpp"
#include "nandsim/util.hpp"
#include "test_framework.hpp"

using namespace nandsim;

namespace {
CrashTrialConfig base_cfg(GcPolicy gc, WearMode wl) {
    CrashTrialConfig c;
    c.geo = FlashGeometry{32, 16, 32};
    c.ftl.overprovision = 0.25;
    c.ftl.gc_policy = gc;
    c.ftl.wear_mode = wl;
    c.ftl.static_wl_threshold = 3;
    c.ftl.static_wl_interval = 1;
    return c;
}
}  // namespace

TEST(recovery, clean_remount_restores_every_mapping) {
    FlashModel flash(FlashGeometry{64, 16, 64});
    FtlConfig cfg;
    std::vector<uint32_t> ver;
    {
        Ftl ftl(flash, cfg);
        REQUIRE_EQ(ftl.mount(), Status::Ok);
        uint32_t L = ftl.logical_pages();
        ver.assign(L, 0);
        Rng rng(1);
        uint32_t v = 0;
        for (int i = 0; i < 20000; ++i) {
            Lpn l = Lpn(rng.below(L));
            REQUIRE_EQ(ftl.write(l, make_page_data(l, ++v, 64)), Status::Ok);
            ver[l] = v;
        }
    }
    Ftl ftl2(flash, cfg);
    REQUIRE_EQ(ftl2.mount(), Status::Ok);
    CHECK(ftl2.check_invariants().empty());
    for (Lpn l = 0; l < ver.size(); ++l) {
        std::vector<uint8_t> b;
        if (!ver[l]) { CHECK_EQ(ftl2.read(l, b), Status::Unmapped); continue; }
        REQUIRE_EQ(ftl2.read(l, b), Status::Ok);
        REQUIRE(b == make_page_data(l, ver[l], 64));
    }
    CHECK(ftl2.recovery_report().pages_scanned > 0);
    CHECK(ftl2.recovery_report().stale_pages > 0);
}

TEST(recovery, writes_continue_correctly_after_remount) {
    FlashModel flash(FlashGeometry{64, 16, 64});
    FtlConfig cfg;
    { Ftl f(flash, cfg); f.mount(); for (Lpn l = 0; l < 300; ++l) f.write(l, make_page_data(l, 1, 64)); }
    Ftl ftl(flash, cfg);
    REQUIRE_EQ(ftl.mount(), Status::Ok);
    Rng rng(3);
    for (int i = 0; i < 20000; ++i) { Lpn l = Lpn(rng.below(300)); REQUIRE_EQ(ftl.write(l, make_page_data(l, 2, 64)), Status::Ok); }
    CHECK(ftl.check_invariants().empty());
    std::vector<uint8_t> b;
    REQUIRE_EQ(ftl.read(7, b), Status::Ok);
    CHECK(b == make_page_data(7, 2, 64));
}

TEST(recovery, torn_write_keeps_previous_version) {
    FlashModel flash(FlashGeometry{64, 16, 64});
    FtlConfig cfg;
    Ftl ftl(flash, cfg);
    REQUIRE_EQ(ftl.mount(), Status::Ok);
    REQUIRE_EQ(ftl.write(10, make_page_data(10, 1, 64)), Status::Ok);
    flash.arm_power_loss(0, TearMode::Torn);
    CHECK_EQ(ftl.write(10, make_page_data(10, 2, 64)), Status::ErrPowerLoss);
    flash.power_cycle();
    Ftl rec(flash, cfg);
    REQUIRE_EQ(rec.mount(), Status::Ok);
    CHECK_EQ(rec.recovery_report().torn_pages, 1u);
    std::vector<uint8_t> b;
    REQUIRE_EQ(rec.read(10, b), Status::Ok);
    CHECK(b == make_page_data(10, 1, 64));
}

TEST(recovery, torn_erase_block_is_reerased_at_mount) {
    FlashModel flash(FlashGeometry{16, 4, 32});
    FtlConfig cfg; cfg.overprovision = 0.5;
    // Torn erase of a block full of stale pages (all LPNs overwritten elsewhere).
    Ftl ftl(flash, cfg);
    REQUIRE_EQ(ftl.mount(), Status::Ok);
    for (Lpn l = 0; l < 4; ++l) REQUIRE_EQ(ftl.write(l, make_page_data(l, 1, 32)), Status::Ok);
    for (Lpn l = 0; l < 4; ++l) REQUIRE_EQ(ftl.write(l, make_page_data(l, 2, 32)), Status::Ok);
    flash.arm_power_loss(0, TearMode::Torn);
    CHECK_EQ(flash.erase_block(0), Status::ErrPowerLoss);  // block 0 held the stale v1 copies
    flash.power_cycle();
    Ftl rec(flash, cfg);
    REQUIRE_EQ(rec.mount(), Status::Ok);
    CHECK(rec.recovery_report().blocks_erased >= 1u);
    CHECK(rec.check_invariants().empty());
    for (Lpn l = 0; l < 4; ++l) {
        std::vector<uint8_t> b;
        REQUIRE_EQ(rec.read(l, b), Status::Ok);
        CHECK(b == make_page_data(l, 2, 32));
    }
}

TEST(recovery, mount_is_idempotent_when_interrupted) {
    FlashModel flash(FlashGeometry{32, 8, 32});
    FtlConfig cfg; cfg.overprovision = 0.25;
    {
        Ftl f(flash, cfg); f.mount();
        for (int r = 0; r < 3; ++r) for (Lpn l = 0; l < 40; ++l) f.write(l, make_page_data(l, r + 1, 32));
    }
    // Interrupt the mount itself at every possible mutation point, then mount again.
    for (uint64_t p = 0; p < 6; ++p) {
        flash.arm_power_loss(p, TearMode::Torn);
        Ftl a(flash, cfg);
        Status s = a.mount();
        flash.power_cycle();
        (void)s;
        Ftl b(flash, cfg);
        REQUIRE_EQ(b.mount(), Status::Ok);
        CHECK(b.check_invariants().empty());
        for (Lpn l = 0; l < 40; ++l) {
            std::vector<uint8_t> d;
            REQUIRE_EQ(b.read(l, d), Status::Ok);
            REQUIRE(d == make_page_data(l, 3, 32));
        }
    }
}

TEST(recovery, exhaustive_crash_point_sweep_greedy_static) {
    CrashTrialConfig c = base_cfg(GcPolicy::Greedy, WearMode::Static);
    c.ops = 450;
    CrashSweepResult r = run_crash_sweep(c, 1);
    CHECK_MSG(r.failures == 0, r.first_failure);
    CHECK(r.trials > 800);
    std::cout << "    swept " << r.trials << " crash trials over " << r.total_mutations << " flash mutations\n";
}

TEST(recovery, exhaustive_crash_point_sweep_cost_benefit_dynamic) {
    CrashTrialConfig c = base_cfg(GcPolicy::CostBenefit, WearMode::Dynamic);
    c.ops = 450;
    c.seed = 5;
    CrashSweepResult r = run_crash_sweep(c, 2);
    CHECK_MSG(r.failures == 0, r.first_failure);
    CHECK(r.trials > 400);
}
