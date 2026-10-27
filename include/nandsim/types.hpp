#pragma once
#include <cstdint>
#include <ostream>

namespace nandsim {

using Lpn = uint32_t;      // logical page number (host view)
using Ppn = uint32_t;      // physical page number (block * pages_per_block + page)
using BlockId = uint32_t;
constexpr uint32_t kInvalid = 0xFFFFFFFFu;

enum class Status {
    Ok = 0,
    Unmapped,           // host read of a never-written / trimmed LPN (zeros returned); not an error
    ErrInvalidAddress,
    ErrInvalidArgument,
    ErrNotErased,       // program to a page that is not erased
    ErrOutOfOrder,      // program that skips pages inside a block (NAND requires sequential order)
    ErrBadBlock,
    ErrProgramFail,
    ErrEraseFail,
    ErrUncorrectable,   // ECC could not correct the page (or the page is torn)
    ErrPowerLoss,       // simulated power loss; device is off until power_cycle()
    ErrNoSpace,         // no reclaimable space (too many bad blocks / device worn out)
    ErrCorrupt,         // metadata inconsistency detected
};

inline const char* to_string(Status s) {
    switch (s) {
        case Status::Ok: return "Ok";
        case Status::Unmapped: return "Unmapped";
        case Status::ErrInvalidAddress: return "ErrInvalidAddress";
        case Status::ErrInvalidArgument: return "ErrInvalidArgument";
        case Status::ErrNotErased: return "ErrNotErased";
        case Status::ErrOutOfOrder: return "ErrOutOfOrder";
        case Status::ErrBadBlock: return "ErrBadBlock";
        case Status::ErrProgramFail: return "ErrProgramFail";
        case Status::ErrEraseFail: return "ErrEraseFail";
        case Status::ErrUncorrectable: return "ErrUncorrectable";
        case Status::ErrPowerLoss: return "ErrPowerLoss";
        case Status::ErrNoSpace: return "ErrNoSpace";
        case Status::ErrCorrupt: return "ErrCorrupt";
    }
    return "?";
}
inline std::ostream& operator<<(std::ostream& os, Status s) { return os << to_string(s); }
inline bool ok(Status s) { return s == Status::Ok; }

}  // namespace nandsim
