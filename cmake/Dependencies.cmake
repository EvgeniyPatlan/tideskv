include(FetchContent)
find_package(Threads REQUIRED)

# --- TidesDB (pinned: v10.1.1) ---
set(TIDESDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TIDESDB_WARN_STRICT OFF CACHE BOOL "" FORCE)
set(TIDESDB_WITH_SNAPPY OFF CACHE BOOL "" FORCE)
set(TIDESDB_WITH_LZ4 ${TIDESKV_COMPRESSION} CACHE BOOL "" FORCE)
set(TIDESDB_WITH_ZSTD ${TIDESKV_COMPRESSION} CACHE BOOL "" FORCE)
# The patch fixes a planner loop: with only the largest level over its capacity, 10.1.1 merges into a
# smaller level that fits, leaves the largest level full, and repeats that merge forever on an idle family.
# Applying it a second time (a re-run patch step) is detected by the reverse check and skipped.
set(TIDESDB_PATCH ${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-deepen-when-largest-level-full.patch)
FetchContent_Declare(tidesdb
  GIT_REPOSITORY https://github.com/tidesdb/tidesdb.git
  GIT_TAG 6d1fb731e6ea1bff0de1c290041bdeacbc1267e6
  PATCH_COMMAND sh -c "git apply --reverse --check '${TIDESDB_PATCH}' 2>/dev/null || git apply '${TIDESDB_PATCH}'")

# --- spdlog (bundled fmt) ---
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.14.1)

# --- standalone Asio (header-only, no CMakeLists in repo) ---
FetchContent_Declare(asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG asio-1-30-2)

FetchContent_MakeAvailable(tidesdb spdlog asio)

add_library(asio INTERFACE)
target_include_directories(asio SYSTEM INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_link_libraries(asio INTERFACE Threads::Threads)

if(TIDESKV_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2)
  set(RC_ENABLE_GTEST ON CACHE BOOL "" FORCE)
  FetchContent_Declare(rapidcheck
    GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
    GIT_TAG 2c3c4365aca21ef4e612768fdc95b1ce8b39a651)
  FetchContent_MakeAvailable(googletest rapidcheck)
endif()
