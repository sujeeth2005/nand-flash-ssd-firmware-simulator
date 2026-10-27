#!/usr/bin/env python3
"""Plot benchmark results produced by run_bench.py.

  python3 tools/plot.py --results results            # writes results/plots/*.png
"""
import argparse
import csv
import json
import os
import sys

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib is required: pip install matplotlib")

WEAR_COLORS = {"none": "#d1495b", "dynamic": "#edae49", "static": "#00798c"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results")
    args = ap.parse_args()
    out = os.path.join(args.results, "plots")
    os.makedirs(out, exist_ok=True)
    rows = list(csv.DictReader(open(os.path.join(args.results, "bench.csv"))))
    workloads = list(dict.fromkeys(r["workload"] for r in rows))

    # 1) Write amplification per workload: GC policy x wear mode
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.2), sharey=True)
    for ax, gc in zip(axes, ["greedy", "cost-benefit"]):
        width = 0.26
        for i, wear in enumerate(["none", "dynamic", "static"]):
            vals = [float(next(r for r in rows if r["workload"] == w and r["gc"] == gc and r["wear"] == wear)
                          ["write_amplification"]) for w in workloads]
            ax.bar([x + (i - 1) * width for x in range(len(workloads))], vals, width,
                   label=f"wear: {wear}", color=WEAR_COLORS[wear])
        ax.set_xticks(range(len(workloads)))
        ax.set_xticklabels(workloads, rotation=20, ha="right")
        ax.set_title(f"GC policy: {gc}")
        ax.grid(axis="y", alpha=0.3)
    axes[0].set_ylabel("write amplification (flash programs / host writes)")
    axes[0].legend()
    fig.tight_layout()
    fig.savefig(os.path.join(out, "write_amplification.png"), dpi=140)
    plt.close(fig)

    # 2) Erase-count distribution (hot/cold workload) with and without wear levelling
    wl = "hotcold_90_10" if "hotcold_90_10" in workloads else workloads[0]
    fig, ax = plt.subplots(figsize=(8, 4.4))
    for wear in ["none", "dynamic", "static"]:
        p = os.path.join(args.results, "json", f"{wl}__greedy__{wear}.json")
        if not os.path.exists(p):
            continue
        counts = sorted(json.load(open(p))["wear"]["erase_counts"])
        ax.plot(range(len(counts)), counts, label=f"{wear} (max-min = {counts[-1] - counts[0]})",
                color=WEAR_COLORS[wear], linewidth=2)
    ax.set_xlabel("blocks, sorted by erase count")
    ax.set_ylabel("erase cycles")
    ax.set_title(f"Erase-count distribution, workload {wl}, greedy GC")
    ax.grid(alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(out, "wear_distribution.png"), dpi=140)
    plt.close(fig)

    # 3) Write-latency tail
    fig, ax = plt.subplots(figsize=(9, 4.2))
    labels = [f"{w}\n{g}" for w in workloads for g in ["greedy", "cost-benefit"]]
    for key, color in (("write_p50_us", "#00798c"), ("write_p99_us", "#edae49"), ("write_p999_us", "#d1495b")):
        vals = []
        for w in workloads:
            for g in ["greedy", "cost-benefit"]:
                r = next(r for r in rows if r["workload"] == w and r["gc"] == g and r["wear"] == "static")
                vals.append(float(r[key]))
        ax.plot(range(len(vals)), vals, marker="o", label=key.replace("write_", "").replace("_us", ""), color=color)
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels, fontsize=7)
    ax.set_yscale("log")
    ax.set_ylabel("write latency (us, simulated)")
    ax.set_title("Write latency percentiles (static wear levelling)")
    ax.grid(alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(out, "write_latency.png"), dpi=140)
    plt.close(fig)
    print("plots written to", out)


if __name__ == "__main__":
    main()
