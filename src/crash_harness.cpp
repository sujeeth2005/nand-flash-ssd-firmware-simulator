#include "nandsim/crash_harness.hpp"

#include <memory>

#include "nandsim/util.hpp"

namespace nandsim {
namespace {

struct Failure {
    std::string msg;
};

class Trial {
public:
    explicit Trial(const CrashTrialConfig& c)
        : cfg_(c), flash_(c.geo, FlashTiming{}, FlashFaultConfig{3000, 0.0, 8, c.seed}), rng_(c.seed * 7919 + 13) {
        ftl_ = std::make_unique<Ftl>(flash_, c.ftl);
        model_.assign(ftl_->logical_pages(), 0);
    }

    CrashTrialResult run() {
        CrashTrialResult res;
        try {
            must(ftl_->mount(), "initial mount");
            schedule_crash();
            const uint32_t L = ftl_->logical_pages();
            // Phase 1: fill the whole logical space once so GC pressure starts immediately.
            for (uint32_t i = 0; i < L && ops_done_ < cfg_.ops; ++i) do_write(i);
            // Phase 2: skewed random overwrites (80% of traffic to 20% of LBAs) + reads.
            while (ops_done_ < cfg_.ops) {
                Lpn lpn = rng_.uniform() < 0.8 ? Lpn(rng_.below(std::max<uint32_t>(1, L / 5)))
                                               : Lpn(rng_.below(L));
                if (rng_.uniform() < cfg_.read_fraction) do_read(lpn);
                else do_write(lpn);
                if (ops_done_ % 64 == 0) check_inv("periodic");
            }
            check_inv("final");
            verify_all(kInvalid, 0);
        } catch (const Failure& f) {
            res.passed = false;
            res.failure = f.msg + " (seed " + std::to_string(cfg_.seed) + ", op " + std::to_string(ops_done_) + ")";
        }
        res.crashes = crashes_;
        res.recovery_crashes = recovery_crashes_;
        res.ops_done = ops_done_;
        res.mutations = flash_.mutation_count();
        return res;
    }

private:
    static void must(Status s, const char* what) {
        if (s != Status::Ok) throw Failure{std::string(what) + " failed: " + to_string(s)};
    }
    void check_inv(const char* when) {
        std::string e = ftl_->check_invariants();
        if (!e.empty()) throw Failure{std::string("invariant violated (") + when + "): " + e};
    }

    void schedule_crash() {
        if (cfg_.fixed_crash_at >= 0) {
            if (!fixed_used_) {
                fixed_used_ = true;
                flash_.arm_power_loss(uint64_t(cfg_.fixed_crash_at), cfg_.tear);
            }
            return;
        }
        if (cfg_.mean_crash_interval == 0) return;
        flash_.arm_power_loss(rng_.below(2ull * cfg_.mean_crash_interval) + 1, cfg_.tear);
    }

    // Observed content of lpn: 0 = unmapped, else the version that matches the data.
    uint32_t observe(Lpn lpn, uint32_t alt_version) {
        std::vector<uint8_t> buf;
        Status s = ftl_->read(lpn, buf);
        if (s == Status::Unmapped) return 0;
        if (s != Status::Ok) throw Failure{"read lpn " + std::to_string(lpn) + " failed: " + to_string(s)};
        if (model_[lpn] != 0 && buf == make_page_data(lpn, model_[lpn], buf.size())) return model_[lpn];
        if (alt_version != 0 && buf == make_page_data(lpn, alt_version, buf.size())) return alt_version;
        throw Failure{"lpn " + std::to_string(lpn) + " returned data matching no acknowledged version"};
    }

    // After recovery: every LPN must equal the model, except `pending` which may be old or new.
    void verify_all(Lpn pending, uint32_t pending_ver) {
        for (Lpn lpn = 0; lpn < model_.size(); ++lpn) {
            if (lpn == pending) {
                model_[lpn] = observe(lpn, pending_ver);  // adopt whichever outcome the device chose
            } else {
                uint32_t got = observe(lpn, 0);
                if (got != model_[lpn])
                    throw Failure{"lpn " + std::to_string(lpn) + " lost/regressed after recovery"};
            }
        }
    }

