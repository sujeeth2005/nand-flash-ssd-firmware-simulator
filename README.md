# nandsim — NAND Flash & SSD Firmware Simulator

A from-scratch C++17 model of the core logic inside an SSD controller, plus a Python validation and
benchmarking toolkit. No hardware needed. It models raw NAND (with the real physical rules and
failure modes) and a page-mapped **Flash Translation Layer** with garbage collection, wear
levelling and **power-loss recovery**, then attacks it with fault injection.

```
host reads/writes/trims ─► FTL (L2P map · GC · wear levelling · recovery) ─► NAND model
                                                                  (program-order rules · erase blocks ·
                                                                   wear-out · bad blocks · torn writes ·
                                                                   ECC · simulated time)
```

## Quick start

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure           # 60 tests in 7 suites

python3 tools/run_tests.py     --tests build/nandsim_tests            # Markdown/JSON test report
python3 tools/crash_campaign.py --cli build/nandsim_cli --seeds 200    # power-loss fault injection
python3 tools/run_bench.py     --cli build/nandsim_cli --out results   # benchmark matrix -> results/bench.csv
python3 tools/plot.py          --results results                       # PNG plots

# sanitizers (GCC/Clang):  cmake -B build-san -DCMAKE_BUILD_TYPE=Debug -DNANDSIM_SANITIZE=ON
# manual run:
python3 tools/gen_workload.py --pattern hotcold --lpns 7168 --ops 100000 --fill -o /tmp/w.txt
build/nandsim_cli run --workload /tmp/w.txt --gc cost-benefit --wear static --blocks 128 --page-size 4096
```
Needs: a C++17 compiler, CMake ≥ 3.16, Python 3.8+ (matplotlib only for `plot.py`). No third-party C++ deps
(a tiny built-in test harness is used instead of GoogleTest). Works in VS Code on Windows with the
CMake Tools extension (MSVC) — the CI includes a Windows job, but I could only verify Linux/GCC locally.

## What is implemented

| Area | Details |
|---|---|
| **NAND model** (`flash_model`) | pages/blocks; program only erased pages, strictly sequential inside a block; erase per block; P/E-cycle wear-out → bad block; factory bad blocks; injected program/erase failures; raw bit errors with an ECC correction threshold; per-op simulated latency; OOB area per page |
| **Power loss** | cut power at the Nth program/erase: either *lost* (nothing happened) or *torn* (half-written page / half-erased block that never passes ECC) |
| **FTL** (`ftl`) | page-level L2P map, reverse map, per-block valid counts, over-provisioning, TRIM, separate host and GC write streams (hot/cold separation), write retry on program failure, bad-block retirement, graceful `ErrNoSpace` when the device wears out |
| **Garbage collection** (`gc`) | free-block watermarks; **greedy** (fewest valid pages) and **cost-benefit** (`(1-u)/(2u)·age`) victim selection; destination-room check so GC can never wedge |
| **Wear levelling** (`wear`) | `none` (lowest block index first), `dynamic` (least-worn free block for new writes), `static` (dynamic + migrates *cold* data onto worn blocks, in its own block so it is not dragged around by GC) |
| **Recovery** | every page's OOB holds `{lpn, sequence number, stream}`. `mount()` rescans flash, newest sequence wins, drops torn pages, re-erases dead blocks, resumes the open host/GC blocks. Works from a fresh device and after a crash — even a crash *during* mount |
| **Self-check** | `Ftl::check_invariants()` cross-checks L2P ↔ P2L ↔ per-block counts ↔ free list ↔ physical flash state; used by tests and the crash harness |
| **Validation** | 60 unit/integration tests; randomized differential fuzzing against a reference model; exhaustive crash-point sweep; randomized crash campaign |
| **Benchmarking** | trace-driven runner (`W/R/T lpn pages`, `RESET`), data verification on every read, throughput, p50/p99/p99.9 latency, write amplification, erase-count distribution |

## Results (measured; `results/bench.csv`, `reports/`)

Device: 128 blocks × 64 pages × 4 KiB, 12.5 % over-provisioning, simulated timing 60 µs read / 600 µs program /
3.5 ms erase. Each workload: sequential preconditioning fill, then 150 000 measured writes. Numbers are
properties of this *model*, not of any real drive.

**Garbage-collection policy** — write amplification (WA = flash page programs / host page writes), dynamic wear levelling:

| workload | greedy | cost-benefit |
|---|---|---|
| uniform random writes | 4.69 | 4.75 |
| Zipf (s = 1) | 4.32 | **3.72** |
| hot/cold 90 % of writes to 10 % of LBAs | 4.39 | **3.07** |
| sequential overwrite | 1.00 | 1.00 |

Cost-benefit wins when data has temperature skew (−30 % WA on hot/cold) and is a wash on uniform traffic.

**Wear levelling** — erase-count spread on hot/cold, greedy GC (`results/plots/wear_distribution.png`):

| mode | min…max erases | max−min | WA |
|---|---|---|---|
| none | 0…269 | 269 | 4.40 |
| dynamic | 31…194 | 163 | 4.39 |
| static | 76…124 | 48 | 4.54 |

Static levelling shrinks the spread ~5.6× for ~3 % extra write amplification: cold data parked in a
never-erased block would otherwise leave that block at 0 cycles while hot blocks wear out. (An early
version of static levelling cost +144 % WA in a unit-test scenario because migrated cold data shared a block with GC
remnants of hot data and GC kept moving it; fixed by giving migrations their own block — see `wear.cpp`.)

**Reliability**
* Exhaustive crash-point sweep: power cut at **every** flash program/erase of a GC-heavy scenario, in
  both *lost* and *torn* modes, for all 6 GC×wear configurations → **10 236 crash points, 0 failures**.
* Randomized campaign: 120 seeded runs, **10 761 injected power losses (+38 during recovery itself), 0 failures**.
  After each crash every acknowledged write must read back exactly; the single in-flight write must be old *or* new
  data, never garbage.
* All 60 tests pass under AddressSanitizer + UBSan with `-Werror`.

## Bugs the validation found (and that are fixed)

Worth knowing because they are the interesting interview stories:
1. **GC deadlock after a crash.** Crash fuzzing hit a state with *zero free blocks and no open GC block*, so GC had nowhere to
   copy data. Fixes: the host stream may never take the last 2 free blocks; OOB carries the stream id so `mount()` resumes the
   interrupted GC block instead of mistaking it for a host block; victim selection only considers blocks that fit the
   available destination space.
2. **Static wear-levelling write-amplification blow-up** (above).
3. **Wasted/leaked partial blocks** at mount — closing the two open blocks lost their free tails; now they are resumed.

## Design notes / likely interview questions

* **Why page-level mapping?** Best write amplification and no read-modify-write; cost is DRAM: here 4 B × logical pages
  (≈ 0.1 % of capacity for 4 KiB pages) — block-level/hybrid mapping trades that for slower random writes.
* **Why sequence numbers in OOB?** The L2P table lives in volatile DRAM; after power loss the newest copy of every LBA is
  recovered by scanning, no journal required. Cost: mount time is O(pages) (`RecoveryReport::scan_time_ns`).
* **Why can't GC just erase?** NAND erases whole blocks but programs single pages, so valid pages must be relocated first
  — that relocation *is* write amplification.
* **Over-provisioning vs WA:** `tests/test_gc_wear.cpp` shows WA falling as OP rises (10 % → 30 %).
* **Hot/cold separation:** host and GC data use separate open blocks; GC survivors are by definition colder.

## Known limitations (deliberate)

* **TRIM is not persistent** across power loss (a trimmed LBA may reappear with old data) — as permitted by many real drives. The
  crash harness therefore does not trim.
* One channel/die, no parallelism, **no queue-depth model** (request size is supported through multi-page requests). Latency is
  simulated flash busy time; GC runs in the foreground, so p99.9 latency shows GC stalls (~370 ms) that a real drive hides with
  background GC and parallelism.
* No data-path ECC *content* (an ECC threshold model decides correctable vs. uncorrectable), no read-disturb/retention modelling,
  no write cache / flush semantics, no multi-plane or SLC-cache modelling.
* An unreadable (uncorrectable) newest page found during mount silently falls back to an older copy if one still exists.
* After `ErrPowerLoss` the `Ftl` object must be discarded and a new one `mount()`ed (that is what a reboot is).

## Layout

```
include/nandsim/   flash_model.hpp ftl.hpp workload.hpp crash_harness.hpp types.hpp util.hpp
src/               flash_model.cpp ftl.cpp gc.cpp wear.cpp workload.cpp crash_harness.cpp main.cpp (CLI)
tests/             test_framework.hpp + flash / ftl / gc+wear / recovery / fuzz / workload suites
tools/             gen_workload.py run_bench.py plot.py run_tests.py crash_campaign.py
bench/baseline.csv quick-config baseline used by the CI performance-regression gate
results/           bench.csv, per-run JSON, plots/*.png from a full run
.github/workflows/ci.yml   Linux release + sanitizer, Windows, campaign, regression gate
```

## CLI

```
nandsim_cli info       [device opts]
nandsim_cli run        --workload FILE [device opts] [--json out.json] [--no-verify]
nandsim_cli crashfuzz  --seed N [--ops N] [--crash-interval N] [device opts]
nandsim_cli crashsweep [--ops N] [--stride N] [device opts]
device opts: --blocks --pages-per-block --page-size --op --gc greedy|cost-benefit --wear none|dynamic|static
             --wl-threshold --max-pe --bit-error-prob --ecc-bits --seed
```

## Resume bullets (use only the numbers you reproduce yourself)

> **NAND Flash & SSD Firmware Simulator** | C++17, Python, CMake
> * Built a page-mapped Flash Translation Layer over a NAND model that enforces erase-before-program and sequential-program rules,
>   with greedy and cost-benefit garbage collection and dynamic/static wear levelling; cost-benefit cut write amplification 30 %
>   on hot/cold workloads (4.39 → 3.07) and static levelling reduced erase-count spread 5.6× (269 → 48).
> * Implemented OOB-sequence-number power-loss recovery and validated it with fault injection: 10 000+ exhaustive crash points
>   (torn programs/erases, crashes during GC and during recovery) and 10 000+ randomized power cuts with zero data-integrity failures.
> * Wrote 60 C++ tests (differential fuzzing, invariant checker, ASan/UBSan) plus Python workload generators, a benchmark/plotting
>   pipeline and a CI performance-regression gate; fuzzing uncovered and I fixed a post-crash GC deadlock.
