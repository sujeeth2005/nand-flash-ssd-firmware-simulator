#include "nandsim/flash_model.hpp"

#include <algorithm>

namespace nandsim {

FlashModel::FlashModel(FlashGeometry geo, FlashTiming timing, FlashFaultConfig faults)
    : geo_(geo), timing_(timing), fault_(faults), rng_(faults.seed), blocks_(geo.num_blocks) {
    for (auto& b : blocks_) b.pages.resize(geo.pages_per_block);
}

bool FlashModel::is_page_programmed(Ppn ppn) const {
    if (ppn >= geo_.total_pages()) return false;
    return blocks_[ppn / geo_.pages_per_block].pages[ppn % geo_.pages_per_block].programmed;
}

FlashStats FlashModel::stats() const {
    FlashStats s = stats_;
    s.busy_ns = busy_ns_ - busy_base_;
    return s;
}

void FlashModel::reset_stats() {
    stats_ = FlashStats{};
    busy_base_ = busy_ns_;
}

void FlashModel::arm_power_loss(uint64_t after_n, TearMode mode) {
    loss_armed_ = true;
    loss_countdown_ = after_n;
    tear_mode_ = mode;
}

bool FlashModel::power_loss_now() {
    if (!loss_armed_) return false;
    if (loss_countdown_ > 0) { --loss_countdown_; return false; }
    loss_armed_ = false;
    powered_ = false;
    return true;
}

bool FlashModel::tear_is_torn() {
    if (tear_mode_ == TearMode::Torn) return true;
    if (tear_mode_ == TearMode::Lost) return false;
    return (rng_.next() & 1) != 0;
}

int FlashModel::sample_bit_errors() {
    if (fault_.raw_bit_error_prob <= 0.0) return 0;
    int n = 0;
    while (n < 1000 && rng_.uniform() < fault_.raw_bit_error_prob) ++n;
    return n;
}

Status FlashModel::read_page(Ppn ppn, std::vector<uint8_t>* data, Oob* oob) {
    if (!powered_) return Status::ErrPowerLoss;
    if (ppn >= geo_.total_pages()) return Status::ErrInvalidAddress;
    Block& b = blocks_[ppn / geo_.pages_per_block];
    if (b.bad) return Status::ErrBadBlock;
    const Page& p = b.pages[ppn % geo_.pages_per_block];
    ++stats_.page_reads;
    busy_ns_ += timing_.read_ns;

    if (!p.programmed) {  // erased page reads as all 0xFF
        if (data) data->assign(geo_.page_size, 0xFF);
        if (oob) *oob = Oob{};
        return Status::Ok;
    }
    if (p.torn) {
        ++stats_.ecc_uncorrectable_reads;
        return Status::ErrUncorrectable;
    }
    int errs = sample_bit_errors();
    if (errs > int(fault_.ecc_correctable_bits)) {
        ++stats_.ecc_uncorrectable_reads;
        return Status::ErrUncorrectable;
    }
    if (errs > 0) ++stats_.ecc_corrected_reads;  // ECC fixes it; caller sees clean data
    if (data) *data = p.data;
    if (oob) *oob = p.oob;
    return Status::Ok;
}

Status FlashModel::program_page(Ppn ppn, const uint8_t* data, size_t len, const Oob& oob) {
    if (!powered_) return Status::ErrPowerLoss;
    if (ppn >= geo_.total_pages()) return Status::ErrInvalidAddress;
    if (len != geo_.page_size || data == nullptr) return Status::ErrInvalidArgument;
    Block& b = blocks_[ppn / geo_.pages_per_block];
    if (b.bad) return Status::ErrBadBlock;
    uint32_t idx = ppn % geo_.pages_per_block;
    Page& p = b.pages[idx];
    if (p.programmed || idx < b.next_page) return Status::ErrNotErased;
    if (idx > b.next_page) return Status::ErrOutOfOrder;

    if (power_loss_now()) {
        if (tear_is_torn()) {  // first half of the page made it, the rest is still erased
            p.programmed = true;
            p.torn = true;
            p.oob = oob;
            p.data.assign(data, data + len);
            std::fill(p.data.begin() + len / 2, p.data.end(), 0xFF);
            b.next_page = idx + 1;
        }
        return Status::ErrPowerLoss;
    }
    ++mutations_;
    busy_ns_ += timing_.program_ns;
    if (b.fail_program) {  // program-status-fail: page is consumed but unusable
        b.fail_program = false;
        p.programmed = true;
        p.torn = true;
        b.next_page = idx + 1;
        ++stats_.program_fails;
        return Status::ErrProgramFail;
    }
    p.programmed = true;
    p.torn = false;
    p.oob = oob;
    p.data.assign(data, data + len);
    b.next_page = idx + 1;
    ++stats_.page_programs;
    return Status::Ok;
}

Status FlashModel::erase_block(BlockId id) {
    if (!powered_) return Status::ErrPowerLoss;
    if (id >= geo_.num_blocks) return Status::ErrInvalidAddress;
    Block& b = blocks_[id];
    if (b.bad) return Status::ErrBadBlock;

    if (power_loss_now()) {
        if (tear_is_torn()) {  // half-erased block: every page is garbage until erased again
            for (auto& p : b.pages) {
                p.programmed = true;
                p.torn = true;
                p.oob = Oob{};
                p.data.clear();
            }
            b.next_page = geo_.pages_per_block;
        }
        return Status::ErrPowerLoss;
    }
    ++mutations_;
    busy_ns_ += timing_.erase_ns;
    if (b.fail_erase || b.erase_count >= fault_.max_pe_cycles) {
        b.fail_erase = false;
        b.bad = true;
        ++stats_.erase_fails;
        return Status::ErrEraseFail;
    }
    for (auto& p : b.pages) {
        p.programmed = false;
        p.torn = false;
        p.oob = Oob{};
        std::vector<uint8_t>().swap(p.data);
    }
    b.next_page = 0;
    ++b.erase_count;
    ++stats_.block_erases;
    return Status::Ok;
}

}  // namespace nandsim