    void recover(Lpn pending, uint32_t pending_ver) {
        ++crashes_;
        for (int attempt = 0; attempt < 16; ++attempt) {
            flash_.power_cycle();
            ftl_ = std::make_unique<Ftl>(flash_, cfg_.ftl);
            if (cfg_.crash_during_recovery && cfg_.fixed_crash_at < 0 && cfg_.mean_crash_interval > 0 &&
                attempt < 2 && rng_.uniform() < 0.25)
                flash_.arm_power_loss(rng_.below(6), cfg_.tear);  // nested crash while mounting
            Status s = ftl_->mount();
            if (s == Status::ErrPowerLoss) { ++recovery_crashes_; continue; }
            must(s, "mount after crash");
            flash_.disarm_power_loss();
            check_inv("after recovery");
            verify_all(pending, pending_ver);
            schedule_crash();
            return;
        }
        throw Failure{"recovery kept failing"};
    }

    std::string describe() const {
        std::string d = "free=" + std::to_string(ftl_->free_block_count()) + " blocks:";
        for (BlockId b = 0; b < cfg_.geo.num_blocks; ++b) {
            BlockInfo bi = ftl_->block_info(b);
            const char* st[] = {"F", "H", "G", "X", "B"};
            d += std::string(" ") + st[int(bi.state)] + std::to_string(bi.valid_count);
        }
        return d;
    }

    void do_write(Lpn lpn) {
        uint32_t ver = ++next_ver_;
        Status s = ftl_->write(lpn, make_page_data(lpn, ver, cfg_.geo.page_size));
        ++ops_done_;
        if (s == Status::Ok) model_[lpn] = ver;
        else if (s == Status::ErrPowerLoss) recover(lpn, ver);
        else throw Failure{"write lpn " + std::to_string(lpn) + " failed: " + to_string(s) + " [" + describe() + "]"};
    }

    void do_read(Lpn lpn) {
        std::vector<uint8_t> buf;
        Status s = ftl_->read(lpn, buf);
        ++ops_done_;
        if (s == Status::ErrPowerLoss) { recover(kInvalid, 0); return; }
        if (s == Status::Unmapped) {
            if (model_[lpn] != 0) throw Failure{"lpn " + std::to_string(lpn) + " unexpectedly unmapped"};
        } else if (s != Status::Ok || buf != make_page_data(lpn, model_[lpn], buf.size())) {
            throw Failure{"read verify failed lpn " + std::to_string(lpn)};
        }
    }

    CrashTrialConfig cfg_;
    FlashModel flash_;
    Rng rng_;
    std::unique_ptr<Ftl> ftl_;
    std::vector<uint32_t> model_;
    uint32_t next_ver_ = 0, ops_done_ = 0, crashes_ = 0, recovery_crashes_ = 0;
    bool fixed_used_ = false;
};

}  // namespace

CrashTrialResult run_crash_trial(const CrashTrialConfig& cfg) { return Trial(cfg).run(); }

CrashSweepResult run_crash_sweep(const CrashTrialConfig& base, uint32_t stride) {
    CrashSweepResult out;
    CrashTrialConfig ref = base;
    ref.fixed_crash_at = -1;
    ref.mean_crash_interval = 0;
    CrashTrialResult r0 = run_crash_trial(ref);
    if (!r0.passed) {
        out.failures = 1;
        out.first_failure = "reference run failed: " + r0.failure;
        return out;
    }
    out.total_mutations = r0.mutations;
    for (TearMode mode : {TearMode::Lost, TearMode::Torn}) {
        for (uint64_t p = 0; p < r0.mutations; p += std::max<uint32_t>(1, stride)) {
            CrashTrialConfig c = base;
            c.fixed_crash_at = int64_t(p);
            c.tear = mode;
            CrashTrialResult r = run_crash_trial(c);
            ++out.trials;
            if (!r.passed) {
                ++out.failures;
                if (out.first_failure.empty())
                    out.first_failure = "crash point " + std::to_string(p) + (mode == TearMode::Torn ? " (torn): " : " (lost): ") + r.failure;
            }
        }
    }
    return out;
}

}  // namespace nandsim
