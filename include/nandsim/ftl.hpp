#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nandsim/flash_model.hpp"
#include "nandsim/types.hpp"

namespace nandsim {

enum class GcPolicy { Greedy, CostBenefit };
enum class WearMode { None, Dynamic, Static };

const char* to_string(GcPolicy p);
const char* to_string(WearMode m);

struct FtlConfig {
    double overprovision = 0.125;     // fraction of physical blocks hidden from the host
    uint32_t gc_low_watermark = 2;    // run GC when free blocks <= this ...
    uint32_t gc_high_watermark = 4;   // ... until free blocks >= this (clamped to what OP allows)
    GcPolicy gc_policy = GcPolicy::Greedy;
    WearMode wear_mode = WearMode::Static;
    uint32_t static_wl_threshold = 20;  // erase-count gap (max - coldest data block) that triggers migration
    uint32_t static_wl_interval = 4;    // evaluate static WL once every N GC invocations
    uint32_t program_retries = 4;       // extra attempts after a program failure
};

struct FtlStats {
    uint64_t host_reads = 0, host_writes = 0, host_trims = 0;  // in pages
    uint64_t unmapped_reads = 0;
    uint64_t gc_runs = 0, gc_pages_moved = 0;
    uint64_t wl_migrations = 0, wl_pages_moved = 0;
    uint64_t program_retries = 0;
    uint64_t bad_blocks_retired = 0;   // blocks that failed erase at runtime
    uint64_t read_errors = 0;
    uint64_t data_loss_pages = 0;      // valid pages that became unreadable during GC
};

struct RecoveryReport {
    uint64_t pages_scanned = 0, valid_pages = 0, stale_pages = 0, torn_pages = 0;
    uint64_t blocks_erased = 0, bad_blocks = 0;
    uint64_t scan_time_ns = 0;  // simulated flash time spent in mount()
};

enum class BlockState : uint8_t { Free, ActiveHost, ActiveGc, Full, Bad };

struct BlockInfo {
    BlockState state;
    uint32_t valid_count;
    uint32_t erase_count;
    uint64_t last_seq;
};

// Page-mapped Flash Translation Layer.
//
//  * L2P table in "DRAM"; per-page OOB holds {lpn, seq} so the table can be rebuilt by a scan.
//  * Two write streams (host data / GC-relocated data) keep hot and cold data apart.
//  * Greedy or cost-benefit garbage collection with free-block watermarks.
//  * Wear levelling: None (lowest block index first), Dynamic (least-worn free block for new
//    writes), Static (dynamic + cold-data migration onto worn blocks).
//  * After an ErrPowerLoss result the instance must be discarded; create a new Ftl on the same
//    FlashModel, call flash.power_cycle() and mount().
//
// Known limitation (documented, as in many real drives): TRIM is not persisted, so after a power
// loss a trimmed LPN may return its previous data.
class Ftl {
public:
    Ftl(FlashModel& flash, FtlConfig cfg = {});  // throws std::invalid_argument on bad geometry/config

    // Scan flash, rebuild all metadata (works on a fresh device and after a crash).
    Status mount();

    Status write(Lpn lpn, const std::vector<uint8_t>& data);
    Status read(Lpn lpn, std::vector<uint8_t>& out);  // Status::Unmapped => out is zero-filled
    Status trim(Lpn lpn);

    // ---- introspection
    uint32_t logical_pages() const { return logical_pages_; }
    uint32_t page_size() const { return geo_.page_size; }
    uint32_t op_blocks() const { return op_blocks_; }
    const FtlConfig& config() const { return cfg_; }
    const FtlStats& stats() const { return stats_; }
    const RecoveryReport& recovery_report() const { return report_; }
    void reset_stats();
    double write_amplification() const;  // flash programs / host page writes (since reset_stats)
    uint64_t last_op_latency_ns() const { return last_latency_ns_; }
    uint32_t free_block_count() const { return uint32_t(free_.size()); }
    BlockInfo block_info(BlockId b) const;
    BlockId host_block() const { return host_.blk; }
    BlockId gc_block() const { return gc_.blk; }
    BlockId peek_victim() const { return select_victim(); }
    bool is_mapped(Lpn lpn) const { return lpn < logical_pages_ && l2p_[lpn] != kInvalid; }
    Ppn physical_of(Lpn lpn) const { return l2p_[lpn]; }

    // Full self-check of all FTL metadata against the flash. Returns "" when consistent.
    std::string check_invariants() const;

private:
    enum class StreamKind { Host, Gc };
    struct Stream { BlockId blk = kInvalid; uint32_t next = 0; };
    struct BlockMeta {
        BlockState state = BlockState::Free;
        uint32_t valid_count = 0;
        uint64_t last_seq = 0;
    };

    Status append(StreamKind kind, Lpn lpn, const uint8_t* data);
    Status open_block(StreamKind kind);
    void close_stream(Stream& st);
    BlockId pick_free(StreamKind kind);
    void commit(Lpn lpn, Ppn ppn, uint64_t seq);
    void invalidate(Ppn ppn);

    Status ensure_free_space();
    Status gc_once();
    BlockId select_victim() const;
    Status relocate_block(BlockId victim, bool for_wear_leveling);
    Status maybe_static_wear_level();

    FlashModel& flash_;
    FlashGeometry geo_;
    FtlConfig cfg_;
    uint32_t ppb_ = 0, op_blocks_ = 0, logical_pages_ = 0;
    uint32_t gc_low_ = 2, gc_high_ = 4;

    std::vector<uint32_t> l2p_, p2l_;
    std::vector<BlockMeta> meta_;
    std::vector<BlockId> free_;
    Stream host_, gc_;
    uint64_t seq_ = 0;
    uint32_t gc_since_wl_ = 0;

    FtlStats stats_;
    RecoveryReport report_;
    uint64_t last_latency_ns_ = 0;
};

}  // namespace nandsim
