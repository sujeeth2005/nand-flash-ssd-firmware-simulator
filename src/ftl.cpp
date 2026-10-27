// Core of the FTL: mapping, host I/O path, block allocation, mount/recovery, invariants.
#include "nandsim/ftl.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace nandsim {

const char* to_string(GcPolicy p) { return p == GcPolicy::Greedy ? "greedy" : "cost-benefit"; }
const char* to_string(WearMode m) {
    switch (m) {
        case WearMode::None: return "none";
        case WearMode::Dynamic: return "dynamic";
        case WearMode::Static: return "static";
    }
    return "?";
}

namespace {
constexpr uint32_t kMinOpBlocks = 6;    // 2 reserved + 2 active streams + 2 slack
// The host stream may never take the last kHostReserve free blocks: GC needs a spare destination
// block to make progress, and a crash in the middle of GC must still leave one at mount time.
constexpr uint32_t kHostReserve = 2;
}

Ftl::Ftl(FlashModel& flash, FtlConfig cfg) : flash_(flash), geo_(flash.geometry()), cfg_(cfg) {
    ppb_ = geo_.pages_per_block;
    if (ppb_ < 2 || geo_.page_size < 16) throw std::invalid_argument("geometry too small");
    if (cfg.overprovision < 0.0 || cfg.overprovision >= 1.0) throw std::invalid_argument("bad overprovision");
    op_blocks_ = std::max<uint32_t>(kMinOpBlocks, uint32_t(std::ceil(geo_.num_blocks * cfg.overprovision)));
    if (op_blocks_ + 4 > geo_.num_blocks) throw std::invalid_argument("not enough blocks for requested OP");
    logical_pages_ = (geo_.num_blocks - op_blocks_) * ppb_;

    gc_low_ = std::max<uint32_t>(2, cfg.gc_low_watermark);
    uint32_t hi_cap = std::max<uint32_t>(gc_low_ + 1, op_blocks_ / 2);
    gc_high_ = std::min(std::max(cfg.gc_high_watermark, gc_low_ + 1), hi_cap);

    l2p_.assign(logical_pages_, kInvalid);
    p2l_.assign(geo_.total_pages(), kInvalid);
    meta_.assign(geo_.num_blocks, BlockMeta{});
}

void Ftl::reset_stats() {
    stats_ = FtlStats{};
    flash_.reset_stats();
}

double Ftl::write_amplification() const {
    if (stats_.host_writes == 0) return 0.0;
    return double(flash_.stats().page_programs) / double(stats_.host_writes);
}

BlockInfo Ftl::block_info(BlockId b) const {
    return BlockInfo{meta_[b].state, meta_[b].valid_count, flash_.erase_count(b), meta_[b].last_seq};
}

