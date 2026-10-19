include(GoogleTest)

# Header-only until support/*.cc files appear (Task 18), then a static library.
file(GLOB SUPPORT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/support/*.cc)
if(SUPPORT_SOURCES)
  add_library(test_support STATIC ${SUPPORT_SOURCES})
  target_include_directories(test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(test_support PUBLIC tideskv_core GTest::gtest)
  tideskv_defaults(test_support)
  # TestServer passes these to every child server's TSAN_OPTIONS (R17); public so tests can check it.
  target_compile_definitions(test_support PUBLIC
    TIDESKV_TSAN_SUPPRESSIONS="${CMAKE_CURRENT_SOURCE_DIR}/sanitizers/tsan.supp")
else()
  add_library(test_support INTERFACE)
  target_include_directories(test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(test_support INTERFACE tideskv_core GTest::gtest)
endif()

file(GLOB UNIT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc)
add_executable(unit_tests ${UNIT_SOURCES})
target_link_libraries(unit_tests PRIVATE test_support tidesdb tideskv_lua GTest::gtest_main rapidcheck rapidcheck_gtest)
tideskv_defaults(unit_tests)
gtest_discover_tests(unit_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 60)

file(GLOB CONTRACT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/contract/*.cc)
if(CONTRACT_SOURCES)
  add_executable(contract_tests ${CONTRACT_SOURCES})
  # tidesdb directly: some contract tests set up on-disk state an older TidesKV would have left
  target_link_libraries(contract_tests PRIVATE test_support tidesdb GTest::gtest_main)
  tideskv_defaults(contract_tests)
  if(TIDESKV_COMPRESSION)
    target_compile_definitions(contract_tests PRIVATE TIDESKV_TEST_COMPRESSION)
  endif()
  gtest_discover_tests(contract_tests DISCOVERY_MODE PRE_TEST)
endif()

# TidesDB's own planner tests, run against the patched planner (cmake/patches): they fail if the patch
# stops applying or a TidesDB bump changes the behaviour it fixes.
add_executable(tidesdb_planner_tests ${tidesdb_SOURCE_DIR}/test/compaction_planner__tests.c)
target_include_directories(tidesdb_planner_tests PRIVATE ${tidesdb_SOURCE_DIR}/src)
target_compile_definitions(tidesdb_planner_tests PRIVATE _GNU_SOURCE)  # fallocate, as TidesDB builds it
target_link_libraries(tidesdb_planner_tests PRIVATE tidesdb)
add_test(NAME TidesDbPlanner COMMAND tidesdb_planner_tests)

file(GLOB INTEGRATION_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cc)
if(INTEGRATION_SOURCES AND TARGET tideskv-server)
  add_executable(integration_tests ${INTEGRATION_SOURCES})
  target_link_libraries(integration_tests PRIVATE test_support GTest::gtest_main)
  target_compile_definitions(integration_tests PRIVATE TIDESKV_SERVER_BIN="$<TARGET_FILE:tideskv-server>")
  add_dependencies(integration_tests tideskv-server)
  tideskv_defaults(integration_tests)
  # Throughput-bound tests whose work cannot shrink without losing what they check, slow under a
  # sanitizer (Debug, clang, everything instrumented). Seconds measured with taskset -c 0-3 (two SMT
  # pairs, like the 4-vCPU CI runner), sanitized, beside three other heavy tests; the CI runners are
  # about 1.7x slower again over the whole suite and up to 3.5x for one test:
  #   CollectionConcurrencyIT.ConcurrentCreatesCrossALeaseBoundary  39 s TSan (72000 HSETs: > one oid lease)
  #   CompactionIT.FlushDbPhysicallyReclaimsThatDbsKeys             40 s TSan, 37 s of it the 60000-key fill
  #   ScriptingIT.KillDuringEvalIsAllOrNothing                      41 s TSan (11 server starts, 6 scripts)
  #   ListZsetIT.LargeListAcrossManyFlushes                         22 s TSan in the full suite; 58 s in CI
  # In CI run 39108774940 the first two timed out at 60 s under ASan and TSan, the third under ASan (34 s
  # under TSan), and the last passed both with 2 s to spare.
  set(SLOW_INTEGRATION_TESTS
      CollectionConcurrencyIT.ConcurrentCreatesCrossALeaseBoundary
      CompactionIT.FlushDbPhysicallyReclaimsThatDbsKeys
      ScriptingIT.KillDuringEvalIsAllOrNothing
      ListZsetIT.LargeListAcrossManyFlushes)
  list(JOIN SLOW_INTEGRATION_TESTS ":" slow_filter)
  set(slow_timeout 60)
  if(TIDESKV_SANITIZE)
    set(slow_timeout 300)
  endif()
  gtest_discover_tests(integration_tests DISCOVERY_MODE PRE_TEST TEST_FILTER "-${slow_filter}"
                       PROPERTIES TIMEOUT 60)
  gtest_discover_tests(integration_tests DISCOVERY_MODE PRE_TEST TEST_FILTER "${slow_filter}"
                       PROPERTIES TIMEOUT ${slow_timeout})
endif()

if(TIDESKV_FUZZ)
  add_executable(resp_parser_fuzz ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/resp_parser_fuzz.cc)
  target_compile_options(resp_parser_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(resp_parser_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_libraries(resp_parser_fuzz PRIVATE tideskv_core)
  tideskv_defaults(resp_parser_fuzz)

  add_executable(replog_codec_fuzz ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/replog_codec_fuzz.cc)
  target_compile_options(replog_codec_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(replog_codec_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_libraries(replog_codec_fuzz PRIVATE tideskv_core)
  tideskv_defaults(replog_codec_fuzz)
endif()

add_test(NAME engine_boundary
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/check_engine_boundary.sh
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

find_package(Python3 3.10 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME bench_report
    COMMAND ${Python3_EXECUTABLE} -m unittest bench/compare/test_report.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()

add_test(NAME bench_workloads
  COMMAND bash bench/compare/test_workloads.sh
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

option(TIDESKV_COMPAT "Run the Go compatibility suites (needs Go >= 1.22; first run downloads modules)" OFF)
find_program(GO_EXECUTABLE go)
if(TIDESKV_COMPAT AND NOT GO_EXECUTABLE)
  message(WARNING "TIDESKV_COMPAT is ON but no Go toolchain was found; compat_kvrocks is not registered")
elseif(TIDESKV_COMPAT AND TARGET tideskv-server)
  add_test(NAME compat_kvrocks COMMAND ${PROJECT_SOURCE_DIR}/scripts/compat.sh $<TARGET_FILE:tideskv-server>)
  set_tests_properties(compat_kvrocks PROPERTIES TIMEOUT 1800 LABELS compat)
endif()

file(GLOB STRESS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/stress/*.cc)
if(STRESS_SOURCES)
  add_executable(stress_tests ${STRESS_SOURCES})
  target_link_libraries(stress_tests PRIVATE test_support GTest::gtest_main)
  tideskv_defaults(stress_tests)
  gtest_discover_tests(stress_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120 LABELS stress)
endif()

# kill -9 durability rounds restart the server many times: own executable, label and timeout. The crash
# and multinode suites share one resource lock: one heavy test at a time, the rest of ctest -j alongside.
file(GLOB CRASH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/crash/*.cc)
if(CRASH_SOURCES AND TARGET tideskv-server)
  add_executable(crash_tests ${CRASH_SOURCES})
  target_link_libraries(crash_tests PRIVATE test_support GTest::gtest_main)
  target_compile_definitions(crash_tests PRIVATE TIDESKV_SERVER_BIN="$<TARGET_FILE:tideskv-server>")
  add_dependencies(crash_tests tideskv-server)
  tideskv_defaults(crash_tests)
  gtest_discover_tests(crash_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 300 LABELS crash RESOURCE_LOCK heavy_servers)
endif()

# L5 replication (M6a exit criterion): several servers per test, kill -9 loops and failovers
file(GLOB MULTINODE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/multinode/*.cc)
if(MULTINODE_SOURCES AND TARGET tideskv-server)
  add_executable(multinode_tests ${MULTINODE_SOURCES})
  target_link_libraries(multinode_tests PRIVATE test_support GTest::gtest_main)
  target_compile_definitions(multinode_tests PRIVATE TIDESKV_SERVER_BIN="$<TARGET_FILE:tideskv-server>")
  add_dependencies(multinode_tests tideskv-server)
  tideskv_defaults(multinode_tests)
  gtest_discover_tests(multinode_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 900 LABELS multinode RESOURCE_LOCK heavy_servers)
endif()
