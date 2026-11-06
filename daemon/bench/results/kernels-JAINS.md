# Kernel micro-benchmarks: host JAINS

Host: Intel i7-14650HX (AVX2, F16C, FMA, no AVX-512), Linux, cpufreq governor `powersave`
(measured about 3.7 GHz under load), release preset (gcc, -O2). Binary: `build/release/av_kernels_bench`.
Tiers present on this host: x86-64 and x86-64-v3 (x86-64-v4 not supported, so skipped).
Method: median of 7 repeats x 200k calls after a warm-up. `pool` = 8192 random vectors (about 25-100 MB, cache-cold, HNSW-like),
`hot` = 16 vectors (cache resident). All calls go through `av_kernel_fn_tier()` + `DirectFunctionCall2`.

## Results

active tier: x86-64-v3, highest supported: x86-64-v3, pool: 8192 vectors, iters: 200000 x 7 repeats (median)

| tier | type | dim | metric | pool ns/op | hot ns/op |
|---|---|---:|---|---:|---:|
| x86-64 | vector | 1536 | l2sq | 315.3 | 101.9 |
| x86-64 | vector | 1536 | neg_ip | 320.5 | 101.0 |
| x86-64 | vector | 1536 | cosine | 322.0 | 125.4 |
| x86-64-v3 | vector | 1536 | l2sq | 297.8 | 101.7 |
| x86-64-v3 | vector | 1536 | neg_ip | 315.2 | 100.2 |
| x86-64-v3 | vector | 1536 | cosine | 325.7 | 125.4 |
| x86-64 | halfvec | 1536 | l2sq | 251.7 | 218.1 |
| x86-64 | halfvec | 1536 | neg_ip | 232.2 | 200.5 |
| x86-64 | halfvec | 1536 | cosine | 266.1 | 238.5 |
| x86-64-v3 | halfvec | 1536 | l2sq | 243.6 | 219.7 |
| x86-64-v3 | halfvec | 1536 | neg_ip | 229.0 | 200.2 |
| x86-64-v3 | halfvec | 1536 | cosine | 305.5 | 239.6 |
| x86-64 | vector | 3072 | l2sq | 679.6 | 203.0 |
| x86-64 | vector | 3072 | neg_ip | 655.1 | 201.5 |
| x86-64 | vector | 3072 | cosine | 682.8 | 241.4 |
| x86-64-v3 | vector | 3072 | l2sq | 653.2 | 203.0 |
| x86-64-v3 | vector | 3072 | neg_ip | 660.0 | 201.4 |
| x86-64-v3 | vector | 3072 | cosine | 667.0 | 241.4 |
| x86-64 | halfvec | 3072 | l2sq | 539.4 | 468.5 |
| x86-64 | halfvec | 3072 | neg_ip | 528.7 | 453.3 |
| x86-64 | halfvec | 3072 | cosine | 568.0 | 492.8 |
| x86-64-v3 | halfvec | 3072 | l2sq | 541.7 | 468.5 |
| x86-64-v3 | halfvec | 3072 | neg_ip | 519.3 | 452.5 |
| x86-64-v3 | halfvec | 3072 | cosine | 559.5 | 492.8 |

| call path (hot, tier x86-64-v3) | ns/op |
|---|---:|
| no-op via DirectFunctionCall2 | 2.7 |
| halfvec 1536 l2sq via DirectFunctionCall2 | 220.0 |
| halfvec 1536 l2sq via prebuilt fcinfo | 218.5 |
| halfvec 1536 l2sq raw kernel pointer (HalfvecL2SquaredDistance) | 219.3 |
| halfvec 1536 l2sq PROTOTYPE 4-accumulator F16C (bench-only) | 78.6 |

## Plan target (PLAN_DAEMON.md 5.5): halfvec 1536-d on x86-64-v3 <= 150 ns/op

**MISSED.** halfvec 1536 l2sq: 220 ns hot, about 249 ns pool; neg_ip 200 / 229; cosine 239 / 261.
(vector 1536 float is 101 ns hot but 290 ns pool: it is memory-bound once the working set leaves cache.)

## Where the time goes

- fmgr / DirectFunctionCall2 is NOT the problem: a no-op call through DirectFunctionCall2 costs 2.7 ns;
  DirectFunctionCall2 vs a pre-built fcinfo vs the raw `HalfvecL2SquaredDistance` pointer are 220.0 / 218.5 / 219.3 ns (noise-level).
- The kernel is the F16C path in `src/pgvector/halfutils.c` (`HalfvecL2SquaredDistanceF16c`, selected at runtime by `HalfvecInit`;
  confirmed in the disassembly: `vcvtph2ps` + `vfmadd231ps`). It uses ONE accumulator, so the loop is bound by
  FMA latency: 192 iterations x about 4.2 cycles = about 806 cycles at 3.7 GHz = 218 ns. Throughput would allow about 1.5-2 cycles/iteration.
- halfutils.c is compiled once (it is not one of the tiered files), so halfvec timings are identical for x86-64 and x86-64-v3.
  Tiering only helps nothing here for halfvec; for vector (float) the baseline and v3 tiers also tie (101 vs 102 ns hot),
  i.e. pgvector float kernels gain nothing from -march=x86-64-v3 on this CPU with this compiler (loop is load/latency bound).
- Pool vs hot: float vectors lose about 2.9x to cache misses (6 KB per vector); halfvec loses only 10-15% (3 KB per vector, compute-bound).

## Experiment (bench-only prototype, not production)

Same F16C l2sq with 4 independent accumulators (`proto_l2sq_4acc` in kernels_bench.c): **78.6 ns** hot (2.8x faster, meets the target
with margin). Result differs from pgvector in the last bits (different summation order), so it needs the parity tolerance rather than bit-exactness.

## Recommendations

1. Do not spend effort on fmgr/DirectFunctionCall2: it costs under 3 ns per call.
2. To meet the 150 ns target for halfvec: unroll to 2-4 accumulators in the halfvec L2/IP/cosine F16C kernels. Options:
   (a) an `ARKIVECTOR`-marked delta in halfutils.c (allowed by AGENTS.md 6.1, PROVENANCE mode must change from verbatim), or
   (b) an Arkivector-owned kernel in av_core, installed over the `Halfvec*` function pointers after `HalfvecInit()` (no pgvector edits).
   Check recall/parity tests with a float tolerance, since summation order changes.
3. Float `vector` at 1536/3072 is memory-bound in a cold pool (290/630 ns): rely on index locality, prefetch (pgvector HNSW already prefetches
   neighbours), or store halfvec in the index if recall allows (about 15% faster cold, 2x less memory).
4. Re-run on a x86-64-v4 host to see whether the v4 tier changes anything (not measurable here). Set the governor to `performance` for stable numbers.