// ------------------------------------------------------------------ mount / recovery
Status Ftl::mount() {
    l2p_.assign(logical_pages_, kInvalid);
    p2l_.assign(geo_.total_pages(), kInvalid);
    meta_.assign(geo_.num_blocks, BlockMeta{});
    free_.clear();
    host_ = Stream{};
    gc_ = Stream{};
    seq_ = 0;
    gc_since_wl_ = 0;
    report_ = RecoveryReport{};

    const uint64_t t0 = flash_.busy_ns();
    std::vector<uint64_t> best_seq(logical_pages_, 0);  // seq numbers start at 1
    std::vector<uint32_t> programmed(geo_.num_blocks, 0);
    std::vector<uint8_t> last_stream(geo_.num_blocks, 0);

    // 1) Scan every programmed page's OOB; the highest sequence number per LPN wins.
    for (BlockId b = 0; b < geo_.num_blocks; ++b) {
        if (flash_.is_bad(b)) {
            meta_[b].state = BlockState::Bad;
            ++report_.bad_blocks;
            continue;
        }
        for (uint32_t i = 0; i < ppb_; ++i) {
            Ppn ppn = b * ppb_ + i;
            if (!flash_.is_page_programmed(ppn)) break;  // programming is sequential
            ++programmed[b];
            Oob oob;
            Status s = flash_.read_page(ppn, nullptr, &oob);
            ++report_.pages_scanned;
            if (s == Status::ErrPowerLoss) return s;
            if (s != Status::Ok || oob.lpn >= logical_pages_) {  // torn / unreadable / garbage OOB
                ++report_.torn_pages;
                continue;
            }
            meta_[b].last_seq = std::max(meta_[b].last_seq, oob.seq);
            last_stream[b] = oob.stream;
            seq_ = std::max(seq_, oob.seq);
            if (oob.seq > best_seq[oob.lpn]) {
                if (l2p_[oob.lpn] != kInvalid) ++report_.stale_pages;
                best_seq[oob.lpn] = oob.seq;
                l2p_[oob.lpn] = ppn;
            } else {
                ++report_.stale_pages;
            }
        }
    }

    // 2) Derive reverse map and per-block valid counts.
    for (Lpn lpn = 0; lpn < logical_pages_; ++lpn) {
        Ppn ppn = l2p_[lpn];
        if (ppn == kInvalid) continue;
        p2l_[ppn] = lpn;
        ++meta_[ppn / ppb_].valid_count;
        ++report_.valid_pages;
    }

    // 3) Classify blocks. Blocks with data but nothing valid (stale or torn) are erased now.
    //    Up to two partially filled blocks (the write streams that were open at power loss) are
    //    re-adopted as the active host/GC streams: closing them would leak their free tails and
    //    could leave the device with zero free blocks, unable to start GC.
    std::vector<BlockId> partial;
    for (BlockId b = 0; b < geo_.num_blocks; ++b) {
        if (meta_[b].state == BlockState::Bad) continue;
        if (programmed[b] == 0) {
            meta_[b].state = BlockState::Free;
            free_.push_back(b);
        } else if (meta_[b].valid_count == 0) {
            Status s = flash_.erase_block(b);
            if (s == Status::Ok) {
                meta_[b] = BlockMeta{};
                free_.push_back(b);
                ++report_.blocks_erased;
            } else if (s == Status::ErrEraseFail) {
                meta_[b].state = BlockState::Bad;
                ++report_.bad_blocks;
            } else {
                return s;
            }
        } else {
            meta_[b].state = BlockState::Full;
            if (programmed[b] < ppb_) partial.push_back(b);
        }
    }
    // Resume the newest partial block of each stream kind; any other partial block stays Full.
    for (uint8_t kind = 0; kind < 2; ++kind) {
        BlockId pick = kInvalid;
        for (BlockId b : partial)
            if (last_stream[b] == kind && meta_[b].state == BlockState::Full &&
                (pick == kInvalid || meta_[b].last_seq > meta_[pick].last_seq))
                pick = b;
        if (pick == kInvalid) continue;
        (kind == 0 ? host_ : gc_) = Stream{pick, programmed[pick]};
        meta_[pick].state = kind == 0 ? BlockState::ActiveHost : BlockState::ActiveGc;
    }
    report_.scan_time_ns = flash_.busy_ns() - t0;

    uint32_t usable = 0;
    for (BlockId b = 0; b < geo_.num_blocks; ++b) usable += meta_[b].state != BlockState::Bad;
    if (usable < (logical_pages_ / ppb_) + 2) return Status::ErrNoSpace;
    return Status::Ok;
}

// ------------------------------------------------------------------ host I/O
Status Ftl::write(Lpn lpn, const std::vector<uint8_t>& data) {
    if (lpn >= logical_pages_) return Status::ErrInvalidAddress;
    if (data.size() != geo_.page_size) return Status::ErrInvalidArgument;
    const uint64_t t0 = flash_.busy_ns();
    Status s = append(StreamKind::Host, lpn, data.data());
    last_latency_ns_ = flash_.busy_ns() - t0;
    if (s == Status::Ok) ++stats_.host_writes;
    return s;
}

Status Ftl::read(Lpn lpn, std::vector<uint8_t>& out) {
    if (lpn >= logical_pages_) return Status::ErrInvalidAddress;
    const uint64_t t0 = flash_.busy_ns();
    ++stats_.host_reads;
    Ppn ppn = l2p_[lpn];
    if (ppn == kInvalid) {
        out.assign(geo_.page_size, 0);
        ++stats_.unmapped_reads;
        last_latency_ns_ = 0;
        return Status::Unmapped;
    }
    Oob oob;
    Status s = flash_.read_page(ppn, &out, &oob);
    last_latency_ns_ = flash_.busy_ns() - t0;
    if (s != Status::Ok) {
        if (s != Status::ErrPowerLoss) ++stats_.read_errors;
        return s;
    }
    if (oob.lpn != lpn) {  // mapping and flash metadata disagree: firmware bug or corruption
        ++stats_.read_errors;
        return Status::ErrCorrupt;
    }
    return Status::Ok;
}

Status Ftl::trim(Lpn lpn) {
    if (lpn >= logical_pages_) return Status::ErrInvalidAddress;
    if (l2p_[lpn] != kInvalid) {
        invalidate(l2p_[lpn]);
        l2p_[lpn] = kInvalid;
    }
    ++stats_.host_trims;
    return Status::Ok;
}

// ------------------------------------------------------------------ write path internals
void Ftl::invalidate(Ppn ppn) {
    p2l_[ppn] = kInvalid;
    --meta_[ppn / ppb_].valid_count;
}

void Ftl::commit(Lpn lpn, Ppn ppn, uint64_t seq) {
    if (l2p_[lpn] != kInvalid) invalidate(l2p_[lpn]);
    l2p_[lpn] = ppn;
    p2l_[ppn] = lpn;
    BlockMeta& m = meta_[ppn / ppb_];
    ++m.valid_count;
    m.last_seq = seq;
}

void Ftl::close_stream(Stream& st) {
    meta_[st.blk].state = BlockState::Full;
    st = Stream{};
}

