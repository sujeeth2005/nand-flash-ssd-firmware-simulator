#include <sstream>

#include "nandsim/util.hpp"
#include "nandsim/workload.hpp"
#include "test_framework.hpp"

using namespace nandsim;

TEST(workload, parses_trace_with_comments_and_counts) {
    std::istringstream in("# header\nW 5 4\nR 7\nT 9 2   # trim\nRESET\n\n");
    std::vector<TraceOp> ops; std::string err;
    REQUIRE(parse_trace(in, &ops, &err));
    REQUIRE_EQ(ops.size(), 4u);
    CHECK(ops[0].kind == TraceOp::Write); CHECK_EQ(ops[0].lpn, 5u); CHECK_EQ(ops[0].count, 4u);
    CHECK(ops[1].kind == TraceOp::Read);  CHECK_EQ(ops[1].count, 1u);
    CHECK(ops[2].kind == TraceOp::Trim);  CHECK_EQ(ops[2].count, 2u);
    CHECK(ops[3].kind == TraceOp::Reset);
}

TEST(workload, rejects_malformed_lines) {
    std::vector<TraceOp> ops; std::string err;
    std::istringstream a("X 1\n"); CHECK(!parse_trace(a, &ops, &err)); CHECK(!err.empty());
    std::istringstream b("W\n");   CHECK(!parse_trace(b, &ops, &err));
    std::istringstream c("W 1 0\n"); CHECK(!parse_trace(c, &ops, &err));
}

TEST(workload, runner_verifies_reads_and_resets_stats) {
    FlashModel flash(FlashGeometry{64, 16, 64});
    Ftl ftl(flash, FtlConfig{});
    REQUIRE_EQ(ftl.mount(), Status::Ok);
    std::istringstream in("W 0 100\nRESET\nW 3 10\nR 0 100\nT 50 5\nR 50 5\n");
    std::vector<TraceOp> ops; std::string err;
    REQUIRE(parse_trace(in, &ops, &err));
    TraceRunner runner(ftl, true);
    RunResult r = runner.run(ops);
    CHECK_EQ(r.verify_mismatches, 0u);
    CHECK_EQ(r.errors, 0u);
    CHECK_EQ(r.host_write_pages, 10u);   // the 100 pre-RESET writes are excluded
    CHECK_EQ(r.host_read_pages, 105u);
    CHECK_EQ(r.write_lat_ns.size(), 1u);
    CHECK(r.write_lat_ns[0] > 0);
    CHECK_EQ(ftl.stats().host_writes, 10u);
}

TEST(workload, runner_reports_out_of_range_requests) {
    FlashModel flash(FlashGeometry{64, 16, 64});
    Ftl ftl(flash, FtlConfig{});
    ftl.mount();
    std::istringstream in("W 100000 1\n");
    std::vector<TraceOp> ops; std::string err;
    REQUIRE(parse_trace(in, &ops, &err));
    RunResult r = TraceRunner(ftl, true).run(ops);
    CHECK_EQ(r.errors, 1u);
}

TEST(workload, percentile_nearest_rank) {
    std::vector<uint64_t> v{10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    CHECK_EQ(percentile(v, 50), 50u);
    CHECK_EQ(percentile(v, 90), 90u);
    CHECK_EQ(percentile(v, 99), 100u);
    CHECK_EQ(percentile(v, 100), 100u);
    CHECK_EQ(percentile({}, 50), 0u);
}
