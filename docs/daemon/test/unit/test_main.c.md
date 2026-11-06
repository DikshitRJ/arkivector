# daemon/test/unit/test_main.c

## Purpose

Runner for the unit tests. It uses `greatest.h`. It starts each suite in order, and reports the result.

## Behavior

- The suites are `pgcompat`, `kernels`, `vectorbytes`, `text_io`, and `hnsw` when `AV_HAVE_HNSW_TEST` is defined.
- `av_unit -s <suite>` runs one suite. `test/CMakeLists.txt` registers one CTest test per suite.

## Testing

This file is the test entry point.
