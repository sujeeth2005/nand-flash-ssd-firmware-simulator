#!/usr/bin/env python3
"""Run the C++ unit-test binary and produce a pass/fail report (Markdown + JSON).

  python3 tools/run_tests.py --tests build/nandsim_tests --report reports
Exit code 0 = all tests passed.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tests", default="build/nandsim_tests")
    ap.add_argument("--report", default="reports")
    args = ap.parse_args()

    t0 = time.time()
    p = subprocess.run([args.tests], capture_output=True, text=True)
    wall = time.time() - t0
    results, current_msgs = [], []
    for line in p.stdout.splitlines():
        m = re.match(r"\[\s*(OK|FAIL)\s*\] (\S+) \(([\d.e+-]+) ms\)", line)
        if m:
            results.append({"name": m.group(2), "passed": m.group(1) == "OK", "ms": float(m.group(3)),
                            "messages": current_msgs})
            current_msgs = []
        elif line.strip().startswith("FAILED") or line.startswith("    "):
            current_msgs.append(line.strip())
        elif line.startswith("[ RUN"):
            current_msgs = []

    suites = {}
    for r in results:
        s = suites.setdefault(r["name"].split(".")[0], {"passed": 0, "failed": 0, "ms": 0.0})
        s["passed" if r["passed"] else "failed"] += 1
        s["ms"] += r["ms"]
    failed = [r for r in results if not r["passed"]]

    os.makedirs(args.report, exist_ok=True)
    with open(os.path.join(args.report, "test_report.json"), "w") as f:
        json.dump({"total": len(results), "failed": len(failed), "wall_seconds": wall,
                   "suites": suites, "results": results}, f, indent=2)
    with open(os.path.join(args.report, "test_report.md"), "w") as f:
        f.write(f"# nandsim test report\n\n{len(results) - len(failed)}/{len(results)} tests passed "
                f"in {wall:.1f}s.\n\n| suite | passed | failed | time (ms) |\n|---|---|---|---|\n")
        for name, s in suites.items():
            f.write(f"| {name} | {s['passed']} | {s['failed']} | {s['ms']:.0f} |\n")
        if failed:
            f.write("\n## Failures\n\n")
            for r in failed:
                f.write(f"### {r['name']}\n```\n" + "\n".join(r["messages"]) + "\n```\n")
    print(f"{len(results) - len(failed)}/{len(results)} tests passed ({wall:.1f}s) -> {args.report}/test_report.md")
    for r in failed:
        print(f"FAILED {r['name']}:\n  " + "\n  ".join(r["messages"]), file=sys.stderr)
    sys.exit(1 if failed or p.returncode not in (0, 1) else 0)


if __name__ == "__main__":
    main()
