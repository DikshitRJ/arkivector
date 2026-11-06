# daemon/test/unit/test_kernels.c

## Purpose

Tests the pgvector kernels, the half-float conversion, the bit kernels, the opclass registry, and the text limits.

## Suite `kernels`

| Test | Checks |
| :--- | :--- |
| `hardening_limits` | Opclass distance on mismatched dimensions returns NaN. A NUL byte in a literal is rejected. A 64001-bit literal is rejected, and a 64000-bit literal is accepted. |
| `tiered_float_kernels_match_reference` | Every available tier, for vector, halfvec and sparsevec, against a double reference. Dimensions 1 to 16000. Tolerance 1e-5 for vector, 2e-3 for halfvec. |
| `bit_kernels_exact` | Hamming and Jaccard against exact counts. |
| `opclass_registry_matches_pgvector` | 14 classes, operators, normalize flag, index metrics, 3-4-5 distance. |
| `half_roundtrip_all_patterns` | All non-NaN half bit patterns round-trip through float. |
| `half_known_values` | A few known conversions. |
| `bit_hamming_and_jaccard_vs_scalar` | 500 random bit strings against a scalar reference. |
| `halfvec_l2_default_matches_scalar` | Default halfvec L2 matches a scalar sum. |

## Testing

Run with `av_unit -s kernels`, or `ctest -R unit_kernels`.
