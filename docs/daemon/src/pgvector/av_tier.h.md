# daemon/src/pgvector/av_tier.h

- **Upstream:** none. Arkivector-owned.
- **Purpose:** force-included into each kernel tier build (`-include`). It renames the exported kernel symbols with the tier suffix (`_t3` or `_t4`).
- **Used by:** `cmake/KernelTiers.cmake`.
- **Testing:** `test/unit/test_kernels.c` checks every tier.
