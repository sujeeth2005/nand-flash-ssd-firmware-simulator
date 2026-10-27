// nandsim_cli - command line front end.
//   nandsim_cli info       [device options]
//   nandsim_cli run        --workload trace.txt [device options] [--json out.json] [--no-verify]
//   nandsim_cli crashfuzz  --seed N [--ops N] [device options]          (one randomized power-loss trial)
//   nandsim_cli crashsweep [--ops N] [--stride N] [device options]      (exhaustive crash-point sweep)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "nandsim/crash_harness.hpp"
#include "nandsim/ftl.hpp"
#include "nandsim/workload.hpp"

using namespace nandsim;

namespace {

struct Args {
    std::string cmd;
    std::map<std::string, std::string> kv;
    bool has(const std::string& k) const { return kv.count(k) != 0; }
    std::string str(const std::string& k, const std::string& d = "") const {
        auto it = kv.find(k);
        return it == kv.end() ? d : it->second;
    }
    long long num(const std::string& k, long long d) const { return has(k) ? std::stoll(kv.at(k)) : d; }
    double dbl(const std::string& k, double d) const { return has(k) ? std::stod(kv.at(k)) : d; }
};

bool parse_args(int argc, char** argv, Args* a) {
    if (argc < 2) return false;
    a->cmd = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string k = argv[i];
        if (k.rfind("--", 0) != 0) { std::cerr << "unexpected argument: " << k << "\n"; return false; }
        k = k.substr(2);
        if (k == "no-verify") { a->kv[k] = "1"; continue; }
        if (i + 1 >= argc) { std::cerr << "missing value for --" << k << "\n"; return false; }
        a->kv[k] = argv[++i];
    }
    return true;
}

bool build_config(const Args& a, FlashGeometry* geo, FlashFaultConfig* faults, FtlConfig* cfg) {
    geo->num_blocks = uint32_t(a.num("blocks", 128));
    geo->pages_per_block = uint32_t(a.num("pages-per-block", 64));
    geo->page_size = uint32_t(a.num("page-size", 512));
    faults->max_pe_cycles = uint32_t(a.num("max-pe", 3000));
    faults->ecc_correctable_bits = uint32_t(a.num("ecc-bits", 8));
    faults->raw_bit_error_prob = a.dbl("bit-error-prob", 0.0);
    faults->seed = uint64_t(a.num("seed", 1));
    cfg->overprovision = a.dbl("op", 0.125);
    cfg->static_wl_threshold = uint32_t(a.num("wl-threshold", 20));
    std::string gc = a.str("gc", "greedy");
    if (gc == "greedy") cfg->gc_policy = GcPolicy::Greedy;
    else if (gc == "cost-benefit" || gc == "cb") cfg->gc_policy = GcPolicy::CostBenefit;
    else { std::cerr << "unknown --gc " << gc << " (greedy|cost-benefit)\n"; return false; }
    std::string wl = a.str("wear", "static");
    if (wl == "none") cfg->wear_mode = WearMode::None;
    else if (wl == "dynamic") cfg->wear_mode = WearMode::Dynamic;
    else if (wl == "static") cfg->wear_mode = WearMode::Static;
    else { std::cerr << "unknown --wear " << wl << " (none|dynamic|static)\n"; return false; }
    return true;
}

