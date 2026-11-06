cmake_minimum_required(VERSION 3.20)
project(arkivector VERSION 0.1.0 LANGUAGES C)

# ---------------------------------------------------------------------------
# Arkivector daemon (arkivectord). See PLAN_DAEMON.md and ARCHITECTURE.md §16.
# Targets:
#   av_pgcompat  PostgreSQL API shims + verbatim PostgreSQL helper files (static)
#   av_pgvector  code copied from pgvector v0.8.7, upstream flags (static)
#   av_core      Arkivector code, strict warnings (static)
#   arkivectord  the daemon executable
#   av_unit      unit tests (test/CMakeLists.txt)
# ---------------------------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # gnu11: pgvector uses GNU attributes/intrinsics
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

option(AV_NATIVE "Compile kernels once with -march=native (pgvector default)" OFF)
option(AV_BUILD_TESTS "Build unit tests" ON)
option(AV_BUILD_BENCH "Build micro-benchmarks (av_kernels_bench)" ON)

include(Warnings)
include(Sanitizers)
include(CheckIncludeFile)
include(CheckCSourceCompiles)

find_package(Threads REQUIRED)

set(AV_THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
include(Vendored)

set(AV_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_filename_component(AV_REPO_ROOT ${AV_REPO_ROOT} ABSOLUTE)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# pg_config.h (generated): the configure-time facts copied code checks for.
# ---------------------------------------------------------------------------
check_include_file(cpuid.h HAVE__GET_CPUID_H)
check_c_source_compiles("
#include <cpuid.h>
int main(void) { unsigned int a, b, c, d; __get_cpuid(1, &a, &b, &c, &d); return 0; }"
  HAVE__GET_CPUID)
check_c_source_compiles("
#include <cpuid.h>
int main(void) { unsigned int a, b, c, d; __get_cpuid_count(7, 0, &a, &b, &c, &d); return 0; }"
  HAVE__GET_CPUID_COUNT)
check_c_source_compiles("
#include <immintrin.h>
__attribute__((target(\"xsave\"))) int f(void) { return (int) _xgetbv(0); }
int main(void) { return f(); }"
  HAVE_XSAVE_INTRINSICS)
check_c_source_compiles("
int main(void) { return __builtin_popcount(3u) + __builtin_ctz(2u) + __builtin_clz(2u); }"
  HAVE__BUILTIN_POPCOUNT)
set(HAVE__BUILTIN_CTZ ${HAVE__BUILTIN_POPCOUNT})
set(HAVE__BUILTIN_CLZ ${HAVE__BUILTIN_POPCOUNT})
configure_file(src/pgcompat/include/pg_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/pg_config.h)

# ---------------------------------------------------------------------------
# av_base: allocator shared by pgcompat (palloc) and our code (strict)
# ---------------------------------------------------------------------------
add_library(av_base STATIC src/core/arena.c)
target_include_directories(av_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
av_strict(av_base)

# ---------------------------------------------------------------------------
# av_pgcompat
# ---------------------------------------------------------------------------
set(AV_PGCOMPAT_OURS
    src/pgcompat/src/palloc.c
    src/pgcompat/src/elog.c
    src/pgcompat/src/fmgr.c
    src/pgcompat/src/lwlock.c
    src/pgcompat/src/misc.c)
set(AV_PGCOMPAT_VERBATIM
    src/pgcompat/src/list.c
    src/pgcompat/src/pairingheap.c
    src/pgcompat/src/pg_prng.c
    src/pgcompat/src/pg_bitutils.c
    src/pgcompat/src/f2s.c
    src/pgcompat/src/varbit_io.c)
foreach(f ${AV_PGCOMPAT_VERBATIM})
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f})
    list(REMOVE_ITEM AV_PGCOMPAT_VERBATIM ${f})
  endif()
endforeach()
foreach(f ${AV_PGCOMPAT_OURS})
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f})
    list(REMOVE_ITEM AV_PGCOMPAT_OURS ${f})
  endif()
endforeach()

add_library(av_pgcompat STATIC ${AV_PGCOMPAT_OURS} ${AV_PGCOMPAT_VERBATIM})
# Copied/shim headers are SYSTEM for consumers so our strict warnings skip them.
target_include_directories(av_pgcompat SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pgcompat/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pgvector)
target_include_directories(av_pgcompat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(av_pgcompat PUBLIC ARKIVECTOR=1)
target_link_libraries(av_pgcompat PUBLIC av_base Threads::Threads m)
target_compile_options(av_pgcompat PRIVATE -Wall -Wextra -Wno-unused-parameter)
av_upstream_sources(${AV_PGCOMPAT_VERBATIM})

# ---------------------------------------------------------------------------
# av_pgvector: copied pgvector v0.8.7 code (see src/pgvector/PROVENANCE.md)
# ---------------------------------------------------------------------------
set(AV_PGVECTOR_SOURCES
    src/pgvector/halfutils.c
    src/pgvector/bitutils.c
    src/pgvector/bitvec_kernels.c
    src/pgvector/vector_io.c
    src/pgvector/vector_funcs.c
    src/pgvector/halfvec_io.c
    src/pgvector/halfvec_funcs.c
    src/pgvector/sparsevec_io.c
    src/pgvector/sparsevec_funcs.c)
set(AV_PGVECTOR_OPTIONAL
    src/pgvector/hnswutils.c
    src/pgvector/hnswbuild_mem.c
    src/pgvector/av_hnsw_glue.c)
foreach(f ${AV_PGVECTOR_OPTIONAL})
  file(SIZE ${CMAKE_CURRENT_SOURCE_DIR}/${f} _sz)
  if(_sz GREATER 0)
    list(APPEND AV_PGVECTOR_SOURCES ${f})
  endif()
endforeach()

add_library(av_pgvector STATIC ${AV_PGVECTOR_SOURCES})
target_link_libraries(av_pgvector PUBLIC av_pgcompat)
av_upstream(av_pgvector)
include(KernelTiers)
av_add_kernel_tiers(av_pgvector)
if(AV_NATIVE)
  target_compile_options(av_pgvector PRIVATE -march=native)
endif()

# ---------------------------------------------------------------------------
# av_core: Arkivector code (strict warnings)
# ---------------------------------------------------------------------------
set(AV_CORE_SOURCES
    src/version.c
    src/log.c
    src/codec/vtype.c
    src/kernels/dispatch.c
    src/kernels/opclass.c
    src/codec/text.c
    src/codec/vectorbytes.c
    src/codec/hex.c
    src/codec/base64.c
    src/sync/ttl.c
    src/sync/syncer.c
    src/arkiv/rlp.c
    src/arkiv/keccak.c
    src/arkiv/entity.c
    src/arkiv/logs.c
    src/arkiv/budget.c
    src/arkiv/query_builder.c
    src/arkiv/rpc.c
    src/arkiv/ws.c
    src/store/lmdb_store.c
    src/store/arena_file.c
    src/store/checkpoint.c
    src/index/docid.c
    src/index/live.c
    src/index/attr_index.c
    src/index/segment.c
    src/core/coldef.c
    src/core/collection.c
    src/core/registry.c
    src/filter/value.c
    src/filter/ast.c
    src/filter/lexer.c
    src/filter/parser.c
    src/filter/json_filter.c
    # D2 Lane A: runtime and HTTP server
    src/config.c
    src/core/threadpool.c
    src/core/mpsc.c
    src/server/http.c
    src/server/router.c
    src/server/json_io.c
    src/server/problem.c
    src/server/metrics.c
    src/server/rest_ops.c
    src/filter/eval.c
    src/index/flat.c
    src/query/planner.c
    src/query/executor.c
    src/server/rest_collections.c
    src/server/rest_entities.c
    src/server/rest_search.c
    src/pgvector/hnswscan_mem.c
    src/index/compact.c
    src/query/calibrate.c
    src/arkiv/abi.c
    src/arkiv/tx.c
    src/arkiv/signer.c
    src/write/nonce.c
    src/write/batcher.c
    src/write/writer.c
    src/server/idempotency.c
    src/server/auth.c
    src/server/ratelimit.c
    src/server/qdrant.c
    src/server/mcp.c
    src/sync/discovery.c
    src/core/fileio.c
    src/filter/value_json.c)

# Build-time generated headers (no runtime JSON reading, no runtime licence file lookup).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(AV_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
  OUTPUT ${AV_GEN_DIR}/av_error_table.h
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_error_table.py
          ${AV_REPO_ROOT}/spec/error-codes.json ${AV_GEN_DIR}/av_error_table.h
  DEPENDS ${AV_REPO_ROOT}/spec/error-codes.json ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_error_table.py
  COMMENT "Generating av_error_table.h from spec/error-codes.json"
  VERBATIM)
add_custom_command(
  OUTPUT ${AV_GEN_DIR}/av_licenses.h
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_licenses.py
          ${CMAKE_CURRENT_SOURCE_DIR}/src/pgvector/LICENSE.pgvector
          ${CMAKE_CURRENT_SOURCE_DIR}/src/pgcompat/LICENSE.postgresql ${AV_GEN_DIR}/av_licenses.h
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/pgvector/LICENSE.pgvector
          ${CMAKE_CURRENT_SOURCE_DIR}/src/pgcompat/LICENSE.postgresql
          ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_licenses.py
  COMMENT "Generating av_licenses.h"
  VERBATIM)

add_library(av_core STATIC ${AV_CORE_SOURCES} ${AV_GEN_DIR}/av_error_table.h)
target_include_directories(av_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${AV_GEN_DIR})
target_link_libraries(av_core PUBLIC av_pgvector av_pgcompat av3p_curl av3p_keccak av3p_yyjson av3p_lmdb av3p_roaring av3p_uv av3p_llhttp av3p_tomlc99 av3p_secp256k1 av3p_openssl Threads::Threads m)
target_compile_definitions(av_core PUBLIC AV_REPO_ROOT="${AV_REPO_ROOT}")
set(AV_VERSION "" CACHE STRING "Daemon version string for release builds (default: version.h)")
if(AV_VERSION)
  target_compile_definitions(av_core PUBLIC AV_DAEMON_VERSION="${AV_VERSION}")
endif()
av_strict(av_core)
av_upstream_sources(src/pgvector/hnswscan_mem.c)

# ---------------------------------------------------------------------------
# arkivectord
# ---------------------------------------------------------------------------
if(AV_BUILD_BENCH)
  add_executable(av_kernels_bench bench/kernels_bench.c)
  target_link_libraries(av_kernels_bench PRIVATE av_core)
endif()

add_executable(arkivectord src/main.c ${AV_GEN_DIR}/av_licenses.h)
target_link_libraries(arkivectord PRIVATE av_core)
av_strict(arkivectord)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(AV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
