#!/usr/bin/env python3
"""Power-loss fault-injection campaign.

Runs many randomized crash trials (different seeds, GC policies, wear modes) in parallel plus an
exhaustive crash-point sweep, and summarises the outcome. Every failure prints a one-line repro.

  python3 tools/crash_campaign.py --cli build/nandsim_cli --seeds 300 --report reports
"""
import argparse
import concurrent.futures as cf
import json
import os
import subprocess
import sys

GC = ["greedy", "cost-benefit"]
WEAR = ["none", "dynamic", "static"]


def trial(cli, seed, gc, wear, ops, interval):
    cmd = [cli, "crashfuzz", "--seed", str(seed), "--ops", str(ops), "--gc", gc, "--wear", wear,
           "--crash-interval", str(interval)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    try:
        j = json.loads(r.stdout)
    except json.JSONDecodeError:
        j = {"passed": False, "failure": f"cli error: {r.stderr.strip()[:200]}", "crashes": 0,
             "recovery_crashes": 0, "ops": 0}
    j["seed"], j["gc"], j["wear"] = seed, gc, wear
    j["repro"] = " ".join(cmd)
    return j


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", default="build/nandsim_cli")
    ap.add_argument("--seeds", type=int, default=200)
    ap.add_argument("--ops", type=int, default=2000)
    ap.add_argument("--crash-interval", type=int, default=40, help="mean flash ops between power cuts")
    ap.add_argument("--sweep-ops", type=int, default=700)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--report", default="reports")
    args = ap.parse_args()

    configs = [(gc, w) for gc in GC for w in WEAR]
    jobs = [(args.cli, s, *configs[s % len(configs)], args.ops, args.crash_interval)
            for s in range(1, args.seeds + 1)]
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        results = list(ex.map(lambda a: trial(*a), jobs))

    sweeps = []
    for gc, w in configs:
        r = subprocess.run([args.cli, "crashsweep", "--ops", str(args.sweep_ops), "--gc", gc, "--wear", w],
                           capture_output=True, text=True)
        try:
            sweeps.append(json.loads(r.stdout))
        except json.JSONDecodeError:
            sweeps.append({"trials": 0, "failures": 1, "first_failure": r.stderr.strip()[:200],
                           "gc": gc, "wear": w, "flash_mutations_in_scenario": 0})

    failed = [r for r in results if not r["passed"]]
    crashes = sum(r["crashes"] for r in results)
    rec_crashes = sum(r["recovery_crashes"] for r in results)
    sweep_trials = sum(s["trials"] for s in sweeps)
    sweep_fail = sum(s["failures"] for s in sweeps)

    os.makedirs(args.report, exist_ok=True)
    with open(os.path.join(args.report, "crash_campaign.md"), "w") as f:
        f.write("# Power-loss campaign\n\n")
        f.write(f"* randomized trials: **{len(results)}** ({len(results) - len(failed)} passed, {len(failed)} failed)\n")
        f.write(f"* power losses injected: **{crashes}** (+{rec_crashes} during recovery itself)\n")
        f.write(f"* exhaustive crash-point sweep: **{sweep_trials}** crash points "
                f"({sweep_fail} failed) across {len(configs)} configurations\n\n")
        f.write("| gc | wear | sweep crash points | failures |\n|---|---|---|---|\n")
        for s in sweeps:
            f.write(f"| {s['gc']} | {s['wear']} | {s['trials']} | {s['failures']} |\n")
        if failed or sweep_fail:
            f.write("\n## Failures\n")
            for r in failed:
                f.write(f"* seed {r['seed']} ({r['gc']}/{r['wear']}): {r['failure']}\n  repro: `{r['repro']}`\n")
            for s in sweeps:
                if s["failures"]:
                    f.write(f"* sweep {s['gc']}/{s['wear']}: {s['first_failure']}\n")
    print(f"trials: {len(results)}  failed: {len(failed)}  power losses: {crashes} "
          f"(+{rec_crashes} during recovery)  sweep: {sweep_trials} crash points, {sweep_fail} failed")
    for r in failed:
        print(f"FAIL seed={r['seed']} {r['gc']}/{r['wear']}: {r['failure']}\n  repro: {r['repro']}", file=sys.stderr)
    sys.exit(1 if failed or sweep_fail else 0)


if __name__ == "__main__":
    main()