std::string us(uint64_t ns) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", double(ns) / 1000.0);
    return b;
}
std::string fnum(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

int cmd_info(const Args& a) {
    FlashGeometry geo; FlashFaultConfig faults; FtlConfig cfg;
    if (!build_config(a, &geo, &faults, &cfg)) return 2;
    FlashModel flash(geo, FlashTiming{}, faults);
    Ftl ftl(flash, cfg);
    std::printf("{\"blocks\":%u,\"pages_per_block\":%u,\"page_size\":%u,\"op_blocks\":%u,\"logical_pages\":%u}\n",
                geo.num_blocks, geo.pages_per_block, geo.page_size, ftl.op_blocks(), ftl.logical_pages());
    return 0;
}

int cmd_run(const Args& a) {
    FlashGeometry geo; FlashFaultConfig faults; FtlConfig cfg;
    if (!build_config(a, &geo, &faults, &cfg)) return 2;
    if (!a.has("workload")) { std::cerr << "--workload required\n"; return 2; }
    std::ifstream in(a.str("workload"));
    if (!in) { std::cerr << "cannot open " << a.str("workload") << "\n"; return 2; }
    std::vector<TraceOp> ops;
    std::string err;
    if (!parse_trace(in, &ops, &err)) { std::cerr << "trace error: " << err << "\n"; return 2; }

    FlashModel flash(geo, FlashTiming{}, faults);
    Ftl ftl(flash, cfg);
    Status ms = ftl.mount();
    if (ms != Status::Ok) { std::cerr << "mount failed: " << ms << "\n"; return 3; }

    TraceRunner runner(ftl, !a.has("no-verify"));
    RunResult r = runner.run(ops);
    std::string inv = ftl.check_invariants();

    FlashStats fs = flash.stats();
    const FtlStats& st = ftl.stats();
    uint64_t host_bytes = (r.host_write_pages + r.host_read_pages) * uint64_t(geo.page_size);
    double mbps = fs.busy_ns ? double(host_bytes) / 1e6 / (double(fs.busy_ns) / 1e9) : 0.0;

    std::vector<uint64_t> ec;
    double sum = 0, sumsq = 0;
    uint32_t mn = 0xFFFFFFFFu, mx = 0, bad = 0;
    for (BlockId b = 0; b < geo.num_blocks; ++b) {
        if (flash.is_bad(b)) ++bad;
        uint32_t e = flash.erase_count(b);
        ec.push_back(e);
        mn = std::min(mn, e); mx = std::max(mx, e);
        sum += e; sumsq += double(e) * e;
    }
    double mean = sum / geo.num_blocks;
    double sd = std::sqrt(std::max(0.0, sumsq / geo.num_blocks - mean * mean));

    std::ostringstream j;
    j << "{\n";
    j << "  \"config\": {\"blocks\": " << geo.num_blocks << ", \"pages_per_block\": " << geo.pages_per_block
      << ", \"page_size\": " << geo.page_size << ", \"overprovision\": " << cfg.overprovision
      << ", \"gc\": \"" << to_string(cfg.gc_policy) << "\", \"wear\": \"" << to_string(cfg.wear_mode)
      << "\", \"logical_pages\": " << ftl.logical_pages() << "},\n";
    j << "  \"host\": {\"write_pages\": " << r.host_write_pages << ", \"read_pages\": " << r.host_read_pages
      << ", \"trim_pages\": " << r.trim_pages << ", \"errors\": " << r.errors
      << ", \"verify_mismatches\": " << r.verify_mismatches << "},\n";
    j << "  \"flash\": {\"page_reads\": " << fs.page_reads << ", \"page_programs\": " << fs.page_programs
      << ", \"block_erases\": " << fs.block_erases << ", \"busy_ms\": " << fnum(double(fs.busy_ns) / 1e6) << "},\n";
    j << "  \"ftl\": {\"write_amplification\": " << fnum(ftl.write_amplification())
      << ", \"gc_runs\": " << st.gc_runs << ", \"gc_pages_moved\": " << st.gc_pages_moved
      << ", \"wl_migrations\": " << st.wl_migrations << ", \"wl_pages_moved\": " << st.wl_pages_moved
      << ", \"program_retries\": " << st.program_retries << ", \"bad_blocks\": " << bad
      << ", \"data_loss_pages\": " << st.data_loss_pages << "},\n";
    j << "  \"performance\": {\"throughput_MBps\": " << fnum(mbps)
      << ", \"write_latency_us\": {\"p50\": " << us(percentile(r.write_lat_ns, 50)) << ", \"p99\": "
      << us(percentile(r.write_lat_ns, 99)) << ", \"p999\": " << us(percentile(r.write_lat_ns, 99.9))
      << ", \"max\": " << us(percentile(r.write_lat_ns, 100)) << "}, \"read_latency_us\": {\"p50\": "
      << us(percentile(r.read_lat_ns, 50)) << ", \"p99\": " << us(percentile(r.read_lat_ns, 99))
      << ", \"p999\": " << us(percentile(r.read_lat_ns, 99.9)) << "}},\n";
    j << "  \"wear\": {\"min\": " << mn << ", \"max\": " << mx << ", \"mean\": " << fnum(mean)
      << ", \"stddev\": " << fnum(sd) << ", \"erase_counts\": [";
    for (size_t i = 0; i < ec.size(); ++i) j << (i ? "," : "") << ec[i];
    j << "]},\n";
    j << "  \"invariants_ok\": " << (inv.empty() ? "true" : "false") << ",\n";
    j << "  \"first_error\": \"" << r.first_error << "\"\n}\n";

    if (a.has("json")) {
        std::ofstream out(a.str("json"));
        out << j.str();
    } else {
        std::cout << j.str();
    }
    if (!inv.empty()) std::cerr << "INVARIANT VIOLATION: " << inv << "\n";
    return (r.verify_mismatches == 0 && inv.empty()) ? 0 : 1;
}

bool crash_config(const Args& a, CrashTrialConfig* c) {
    FlashFaultConfig faults;
    c->geo = FlashGeometry{uint32_t(a.num("blocks", 32)), uint32_t(a.num("pages-per-block", 16)),
                           uint32_t(a.num("page-size", 32))};
    FlashGeometry dummy; FtlConfig cfg;
    Args b = a;
    if (!b.has("op")) b.kv["op"] = "0.25";
    if (!build_config(b, &dummy, &faults, &cfg)) return false;
    c->ftl = cfg;
    c->seed = uint64_t(a.num("seed", 1));
    c->ops = uint32_t(a.num("ops", 2000));
    c->mean_crash_interval = uint32_t(a.num("crash-interval", 40));
    return true;
}

int cmd_crashfuzz(const Args& a) {
    CrashTrialConfig c;
    if (!crash_config(a, &c)) return 2;
    CrashTrialResult r = run_crash_trial(c);
    std::printf("{\"seed\":%llu,\"passed\":%s,\"crashes\":%u,\"recovery_crashes\":%u,\"ops\":%u,"
                "\"mutations\":%llu,\"gc\":\"%s\",\"wear\":\"%s\",\"failure\":\"%s\"}\n",
                (unsigned long long)c.seed, r.passed ? "true" : "false", r.crashes, r.recovery_crashes,
                r.ops_done, (unsigned long long)r.mutations, to_string(c.ftl.gc_policy),
                to_string(c.ftl.wear_mode), r.failure.c_str());
    return r.passed ? 0 : 1;
}

int cmd_crashsweep(const Args& a) {
    CrashTrialConfig c;
    if (!crash_config(a, &c)) return 2;
    c.ops = uint32_t(a.num("ops", 700));
    CrashSweepResult r = run_crash_sweep(c, uint32_t(a.num("stride", 1)));
    std::printf("{\"trials\":%llu,\"failures\":%llu,\"flash_mutations_in_scenario\":%llu,"
                "\"gc\":\"%s\",\"wear\":\"%s\",\"first_failure\":\"%s\"}\n",
                (unsigned long long)r.trials, (unsigned long long)r.failures,
                (unsigned long long)r.total_mutations, to_string(c.ftl.gc_policy),
                to_string(c.ftl.wear_mode), r.first_failure.c_str());
    return r.failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse_args(argc, argv, &a)) {
        std::cerr << "usage: nandsim_cli {info|run|crashfuzz|crashsweep} [--key value ...]\n"
                     "  device: --blocks N --pages-per-block N --page-size N --op F --gc greedy|cost-benefit\n"
                     "          --wear none|dynamic|static --wl-threshold N --max-pe N --seed N\n"
                     "          --bit-error-prob P --ecc-bits N\n"
                     "  run:    --workload FILE [--json FILE] [--no-verify]\n";
        return 2;
    }
    try {
        if (a.cmd == "info") return cmd_info(a);
        if (a.cmd == "run") return cmd_run(a);
        if (a.cmd == "crashfuzz") return cmd_crashfuzz(a);
        if (a.cmd == "crashsweep") return cmd_crashsweep(a);
        std::cerr << "unknown command: " << a.cmd << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
    return 2;
}
