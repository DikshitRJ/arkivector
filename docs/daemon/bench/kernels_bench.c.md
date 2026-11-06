# daemon/bench/kernels_bench.c

## Purpose

Micro-benchmark of the pgvector kernels, called the way the daemon calls them. That is, through `DirectFunctionCall2` as a `PGFunction`.

## Usage

```
av_kernels_bench [--pairs N] [--repeats R] [--iters I]
```

## What it measures

- Each tier, type, metric and dimension in the table. Dimensions are 1536 and 3072.
- Two working sets. `pool` is a large set of random vectors. `hot` is 16 vectors that fit in cache.
- Call paths: a no-op function, `DirectFunctionCall2`, a prebuilt `fcinfo`, and the raw kernel pointer.
- A prototype four-accumulator F16C kernel. It is for measurement only. It is not used in the daemon.

Each number is the median over the repeats.

## Ownership and thread safety

Single-threaded benchmark. It creates one memory context.

## Testing

Built when `AV_BUILD_BENCH` is ON. Results are recorded in `src/pgvector/PROVENANCE.md`.
