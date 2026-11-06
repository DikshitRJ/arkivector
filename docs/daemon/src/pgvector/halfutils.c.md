# daemon/src/pgvector/halfutils.c

- **Upstream:** `pgvector/src/halfutils.c`, tag v0.8.7.
- **Mode:** V (verbatim, byte-identical).
- **ARKIVECTOR deltas:** none.
- **Purpose:** half-float (FP16) conversion, F16C kernels and CPUID checks.
- **Known quirk:** `halfutils.h:93` (`HalfToFloat4`) shifts a promoted value. UBSan `shift-base` is excluded in `cmake/Sanitizers.cmake`.
- **Testing:** `test/unit/test_kernels.c` (all 65536 half bit patterns round-trip).
