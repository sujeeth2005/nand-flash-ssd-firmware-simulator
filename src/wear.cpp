// Static wear levelling: park cold data on worn blocks so low-wear blocks re-enter circulation.
// (Dynamic wear levelling = least-worn-first allocation, see Ftl::pick_free.)
#include <algorithm>
#include <limits>

#include "nandsim/ftl.hpp"

namespace nandsim {

Status Ftl::maybe_static_wear_level() {
    if (cfg_.wear_mode != WearMode::Static) return Status::Ok;
    if (++gc_since_wl_ < cfg_.static_wl_interval) return Status::Ok;
    gc_since_wl_ = 0;
    if (free_.size() < 2) return Status::Ok;

    uint32_t max_e = 0, min_e = std::numeric_limits<uint32_t>::max();
    BlockId cold = kInvalid;
    for (BlockId b = 0; b < geo_.num_blocks; ++b) {
        if (meta_[b].state == BlockState::Bad) continue;
        uint32_t e = flash_.erase_count(b);
        max_e = std::max(max_e, e);
        // Only *cold* data qualifies: not rewritten during the last logical-capacity worth of
        // writes. Otherwise a freshly recycled low-wear block that now holds hot data would be
        // migrated pointlessly (wear-levelling thrash).
        bool cold_data = (seq_ - std::min(seq_, meta_[b].last_seq)) >= logical_pages_;
        if (meta_[b].state == BlockState::Full && cold_data && e < min_e) { min_e = e; cold = b; }
    }
    if (cold == kInvalid || max_e - min_e < cfg_.static_wl_threshold) return Status::Ok;
    ++stats_.wl_migrations;
    // Give the migrated cold data its own block(s): mixing it with GC remnants of hot blocks
    // would make GC drag the same cold pages around again and again (write-amplification blow-up).
    if (gc_.blk != kInvalid) close_stream(gc_);
    Status s = relocate_block(cold, true);
    if (s == Status::Ok && gc_.blk != kInvalid) close_stream(gc_);
    return s;
}

}  // namespace nandsim
