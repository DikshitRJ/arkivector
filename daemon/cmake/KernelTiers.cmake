# KernelTiers.cmake: compile the pgvector kernel files once per x86-64 tier.
#
#   baseline  -march=x86-64      symbols unchanged (always valid fallback)
#   v3        -march=x86-64-v3   symbols suffixed _t3 (av_tier.h via -include)
#   v4        -march=x86-64-v4   symbols suffixed _t4
#
# The copied files stay byte-identical: renaming happens through the forced include.
# kernels/dispatch.c picks the best tier the CPU and OS support at startup.
# With AV_NATIVE=ON, or on non-x86-64 hosts, the kernels compile once.

set(AV_KERNEL_FILES
    src/pgvector/vector_kernels.c
    src/pgvector/halfvec_kernels.c
    src/pgvector/sparsevec_kernels.c)

set(AV_KERNEL_TIERS "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT AV_NATIVE)
  set(AV_KERNEL_TIERS t3 t4)
endif()

function(av_add_kernel_tiers target)
  # baseline
  target_sources(${target} PRIVATE ${AV_KERNEL_FILES})
  if(AV_KERNEL_TIERS)
    set_source_files_properties(${AV_KERNEL_FILES} TARGET_DIRECTORY ${target}
                                PROPERTIES COMPILE_OPTIONS "-march=x86-64")
  endif()
  foreach(tier ${AV_KERNEL_TIERS})
    if(tier STREQUAL "t3")
      set(_march x86-64-v3)
    else()
      set(_march x86-64-v4)
    endif()
    add_library(av_kernels_${tier} OBJECT ${AV_KERNEL_FILES})
    target_link_libraries(av_kernels_${tier} PRIVATE av_pgcompat)
    target_compile_options(av_kernels_${tier} PRIVATE ${AV_UPSTREAM_FLAGS} -march=${_march}
                           -include ${CMAKE_CURRENT_SOURCE_DIR}/src/pgvector/av_tier.h)
    target_compile_definitions(av_kernels_${tier} PRIVATE AV_TIER_SUFFIX=_${tier})
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:av_kernels_${tier}>)
  endforeach()
  if(AV_KERNEL_TIERS)
    target_compile_definitions(${target} PUBLIC AV_KERNEL_TIERS=1)
  endif()
endfunction()
