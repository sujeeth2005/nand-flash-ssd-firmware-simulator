#pragma once
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "nandsim/ftl.hpp"

namespace nandsim {

// Trace format (text, one request per line):   W <lpn> [pages]   R <lpn> [pages]   T <lpn> [pages]
//                                              RESET   (clear statistics: marks end of warm-up)
//                                              # comment
struct TraceOp {
    enum Kind { Write, Read, Trim, Reset } kind = Write;
    Lpn lpn = 0;
    uint32_t count = 1;
};

bool parse_trace(std::istream& in, std::vector<TraceOp>* ops, std::string* error);

struct RunResult {
    std::vector<uint64_t> write_lat_ns, read_lat_ns;  // per request (sum over its pages)
    uint64_t host_write_pages = 0, host_read_pages = 0, trim_pages = 0;
    uint64_t verify_mismatches = 0;  // reads that returned data differing from what was written
    uint64_t errors = 0;             // non-OK statuses (ErrNoSpace, ErrUncorrectable, ...)
    std::string first_error;
};

// Replays a trace against an FTL and (optionally) verifies every read against an oracle.
class TraceRunner {
public:
    TraceRunner(Ftl& ftl, bool verify) : ftl_(ftl), verify_(verify), version_(ftl.logical_pages(), 0) {}
    RunResult run(const std::vector<TraceOp>& ops);
private:
    Ftl& ftl_;
    bool verify_;
    std::vector<uint32_t> version_;  // 0 = never written / trimmed
};

}  // namespace nandsim
