#!/usr/bin/env python3
"""Generate synthetic storage workloads (trace files) for nandsim_cli.

Trace format:  W <lpn> [pages] | R <lpn> [pages] | T <lpn> [pages] | RESET | # comment

Patterns
  seq_write    sequential overwrite passes over the whole logical space
  rand_write   uniform random writes
  hotcold      hot_prob of writes go to the first hot_frac of the LBA space (default 90/10)
  zipf         Zipf-distributed LBA popularity (exponent --zipf-s)
  mixed        uniform random with --read-ratio reads
  rand_read    uniform random reads (after the fill)

--fill first writes every LPN sequentially (preconditioning) and then emits RESET so only the
steady-state phase is measured.
"""
import argparse
import bisect
import random
import sys


def zipf_sampler(n, s, rng):
    weights = [1.0 / (i ** s) for i in range(1, n + 1)]
    cdf, acc = [], 0.0
    for w in weights:
        acc += w
        cdf.append(acc)
    total = cdf[-1]
    # shuffle rank->LBA so popular LBAs are scattered rather than clustered at 0
    perm = list(range(n))
    rng.shuffle(perm)
    return lambda: perm[min(bisect.bisect_left(cdf, rng.random() * total), n - 1)]


def generate(args, out):
    rng = random.Random(args.seed)
    n, req = args.lpns, args.req_pages
    slots = max(1, n // req)  # request-aligned start positions
    out.write(f"# pattern={args.pattern} lpns={n} ops={args.ops} req_pages={req} seed={args.seed}\n")

    if args.fill:
        chunk = 8
        for lpn in range(0, n, chunk):
            out.write(f"W {lpn} {min(chunk, n - lpn)}\n")
        out.write("RESET\n")

    if args.pattern == "zipf":
        pick_slot = zipf_sampler(slots, args.zipf_s, rng)
    hot_slots = max(1, int(slots * args.hot_frac))

    for i in range(args.ops):
        op = "W"
        if args.pattern == "seq_write":
            slot = i % slots
        elif args.pattern == "rand_write":
            slot = rng.randrange(slots)
        elif args.pattern == "hotcold":
            slot = rng.randrange(hot_slots) if rng.random() < args.hot_prob else \
                hot_slots + rng.randrange(max(1, slots - hot_slots))
        elif args.pattern == "zipf":
            slot = pick_slot()
        elif args.pattern == "mixed":
            slot = rng.randrange(slots)
            if rng.random() < args.read_ratio:
                op = "R"
        elif args.pattern == "rand_read":
            slot = rng.randrange(slots)
            op = "R"
        else:
            raise SystemExit(f"unknown pattern {args.pattern}")
        out.write(f"{op} {slot * req} {req}\n")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--pattern", required=True,
                   choices=["seq_write", "rand_write", "hotcold", "zipf", "mixed", "rand_read"])
    p.add_argument("--lpns", type=int, required=True, help="logical pages (see `nandsim_cli info`)")
    p.add_argument("--ops", type=int, default=100000, help="number of requests after the fill")
    p.add_argument("--req-pages", type=int, default=1, help="request size in pages")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--fill", action="store_true", help="precondition: sequential fill, then RESET")
    p.add_argument("--hot-frac", type=float, default=0.1)
    p.add_argument("--hot-prob", type=float, default=0.9)
    p.add_argument("--zipf-s", type=float, default=1.0)
    p.add_argument("--read-ratio", type=float, default=0.3)
    p.add_argument("-o", "--output", default="-")
    args = p.parse_args()
    if args.output == "-":
        generate(args, sys.stdout)
    else:
        with open(args.output, "w") as f:
            generate(args, f)


if __name__ == "__main__":
    main()
