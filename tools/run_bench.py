#!/usr/bin/env python3
"""Benchmark matrix runner + performance regression check.

Runs every (workload x GC policy x wear-levelling mode) combination through nandsim_cli and
collects write amplification, latency percentiles, throughput and erase-count statistics.

  python3 tools/run_bench.py --cli build/nandsim_cli --out results            # full run
  python3 tools/run_bench.py --cli build/nandsim_cli --quick --out /tmp/r \
          --baseline bench/baseline.csv                                       # CI regression gate

The simulator is deterministic (seeded), so identical code gives identical numbers; any change
beyond --tolerance in WA or write p99.9 latency vs the baseline fails the run (exit code 1).
"""
import argparse
import csv
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_workload  # noqa: E402

WORKLOADS = {
    # name: generator kwargs
    "rand_write": dict(pattern="rand_write"),
    "hotcold_90_10": dict(pattern="hotcold", hot_frac=0.1, hot_prob=0.9),
    "zipf": dict(pattern="zipf", zipf_s=1.0),
    "mixed_70r": dict(pattern="mixed", read_ratio=0.7),
    "seq_write": dict(pattern="seq_write"),
}
GC = ["greedy", "cost-benefit"]
WEAR = ["none", "dynamic", "static"]
FIELDS = ["workload", "gc", "wear", "write_amplification", "write_p50_us", "write_p99_us", "write_p999_us",
          "read_p99_us", "throughput_MBps", "erase_min", "erase_max", "erase_mean", "erase_stddev",
          "gc_runs", "wl_migrations", "bad_blocks", "verify_mismatches"]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r


def make_trace(path, name, lpns, ops, seed):
    ns = argparse.Namespace(pattern="rand_write", lpns=lpns, ops=ops, req_pages=1, seed=seed, fill=True,
                            hot_frac=0.1, hot_prob=0.9, zipf_s=1.0, read_ratio=0.3)
    for k, v in WORKLOADS[name].items():
        setattr(ns, k, v)
    with open(path, "w") as f:
        gen_workload.generate(ns, f)


def load_csv(path):
    with open(path) as f:
        return {(r["workload"], r["gc"], r["wear"]): r for r in csv.DictReader(f)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", default="build/nandsim_cli")
    ap.add_argument("--out", default="results")
    ap.add_argument("--quick", action="store_true", help="smaller device and fewer ops (CI)")
    ap.add_argument("--blocks", type=int)
    ap.add_argument("--ops", type=int)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--workloads", default=",".join(WORKLOADS))
    ap.add_argument("--baseline", help="CSV from an earlier run to compare against")
    ap.add_argument("--tolerance", type=float, default=0.05, help="allowed relative regression (default 5%%)")
    ap.add_argument("--update-baseline", help="write this run's CSV to the given path")
    args = ap.parse_args()

    blocks = args.blocks or (64 if args.quick else 128)
    ops = args.ops or (40000 if args.quick else 150000)
    dev = ["--blocks", str(blocks), "--pages-per-block", "64", "--page-size", "4096", "--op", "0.125",
           "--wl-threshold", "20", "--seed", str(args.seed)]

    info = json.loads(run([args.cli, "info"] + dev).stdout)
    lpns = info["logical_pages"]
    os.makedirs(os.path.join(args.out, "json"), exist_ok=True)
    os.makedirs(os.path.join(args.out, "traces"), exist_ok=True)
    print(f"device: {blocks} blocks x 64 pages x 4 KiB, {lpns} logical pages, {ops} measured ops/workload")

    rows = []
    for wl in args.workloads.split(","):
        trace = os.path.join(args.out, "traces", f"{wl}.txt")
        make_trace(trace, wl, lpns, ops, args.seed)
        for gc in GC:
            for wear in WEAR:
                jpath = os.path.join(args.out, "json", f"{wl}__{gc}__{wear}.json")
                r = run([args.cli, "run", "--workload", trace, "--gc", gc, "--wear", wear,
                         "--json", jpath] + dev)
                if not os.path.exists(jpath):
                    print(f"FAILED {wl}/{gc}/{wear}: {r.stderr.strip()}", file=sys.stderr)
                    sys.exit(2)
                j = json.load(open(jpath))
                w = j["performance"]["write_latency_us"]
                row = {
                    "workload": wl, "gc": gc, "wear": wear,
                    "write_amplification": j["ftl"]["write_amplification"],
                    "write_p50_us": w["p50"], "write_p99_us": w["p99"], "write_p999_us": w["p999"],
                    "read_p99_us": j["performance"]["read_latency_us"]["p99"],
                    "throughput_MBps": j["performance"]["throughput_MBps"],
                    "erase_min": j["wear"]["min"], "erase_max": j["wear"]["max"],
                    "erase_mean": j["wear"]["mean"], "erase_stddev": j["wear"]["stddev"],
                    "gc_runs": j["ftl"]["gc_runs"], "wl_migrations": j["ftl"]["wl_migrations"],
                    "bad_blocks": j["ftl"]["bad_blocks"], "verify_mismatches": j["host"]["verify_mismatches"],
                }
                if r.returncode != 0 or not j["invariants_ok"]:
                    row["verify_mismatches"] = max(1, row["verify_mismatches"])
                rows.append(row)
                print(f"  {wl:14s} {gc:12s} {wear:8s} WA={float(row['write_amplification']):6.3f} "
                      f"p99={float(row['write_p99_us']):>9.1f}us  wear {row['erase_min']}..{row['erase_max']}  "
                      f"{row['throughput_MBps']} MB/s")

    csv_path = os.path.join(args.out, "bench.csv")
    for path in [csv_path] + ([args.update_baseline] if args.update_baseline else []):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=FIELDS)
            w.writeheader()
            w.writerows(rows)
    print(f"wrote {csv_path}")

    bad_correct = [r for r in rows if int(r["verify_mismatches"]) != 0]
    if bad_correct:
        print(f"CORRECTNESS FAILURE in {len(bad_correct)} runs", file=sys.stderr)
        sys.exit(1)

    if args.baseline:
        base = load_csv(args.baseline)
        regress = []
        for r in rows:
            b = base.get((r["workload"], r["gc"], r["wear"]))
            if not b:
                continue
            for metric in ("write_amplification", "write_p999_us"):
                new, old = float(r[metric]), float(b[metric])
                if old > 0 and new > old * (1 + args.tolerance):
                    regress.append(f"{r['workload']}/{r['gc']}/{r['wear']}: {metric} {old:g} -> {new:g} "
                                   f"(+{(new / old - 1) * 100:.1f}%)")
        if regress:
            print("PERFORMANCE REGRESSIONS vs baseline:", file=sys.stderr)
            for line in regress:
                print("  " + line, file=sys.stderr)
            sys.exit(1)
        print(f"no regressions vs {args.baseline} (tolerance {args.tolerance * 100:.0f}%)")


if __name__ == "__main__":
    main()
