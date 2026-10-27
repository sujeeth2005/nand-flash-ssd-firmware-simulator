#include <memory>

#include "nandsim/crash_harness.hpp"
#include "nandsim/ftl.hpp"
#include "nandsim/util.hpp"
#include "test_framework.hpp"

using namespace nandsim;

namespace {
// Random write/read/trim mix against a reference model, invariants checked throughout.
void fuzz(GcPolicy gc, WearMode wl, uint64_t seed) {
    FlashModel flash(FlashGeometry{48, 16, 32});
    FtlConfig cfg; cfg.gc_policy = gc; cfg.wear_mode = wl; cfg.overprovision = 0.2;
    cfg.static_wl_threshold = 4; cfg.static_wl_interval = 1;
    Ftl ftl(flash, cfg);
    REQUIRE_EQ(ftl.mount(), Status::Ok);
    uint32_t L = ftl.logical_pages();
    std::vector<uint32_t> ver(L, 0);
    Rng rng(seed);
    uint32_t v = 0;
    for (int i = 0; i < 40000; ++i) {
        Lpn l = Lpn(rng.uniform() < 0.7 ? rng.below(L / 8) : rng.below(L));
        double r = rng.uniform();
        std::vector<uint8_t> b;
        if (r < 0.65) {
            REQUIRE_EQ(ftl.write(l, make_page_data(l, ++v, 32)), Status::Ok);
            ver[l] = v;
        } else if (r < 0.9) {
            Status s = ftl.read(l, b);
            if (ver[l] == 0) REQUIRE_EQ(s, Status::Unmapped);
            else { REQUIRE_EQ(s, Status::Ok); REQUIRE(b == make_page_data(l, ver[l], 32)); }
        } else {
            REQUIRE_EQ(ftl.trim(l), Status::Ok);
            ver[l] = 0;
        }
        if (i % 500 == 0) { std::string e = ftl.check_invariants(); CHECK_MSG(e.empty(), e); if (!e.empty()) throw ::nt::Abort(); }
    }
    std::string e = ftl.check_invariants();
    CHECK_MSG(e.empty(), e);
}
}  // namespace

TEST(fuzz, greedy_none) { fuzz(GcPolicy::Greedy, WearMode::None, 1); }
TEST(fuzz, greedy_dynamic) { fuzz(GcPolicy::Greedy, WearMode::Dynamic, 2); }
TEST(fuzz, greedy_static) { fuzz(GcPolicy::Greedy, WearMode::Static, 3); }
TEST(fuzz, cost_benefit_none) { fuzz(GcPolicy::CostBenefit, WearMode::None, 4); }
TEST(fuzz, cost_benefit_dynamic) { fuzz(GcPolicy::CostBenefit, WearMode::Dynamic, 5); }
TEST(fuzz, cost_benefit_static) { fuzz(GcPolicy::CostBenefit, WearMode::Static, 6); }

TEST(fuzz, random_power_loss_many_seeds) {
    uint32_t crashes = 0;
    for (uint64_t seed = 1; seed <= 40; ++seed) {
        CrashTrialConfig c;
        c.ftl.overprovision = 0.25;
        c.ftl.gc_policy = (seed % 2) ? GcPolicy::Greedy : GcPolicy::CostBenefit;
        c.ftl.wear_mode = WearMode(seed % 3);
        c.ftl.static_wl_threshold = 3;
        c.ftl.static_wl_interval = 1;
        c.seed = seed;
        c.ops = 1500;
        CrashTrialResult r = run_crash_trial(c);
        CHECK_MSG(r.passed, r.failure);
        crashes += r.crashes;
    }
    CHECK(crashes > 500);
    std::cout << "    survived " << crashes << " random power losses\n";
}
