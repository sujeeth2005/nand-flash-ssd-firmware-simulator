#pragma once
#include <cstdint>
#include <string>

#include "nandsim/ftl.hpp"

namespace nandsim {

// Randomised power-loss testing. A reference model of "what the host was promised" is kept next
// to the device; power is cut at (pseudo)random flash operations (mid-program, mid-erase,
// mid-GC, even during recovery itself). After each crash the FTL is rebuilt with mount() and:
//   * every acknowledged write must still be readable with exactly its data,
//   * the one in-flight write may be either old or new data (never garbage),
//   * all FTL invariants must hold.
struct CrashTrialConfig {
    FlashGeometry geo{32, 16, 32};
    FtlConfig ftl;
    uint64_t seed = 1;
    uint32_t ops = 2000;
    int64_t fixed_crash_at = -1;         // >=0: exactly one crash after this many flash mutations
    TearMode tear = TearMode::Random;
    uint32_t mean_crash_interval = 40;   // random mode (fixed_crash_at < 0): mean #mutations between crashes; 0 = never
    bool crash_during_recovery = true;
    double read_fraction = 0.15;
};

struct CrashTrialResult {
    bool passed = true;
    std::string failure;
    uint32_t crashes = 0, recovery_crashes = 0;
    uint32_t ops_done = 0;
    uint64_t mutations = 0;
};

CrashTrialResult run_crash_trial(const CrashTrialConfig& cfg);

struct CrashSweepResult {
    uint64_t trials = 0, failures = 0;
    uint64_t total_mutations = 0;  // flash mutations in the crash-free reference run
    std::string first_failure;
};

// Exhaustive sweep: re-run the same scenario once per flash mutation index (and tear mode),
// cutting power at exactly that operation. stride > 1 samples every stride-th point.
CrashSweepResult run_crash_sweep(const CrashTrialConfig& base, uint32_t stride = 1);

}  // namespace nandsim
