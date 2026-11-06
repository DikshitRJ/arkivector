# Unit tests (greatest.h). One executable, av_unit; ctest runs each suite as a test.
set(AV_UNIT_SOURCES
    unit/test_main.c
    unit/test_pgcompat.c
    unit/test_kernels.c
    unit/test_vectorbytes.c
    unit/test_text_io.c
    unit/test_ttl.c
    unit/test_value.c
    unit/test_filter_parser.c)
set(AV_UNIT_SUITES pgcompat kernels vectorbytes text_io ttl value filter_parser)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_hnsw.c)
  file(SIZE ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_hnsw.c _hnsw_sz)
  if(_hnsw_sz GREATER 0)
    list(APPEND AV_UNIT_SOURCES unit/test_hnsw.c)
    list(APPEND AV_UNIT_SUITES hnsw)
    set(AV_HAVE_HNSW_TEST ON)
  endif()
endif()

add_executable(av_unit ${AV_UNIT_SOURCES})
target_link_libraries(av_unit PRIVATE av_core av3p_greatest av3p_yyjson)
target_compile_options(av_unit PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)
if(AV_HAVE_HNSW_TEST)
  target_compile_definitions(av_unit PRIVATE AV_HAVE_HNSW_TEST=1)
endif()

foreach(suite ${AV_UNIT_SUITES})
  add_test(NAME unit_${suite} COMMAND av_unit -s ${suite})
  set_tests_properties(unit_${suite} PROPERTIES TIMEOUT 600)
endforeach()

# libFuzzer targets (clang, -DAV_FUZZ=ON)
if(AV_FUZZ)
  add_executable(fuzz_vectorbytes fuzz/fuzz_vectorbytes.c)
  target_link_libraries(fuzz_vectorbytes PRIVATE av_core)
  target_compile_options(fuzz_vectorbytes PRIVATE -fsanitize=fuzzer-no-link)
  target_link_options(fuzz_vectorbytes PRIVATE -fsanitize=fuzzer)
  add_executable(fuzz_vector_in fuzz/fuzz_vector_in.c)
  target_link_libraries(fuzz_vector_in PRIVATE av_core)
  target_compile_options(fuzz_vector_in PRIVATE -fsanitize=fuzzer-no-link)
  target_link_options(fuzz_vector_in PRIVATE -fsanitize=fuzzer)
  foreach(_fz fuzz_filter_parser fuzz_json_filter)
    add_executable(${_fz} fuzz/${_fz}.c)
    target_link_libraries(${_fz} PRIVATE av_core av3p_yyjson)
    target_compile_options(${_fz} PRIVATE -fsanitize=fuzzer-no-link)
    target_link_options(${_fz} PRIVATE -fsanitize=fuzzer)
  endforeach()
endif()

# pgvector parity helper (used by test/parity/*.py; see test/parity/README.md)
add_executable(av_parity_cli parity/av_parity_cli.c)
target_link_libraries(av_parity_cli PRIVATE av_core av3p_yyjson)
target_compile_options(av_parity_cli PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)
