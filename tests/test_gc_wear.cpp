#include <algorithm>

#include "nandsim/ftl.hpp"
#include "nandsim/util.hpp"
#include "test_framework.hpp"

using namespace nandsim;

namespace {
struct Rig {
    FlashModel flash;
    Ftl ftl;
    std::vector<uint32_t> ver;
    uint32_t next = 0;
    Rig(FtlConfig cfg, FlashGeometry g = {64, 16, 32}) : flash(g), ftl(flash, cfg) {
        ftl.mount();
        ver.assign(ftl.logical_pages(), 0);
    }
    Status write(Lpn l) {
        Status s = ftl.write(l, make_page_data(l, ++next, ftl.page_size()));
        if (s == Status::Ok) ver[l] = next;
        return s;
    }
    bool verify_all() {
        for (Lpn l = 0; l < ver.size(); ++l) {
            std::vector<uint8_t> b;
            Status s = ftl.read(l, b);
            if (ver[l] == 0) { if (s != Status::Unmapped) return false; continue; }
            if (s != Status::Ok || b != make_page_data(l, ver[l], ftl.page_size())) return false;
        }
        return true;
    }
    uint32_t wear_spread() const {
        uint32_t mn = ~0u, mx = 0;
        for (BlockId b = 0; b < 64; ++b) { mn = std::min(mn, flash.erase_count(b)); mx = std::max(mx, flash.erase_count(b)); }
        return mx - mn;
    }
};

// 90% of LBAs written once (cold), a hot 10% overwritten many times.
void hot_cold(Rig& r, int hot_writes, uint64_t seed) {
    uint32_t L = r.ftl.logical_pages();
    for (Lpn l = 0; l < L; ++l) r.write(l);
    Rng rng(seed);
    uint32_t hot = L / 10;
    for (int i = 0; i < hot_writes; ++i) r.write(Lpn(rng.below(hot)));
}
}  // namespace

TEST(gc, reclaims_space_and_preserves_data_greedy) {
    FtlConfig cfg; cfg.gc_policy = GcPolicy::Greedy;
    Rig r(cfg);
    uint32_t L = r.ftl.logical_pages();
    Rng rng(1);
    for (Lpn l = 0; l < L; ++l) REQUIRE_EQ(r.write(l), Status::Ok);
    for (int i = 0; i < 30000; ++i) REQUIRE_EQ(r.write(Lpn(rng.below(L))), Status::Ok);
    CHECK(r.ftl.stats().gc_runs > 0);
    CHECK(r.ftl.stats().gc_pages_moved > 0);
    CHECK(r.verify_all());
    CHECK(r.ftl.check_invariants().empty());
    CHECK(r.ftl.free_block_count() >= 1u);
}

TEST(gc, reclaims_space_and_preserves_data_cost_benefit) {
    FtlConfig cfg; cfg.gc_policy = GcPolicy::CostBenefit;
    Rig r(cfg);
    uint32_t L = r.ftl.logical_pages();
    Rng rng(2);
    for (Lpn l = 0; l < L; ++l) REQUIRE_EQ(r.write(l), Status::Ok);
    for (int i = 0; i < 30000; ++i) REQUIRE_EQ(r.write(Lpn(rng.below(L))), Status::Ok);
    CHECK(r.ftl.stats().gc_runs > 0);
    CHECK(r.verify_all());
    CHECK(r.ftl.check_invariants().empty());
}

TEST(gc, greedy_picks_block_with_fewest_valid_pages) {
    FtlConfig cfg; cfg.gc_policy = GcPolicy::Greedy;
    Rig r(cfg);
    uint32_t L = r.ftl.logical_pages();
    Rng rng(3);
    for (Lpn l = 0; l < L; ++l) r.write(l);
    for (int i = 0; i < 3000; ++i) r.write(Lpn(rng.below(L)));
    BlockId v = r.ftl.peek_victim();
    REQUIRE(v != kInvalid);
    uint32_t vv = r.ftl.block_info(v).valid_count;
    for (BlockId b = 0; b < 64; ++b) {
        BlockInfo bi = r.ftl.block_info(b);
        if (bi.state == BlockState::Full) CHECK(bi.valid_count >= vv);
    }
}

TEST(gc, cost_benefit_prefers_cold_block_over_equally_empty_hot_block) {
    // Same utilisation, different age: cost-benefit must pick the OLDER block.
    FtlConfig cfg; cfg.gc_policy = GcPolicy::CostBenefit; cfg.gc_low_watermark = 2;
    Rig r(cfg);
    uint32_t ppb = 16;
    // Block A: written first, then half of it invalidated much later (stays old).
    for (Lpn l = 0; l < ppb; ++l) r.write(l);
    // Block B: written later.
    for (Lpn l = ppb; l < 2 * ppb; ++l) r.write(l);
    // Invalidate the first 8 pages of both blocks by trimming (same utilisation, 50%).
    for (Lpn l = 0; l < 8; ++l) r.ftl.trim(l);
    for (Lpn l = ppb; l < ppb + 8; ++l) r.ftl.trim(l);
    BlockId a = r.ftl.physical_of(8) / ppb, b = r.ftl.physical_of(ppb + 8) / ppb;
    REQUIRE(a != b);
    CHECK_EQ(r.ftl.peek_victim(), a);  // older (cold) block wins on equal utilisation
}

