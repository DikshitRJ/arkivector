# Warnings.cmake: compiler flag sets for Arkivector code vs copied upstream code.
#
# av_strict(<target>)   our code: -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
# av_upstream(<target>) copied pgvector / PostgreSQL code: pgvector's Makefile flags, no -Werror
#
# Copied code must compile unmodified, so it never gets our strict set (ARCHITECTURE §5.4).

set(AV_STRICT_FLAGS
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
    -Wformat=2 -Wno-unused-parameter)

# pgvector Makefile: PG_CFLAGS += $(OPTFLAGS) -ftree-vectorize -fassociative-math
#                    -fno-signed-zeros -fno-trapping-math -ffp-contract=fast
set(AV_UPSTREAM_FLAGS
    -ftree-vectorize -fassociative-math -fno-signed-zeros -fno-trapping-math -ffp-contract=fast
    -fno-strict-aliasing -fwrapv
    -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers)

function(av_strict target)
  target_compile_options(${target} PRIVATE ${AV_STRICT_FLAGS})
endfunction()

function(av_upstream target)
  target_compile_options(${target} PRIVATE ${AV_UPSTREAM_FLAGS})
endfunction()

# Upstream flags for individual source files inside an otherwise strict target.
function(av_upstream_sources)
  set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS "${AV_UPSTREAM_FLAGS}")
endfunction()
