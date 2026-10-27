// Garbage collection: victim selection, relocation, free-space management.
#include <algorithm>

#include "nandsim/ftl.hpp"

namespace nandsim {

// Keep the free pool above the low watermark by reclaiming blocks.
Status Ftl::ensure_free_space() {
    if (free_.size() > gc_low_) return Status::Ok;
    uint64_t guard = uint64_t(gc_high_ - std::min<size_t>(gc_high_, free_.size()) + 1) * ppb_ + 8;
    bool ran = false;
    while (free_.size() < gc_high_ && guard--) {
        Status s = gc_once();
        if (s == Status::ErrNoSpace) break;  // nothing reclaimable: degrade gracefully
        if (s != Status::Ok) return s;
        ran = true;
    }
    if (ran) {
        Status s = maybe_static_wear_level();
        if (s != Status::Ok && s != Status::ErrNoSpace) return s;
    }
    return Status::Ok;
}

Status Ftl::gc_once() {
    BlockId victim = select_victim();
    if (victim == kInvalid) return Status::ErrNoSpace;
    ++stats_.gc_runs;
    return relocate_block(victim, false);
}

// Greedy: fewest valid pages (cheapest to move), ties -> least worn.
// Cost-benefit: maximise (1-u)/(2u) * age, i.e. prefer blocks that are mostly invalid AND cold.
// Fully valid blocks are never selected (no space would be gained).
BlockId Ftl::select_victim() const {
    BlockId best = kInvalid;
    double best_score = -1.0;
    // Only consider victims whose valid pages are guaranteed to fit: GC must never get stuck
    // halfway with nowhere to put the data (matters right after a crash at the free-block floor).
    const uint32_t room = (gc_.blk != kInvalid ? ppb_ - gc_.next : 0) + uint32_t(free_.size()) * ppb_;
    for (BlockId b = 0; b < geo_.num_blocks; ++b) {
        const BlockMeta& m = meta_[b];
        if (m.state != BlockState::Full || m.valid_count >= ppb_ || m.valid_count > room) continue;
        double score;
        if (cfg_.gc_policy == GcPolicy::Greedy) {
            score = double(ppb_ - m.valid_count);
        } else if (m.valid_count == 0) {
            score = 1e18;  // free lunch
        } else {
            double u = double(m.valid_count) / ppb_;
            double age = double(seq_ - std::min(seq_, m.last_seq)) + 1.0;
            score = (1.0 - u) / (2.0 * u) * age;
        }
        bool better = score > best_score;
        if (!better && score == best_score && best != kInvalid)
            better = flash_.erase_count(b) < flash_.erase_count(best);
        if (better) { best = b; best_score = score; }
    }
    return best;
}

// Move every valid page of `victim` to the GC stream, then erase it.
Status Ftl::relocate_block(BlockId victim, bool for_wear_leveling) {
    std::vector<uint8_t> buf;
    Oob oob;
    for (uint32_t i = 0; i < ppb_; ++i) {
        Ppn ppn = victim * ppb_ + i;
        Lpn lpn = p2l_[ppn];
        if (lpn == kInvalid) continue;
        Status s = flash_.read_page(ppn, &buf, &oob);
        if (s == Status::ErrPowerLoss) return s;
        if (s != Status::Ok) {  // unrecoverable read during GC: report data loss, drop the mapping
            invalidate(ppn);
            l2p_[lpn] = kInvalid;
            ++stats_.data_loss_pages;
            continue;
        }
        if (oob.lpn != lpn) return Status::ErrCorrupt;
        s = append(StreamKind::Gc, lpn, buf.data());
        if (s != Status::Ok) return s;
        if (for_wear_leveling) ++stats_.wl_pages_moved;
        else ++stats_.gc_pages_moved;
    }
    Status e = flash_.erase_block(victim);
    if (e == Status::Ok) {
        meta_[victim] = BlockMeta{};
        free_.push_back(victim);
    } else if (e == Status::ErrEraseFail) {  // worn out: retire permanently
        meta_[victim].state = BlockState::Bad;
        ++stats_.bad_blocks_retired;
    } else {
        return e;
    }
    return Status::Ok;
}

}  // namespace nandsim
