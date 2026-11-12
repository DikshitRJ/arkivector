# Sanitizers.cmake: AV_ASAN, AV_UBSAN, AV_TSAN, AV_FUZZ build options (applied globally).

option(AV_ASAN "Build with AddressSanitizer" OFF)
option(AV_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(AV_TSAN "Build with ThreadSanitizer" OFF)
option(AV_FUZZ "Build libFuzzer targets (clang only)" OFF)

if(AV_ASAN AND AV_TSAN)
  message(FATAL_ERROR "AV_ASAN and AV_TSAN are mutually exclusive")
endif()

set(_av_san_flags "")
if(AV_ASAN)
  list(APPEND _av_san_flags -fsanitize=address -fno-omit-frame-pointer)
endif()
if(AV_UBSAN)
  # pgvector relies on -fwrapv-style arithmetic in a few places; keep UBSan strict but recoverable off.
  list(APPEND _av_san_flags -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  # pgvector v0.8.7 halfutils.h:93 (HalfToFloat4, inline in a header) left-shifts a
  # promoted uint16 by 16 ("32768 << 16" overflows int). Upstream code we may not edit;
  # the result is well defined on gcc/clang. Exclude only the shift-base check.
  list(APPEND _av_san_flags -fno-sanitize=shift-base)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    # pgvector's copied kernels intentionally use unaligned-friendly casts; alignment is checked in our code.
    list(APPEND _av_san_flags -fno-sanitize=function)
  endif()
endif()
if(AV_TSAN)
  list(APPEND _av_san_flags -fsanitize=thread -fno-omit-frame-pointer)
  # pgvector's target_clones kernels (vector_kernels.c, bitutils.c) compile to GNU ifuncs. glibc runs
  # ifunc resolvers during relocation, before the TSan runtime is initialised; TSan instruments the
  # resolver (__tsan_func_entry) and every TSan binary segfaults before main(). pgvector's own
  # DISABLE_DISPATCH switch (halfvec.h) turns dispatch off, so TSan builds use the portable kernels.
  add_compile_definitions(DISABLE_DISPATCH)
endif()

if(_av_san_flags)
  add_compile_options(${_av_san_flags})
  add_link_options(${_av_san_flags})
endif()

if(AV_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "AV_FUZZ requires clang")
  endif()
  # Libraries get coverage instrumentation; fuzz executables link -fsanitize=fuzzer.
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()
