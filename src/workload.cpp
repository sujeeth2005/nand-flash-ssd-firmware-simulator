#include "nandsim/workload.hpp"

#include <istream>
#include <sstream>

#include "nandsim/util.hpp"

namespace nandsim {

bool parse_trace(std::istream& in, std::vector<TraceOp>* ops, std::string* error) {
    std::string line;
    size_t lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream ss(line);
        std::string op;
        if (!(ss >> op)) continue;
        TraceOp t;
        if (op == "RESET") {
            t.kind = TraceOp::Reset;
            ops->push_back(t);
            continue;
        }
        if (op == "W") t.kind = TraceOp::Write;
        else if (op == "R") t.kind = TraceOp::Read;
        else if (op == "T") t.kind = TraceOp::Trim;
        else {
            if (error) *error = "line " + std::to_string(lineno) + ": unknown op '" + op + "'";
            return false;
        }
        long long lpn = -1, n = 1;
        if (!(ss >> lpn) || lpn < 0) {
            if (error) *error = "line " + std::to_string(lineno) + ": missing/invalid lpn";
            return false;
        }
        if (ss >> n) {
            if (n < 1) {
                if (error) *error = "line " + std::to_string(lineno) + ": invalid page count";
                return false;
            }
        } else {
            n = 1;
        }
        t.lpn = Lpn(lpn);
        t.count = uint32_t(n);
        ops->push_back(t);
    }
    return true;
}

RunResult TraceRunner::run(const std::vector<TraceOp>& ops) {
    RunResult r;
    auto note = [&](Status s, const char* what, Lpn lpn) {
        ++r.errors;
        if (r.first_error.empty())
            r.first_error = std::string(what) + " lpn " + std::to_string(lpn) + ": " + to_string(s);
    };
    std::vector<uint8_t> buf;
    uint32_t next_version = 0;

    for (const TraceOp& op : ops) {
        if (op.kind == TraceOp::Reset) {
            r = RunResult{};
            ftl_.reset_stats();
            continue;
        }
        uint64_t lat = 0;
        for (uint32_t k = 0; k < op.count; ++k) {
            Lpn lpn = op.lpn + k;
            if (lpn >= ftl_.logical_pages()) { note(Status::ErrInvalidAddress, "request", lpn); break; }
            if (op.kind == TraceOp::Write) {
                uint32_t ver = ++next_version;
                Status s = ftl_.write(lpn, make_page_data(lpn, ver, ftl_.page_size()));
                lat += ftl_.last_op_latency_ns();
                if (s == Status::Ok) { version_[lpn] = ver; ++r.host_write_pages; }
                else note(s, "write", lpn);
            } else if (op.kind == TraceOp::Read) {
                Status s = ftl_.read(lpn, buf);
                lat += ftl_.last_op_latency_ns();
                ++r.host_read_pages;
                if (s == Status::Ok) {
                    if (verify_ && (version_[lpn] == 0 ||
                                    buf != make_page_data(lpn, version_[lpn], ftl_.page_size()))) {
                        ++r.verify_mismatches;
                        if (r.first_error.empty()) r.first_error = "verify mismatch lpn " + std::to_string(lpn);
                    }
                } else if (s == Status::Unmapped) {
                    if (verify_ && version_[lpn] != 0) {
                        ++r.verify_mismatches;
                        if (r.first_error.empty()) r.first_error = "unexpected unmapped lpn " + std::to_string(lpn);
                    }
                } else {
                    note(s, "read", lpn);
                }
            } else {
                Status s = ftl_.trim(lpn);
                if (s == Status::Ok) { version_[lpn] = 0; ++r.trim_pages; }
                else note(s, "trim", lpn);
            }
        }
        if (op.kind == TraceOp::Write) r.write_lat_ns.push_back(lat);
        else if (op.kind == TraceOp::Read) r.read_lat_ns.push_back(lat);
    }
    return r;
}

}  // namespace nandsim