TEST(gc, fully_valid_blocks_are_never_victims) {
    Rig r(FtlConfig{});
    for (Lpn l = 0; l < 4 * 16; ++l) r.write(l);
    CHECK_EQ(r.ftl.peek_victim(), kInvalid);
}

TEST(gc, write_amplification_is_greater_than_one_under_random_overwrites) {
    Rig r(FtlConfig{});
    uint32_t L = r.ftl.logical_pages();
    Rng rng(4);
    for (Lpn l = 0; l < L; ++l) r.write(l);
    r.ftl.reset_stats();
    for (int i = 0; i < 30000; ++i) r.write(Lpn(rng.below(L)));
    double wa = r.ftl.write_amplification();
    CHECK(wa > 1.2);
    CHECK(wa < 12.0);
}

TEST(gc, more_overprovisioning_lowers_write_amplification) {
    auto run = [](double op) {
        FtlConfig cfg; cfg.overprovision = op;
        Rig r(cfg, FlashGeometry{128, 16, 32});
        uint32_t L = r.ftl.logical_pages();
        Rng rng(6);
        for (Lpn l = 0; l < L; ++l) r.write(l);
        r.ftl.reset_stats();
        for (int i = 0; i < 40000; ++i) r.write(Lpn(rng.below(L)));
        return r.ftl.write_amplification();
    };
    double lo = run(0.10), hi = run(0.30);
    CHECK_MSG(hi < lo, "WA(op=30%)=" << hi << " should be < WA(op=10%)=" << lo);
}

TEST(gc, cost_benefit_beats_greedy_on_hot_cold_workload) {
    auto run = [](GcPolicy p) {
        FtlConfig cfg; cfg.gc_policy = p; cfg.wear_mode = WearMode::Dynamic;
        Rig r(cfg, FlashGeometry{128, 16, 32});
        uint32_t L = r.ftl.logical_pages();
        for (Lpn l = 0; l < L; ++l) r.write(l);
        Rng rng(7);
        r.ftl.reset_stats();
        uint32_t hot = L / 10;
        for (int i = 0; i < 60000; ++i) r.write(Lpn(rng.uniform() < 0.9 ? rng.below(hot) : hot + rng.below(L - hot)));
        return r.ftl.write_amplification();
    };
    double g = run(GcPolicy::Greedy), cb = run(GcPolicy::CostBenefit);
    CHECK_MSG(cb <= g * 1.02, "cost-benefit WA " << cb << " vs greedy " << g);
}

TEST(wear, dynamic_allocation_uses_least_worn_free_block) {
    FtlConfig cfg; cfg.wear_mode = WearMode::Dynamic;
    Rig r(cfg);
    // Pre-age blocks 0..9 so every other block is less worn.
    for (BlockId b = 0; b < 10; ++b) { r.flash.erase_block(b); r.flash.erase_block(b); }
    // Rig has already mounted, so rebuild FTL state from flash.
    Ftl ftl(r.flash, cfg);
    REQUIRE_EQ(ftl.mount(), Status::Ok);
    REQUIRE_EQ(ftl.write(0, make_page_data(0, 1, 32)), Status::Ok);
    CHECK(ftl.host_block() >= 10u);
}

TEST(wear, none_mode_allocates_lowest_index_first) {
    FtlConfig cfg; cfg.wear_mode = WearMode::None;
    Rig r(cfg);
    REQUIRE_EQ(r.write(0), Status::Ok);
    CHECK_EQ(r.ftl.host_block(), 0u);
}

TEST(wear, dynamic_and_static_reduce_wear_spread_vs_none) {
    auto spread = [](WearMode m) {
        FtlConfig cfg; cfg.wear_mode = m; cfg.static_wl_threshold = 10;
        Rig r(cfg);
        hot_cold(r, 150000, 11);
        return r.wear_spread();
    };
    uint32_t none = spread(WearMode::None), dyn = spread(WearMode::Dynamic), stat = spread(WearMode::Static);
    CHECK_MSG(dyn < none, "dynamic " << dyn << " vs none " << none);
    CHECK_MSG(stat < dyn, "static " << stat << " vs dynamic " << dyn);
}

TEST(wear, static_leveling_bounds_spread_and_keeps_data) {
    FtlConfig cfg; cfg.wear_mode = WearMode::Static; cfg.static_wl_threshold = 10;
    Rig r(cfg);
    hot_cold(r, 150000, 12);
    CHECK(r.ftl.stats().wl_migrations > 0);
    CHECK_MSG(r.wear_spread() <= 40, "spread " << r.wear_spread());
    CHECK(r.verify_all());
    CHECK(r.ftl.check_invariants().empty());
}

TEST(wear, static_leveling_costs_extra_writes_but_not_unbounded) {
    auto wa = [](WearMode m) {
        FtlConfig cfg; cfg.wear_mode = m; cfg.static_wl_threshold = 10;
        Rig r(cfg);
        hot_cold(r, 100000, 13);
        return r.ftl.write_amplification();
    };
    double dyn = wa(WearMode::Dynamic), stat = wa(WearMode::Static);
    CHECK_MSG(stat < dyn * 1.5, "static WA " << stat << " vs dynamic " << dyn);
}