Status Ftl::append(StreamKind kind, Lpn lpn, const uint8_t* data) {
    Stream& st = (kind == StreamKind::Host) ? host_ : gc_;
    for (uint32_t attempt = 0; attempt <= cfg_.program_retries; ++attempt) {
        if (st.blk == kInvalid) {
            Status s = open_block(kind);
            if (s != Status::Ok) return s;
        }
        Ppn ppn = st.blk * ppb_ + st.next;
        uint64_t seq = ++seq_;
        Status s = flash_.program_page(ppn, data, geo_.page_size,
                                       Oob{lpn, seq, uint8_t(kind == StreamKind::Gc ? 1 : 0)});
        if (s == Status::Ok) {
            ++st.next;
            commit(lpn, ppn, seq);
            if (st.next == ppb_) close_stream(st);
            return Status::Ok;
        }
        if (s == Status::ErrProgramFail) {  // page burnt; retire the rest of this block, retry elsewhere
            ++stats_.program_retries;
            close_stream(st);
            continue;
        }
        return s;
    }
    return Status::ErrProgramFail;
}

Status Ftl::open_block(StreamKind kind) {
    if (kind == StreamKind::Host) {
        Status s = ensure_free_space();
        if (s != Status::Ok && s != Status::ErrNoSpace) return s;
    }
    if (kind == StreamKind::Host && free_.size() <= kHostReserve) return Status::ErrNoSpace;
    BlockId b = pick_free(kind);
    if (b == kInvalid) return Status::ErrNoSpace;
    Stream& st = (kind == StreamKind::Host) ? host_ : gc_;
    st.blk = b;
    st.next = 0;
    meta_[b].state = (kind == StreamKind::Host) ? BlockState::ActiveHost : BlockState::ActiveGc;
    return Status::Ok;
}

// Allocation policy lives here (see WearMode).
BlockId Ftl::pick_free(StreamKind kind) {
    if (free_.empty()) return kInvalid;
    const bool want_max = (cfg_.wear_mode == WearMode::Static && kind == StreamKind::Gc);
    size_t best = 0;
    for (size_t i = 1; i < free_.size(); ++i) {
        BlockId a = free_[i], c = free_[best];
        bool better;
        if (cfg_.wear_mode == WearMode::None) {
            better = a < c;
        } else {
            uint32_t ea = flash_.erase_count(a), ec = flash_.erase_count(c);
            if (ea == ec) better = a < c;
            else better = want_max ? (ea > ec) : (ea < ec);
        }
        if (better) best = i;
    }
    BlockId b = free_[best];
    free_[best] = free_.back();
    free_.pop_back();
    return b;
}

// ------------------------------------------------------------------ self check
std::string Ftl::check_invariants() const {
    std::ostringstream err;
    std::vector<uint32_t> valid(geo_.num_blocks, 0);
    for (Lpn lpn = 0; lpn < logical_pages_; ++lpn) {
        Ppn ppn = l2p_[lpn];
        if (ppn == kInvalid) continue;
        if (ppn >= geo_.total_pages()) { err << "lpn " << lpn << " maps out of range; "; continue; }
        if (p2l_[ppn] != lpn) err << "p2l mismatch for lpn " << lpn << "; ";
        BlockId b = ppn / ppb_;
        if (flash_.is_bad(b)) err << "lpn " << lpn << " maps into bad block " << b << "; ";
        if (!flash_.is_page_programmed(ppn)) err << "lpn " << lpn << " maps to unprogrammed page; ";
        if (meta_[b].state == BlockState::Free) err << "lpn " << lpn << " maps into free block " << b << "; ";
        ++valid[b];
    }
    for (Ppn ppn = 0; ppn < geo_.total_pages(); ++ppn) {
        if (p2l_[ppn] != kInvalid && (p2l_[ppn] >= logical_pages_ || l2p_[p2l_[ppn]] != ppn))
            err << "stale p2l at ppn " << ppn << "; ";
    }
    uint32_t nfree = 0;
    for (BlockId b = 0; b < geo_.num_blocks; ++b) {
        if (valid[b] != meta_[b].valid_count)
            err << "block " << b << " valid_count " << meta_[b].valid_count << " != " << valid[b] << "; ";
        if (meta_[b].state == BlockState::Free) {
            ++nfree;
            if (flash_.programmed_pages(b) != 0 || flash_.is_bad(b)) err << "free block " << b << " not erased/good; ";
        }
        if (meta_[b].state == BlockState::Bad && !flash_.is_bad(b)) err << "block " << b << " marked bad in FTL only; ";
    }
    if (nfree != free_.size()) err << "free list size " << free_.size() << " != free blocks " << nfree << "; ";
    for (BlockId b : free_)
        if (meta_[b].state != BlockState::Free) err << "free list holds non-free block " << b << "; ";
    if (host_.blk != kInvalid && meta_[host_.blk].state != BlockState::ActiveHost) err << "host stream state; ";
    if (gc_.blk != kInvalid && meta_[gc_.blk].state != BlockState::ActiveGc) err << "gc stream state; ";
    return err.str();
}

}  // namespace nandsim
