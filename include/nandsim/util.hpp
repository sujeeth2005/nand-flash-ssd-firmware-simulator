#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace nandsim {

// Small, fully deterministic PRNG (identical sequence on every compiler/OS, unlike
// std::*_distribution), so every test failure is reproducible from its seed.
class Rng {
public:
    explicit Rng(uint64_t seed = 1) : s_(seed) {}
    uint64_t next() {
        uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint64_t below(uint64_t n) { return n ? next() % n : 0; }
    double uniform() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
private:
    uint64_t s_;
};

// Deterministic page payload for (lpn, version). Used by tests/benchmarks as the
// ground-truth oracle: whatever the FTL returns must equal this pattern.
inline std::vector<uint8_t> make_page_data(uint32_t lpn, uint32_t version, size_t size) {
    std::vector<uint8_t> v(size);
    Rng g((uint64_t(lpn) << 32) ^ uint64_t(version) ^ 0xD1B54A32D192ED03ull);
    size_t i = 0;
    while (i < size) {
        uint64_t x = g.next();
        for (int k = 0; k < 8 && i < size; ++k, ++i) v[i] = uint8_t(x >> (8 * k));
    }
    return v;
}

// p in [0,100]; input need not be sorted. Nearest-rank method.
inline uint64_t percentile(std::vector<uint64_t> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    double rank = p / 100.0 * double(v.size());
    size_t idx = rank <= 1.0 ? 0 : size_t(rank + 0.999999999) - 1;
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

}  // namespace nandsim
