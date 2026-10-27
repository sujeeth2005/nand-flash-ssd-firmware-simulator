#pragma once
#include <cstdint>
#include <vector>

#include "nandsim/types.hpp"
#include "nandsim/util.hpp"

namespace nandsim {

struct FlashGeometry {
    uint32_t num_blocks = 128;
    uint32_t pages_per_block = 64;
    uint32_t page_size = 4096;  // bytes of user data per page
    uint64_t total_pages() const { return uint64_t(num_blocks) * pages_per_block; }
};

// Simulated device time per operation (ns). Typical TLC-ish numbers.
struct FlashTiming {
    uint64_t read_ns = 60000;
    uint64_t program_ns = 600000;
    uint64_t erase_ns = 3500000;
};

struct FlashFaultConfig {
    uint32_t max_pe_cycles = 3000;      // erase on a block that already has this many cycles fails -> bad block
    double raw_bit_error_prob = 0.0;    // per page read: P(at least one raw bit error); count is geometric
    uint32_t ecc_correctable_bits = 8;  // more raw errors than this => uncorrectable
    uint64_t seed = 1;
};

// Out-of-band (spare) area written together with each page.
struct Oob {
    uint32_t lpn = kInvalid;
    uint64_t seq = 0;  // global monotonically increasing write sequence number
    uint8_t stream = 0;  // which write stream produced the page (0 = host, 1 = GC); lets mount() resume streams
};

struct FlashStats {
    uint64_t page_reads = 0, page_programs = 0, block_erases = 0;
    uint64_t program_fails = 0, erase_fails = 0;
    uint64_t ecc_corrected_reads = 0, ecc_uncorrectable_reads = 0;
    uint64_t busy_ns = 0;  // simulated device-busy time since last reset_stats()
};

enum class TearMode { Random, Lost, Torn };

// Behavioural NAND model. Enforces the real physical rules:
//   * a page can only be programmed when erased, and pages inside a block must be
//     programmed in ascending order,
//   * erase works on whole blocks only,
//   * blocks wear out after max_pe_cycles and then become bad.
// Also offers fault injection: power loss (torn program / torn erase), program/erase
// failures, factory bad blocks and raw bit errors with an ECC threshold.
class FlashModel {
public:
    explicit FlashModel(FlashGeometry geo, FlashTiming timing = {}, FlashFaultConfig faults = {});

    // data may be nullptr to read only the OOB.
    Status read_page(Ppn ppn, std::vector<uint8_t>* data, Oob* oob);
    Status program_page(Ppn ppn, const uint8_t* data, size_t len, const Oob& oob);
    Status erase_block(BlockId block);

    // ---- inspection (no simulated time; models reading status registers / bad-block marks)
    const FlashGeometry& geometry() const { return geo_; }
    bool is_bad(BlockId b) const { return blocks_[b].bad; }
    bool is_page_programmed(Ppn ppn) const;
    uint32_t erase_count(BlockId b) const { return blocks_[b].erase_count; }
    uint32_t programmed_pages(BlockId b) const { return blocks_[b].next_page; }

    // ---- stats / time
    FlashStats stats() const;
    void reset_stats();
    uint64_t busy_ns() const { return busy_ns_; }           // monotonic, never reset
    uint64_t mutation_count() const { return mutations_; }  // programs + erases executed so far

    // ---- fault injection
    // After `after_n_mutations` more successful program/erase operations, the next one is
    // interrupted by power loss. Lost = nothing happened, Torn = partially executed.
    void arm_power_loss(uint64_t after_n_mutations, TearMode mode = TearMode::Random);
    void disarm_power_loss() { loss_armed_ = false; }
    void power_cycle() { powered_ = true; loss_armed_ = false; }
    bool powered() const { return powered_; }
    void inject_program_fail(BlockId b) { blocks_[b].fail_program = true; }
    void inject_erase_fail(BlockId b) { blocks_[b].fail_erase = true; }
    void mark_factory_bad(BlockId b) { blocks_[b].bad = true; }
    void set_max_pe_cycles(uint32_t n) { fault_.max_pe_cycles = n; }
    void set_raw_bit_error_prob(double p) { fault_.raw_bit_error_prob = p; }
    void set_ecc_correctable_bits(uint32_t n) { fault_.ecc_correctable_bits = n; }

private:
    struct Page {
        bool programmed = false;
        bool torn = false;  // interrupted program/erase: ECC/CRC will never validate
        Oob oob;
        std::vector<uint8_t> data;
    };
    struct Block {
        bool bad = false;
        bool fail_program = false;
        bool fail_erase = false;
        uint32_t erase_count = 0;
        uint32_t next_page = 0;  // next programmable page (sequential-program rule)
        std::vector<Page> pages;
    };

    bool power_loss_now();
    bool tear_is_torn();
    int sample_bit_errors();

    FlashGeometry geo_;
    FlashTiming timing_;
    FlashFaultConfig fault_;
    Rng rng_;
    std::vector<Block> blocks_;
    FlashStats stats_;
    uint64_t busy_ns_ = 0, busy_base_ = 0, mutations_ = 0;
    bool powered_ = true, loss_armed_ = false;
    uint64_t loss_countdown_ = 0;
    TearMode tear_mode_ = TearMode::Random;
};

}  // namespace nandsim
