include(FetchContent)
find_package(Threads REQUIRED)

# --- TidesDB (pinned: v10.1.1) ---
set(TIDESDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TIDESDB_WARN_STRICT OFF CACHE BOOL "" FORCE)
set(TIDESDB_WITH_SNAPPY OFF CACHE BOOL "" FORCE)
set(TIDESDB_WITH_LZ4 ${TIDESKV_COMPRESSION} CACHE BOOL "" FORCE)
set(TIDESDB_WITH_ZSTD ${TIDESKV_COMPRESSION} CACHE BOOL "" FORCE)
# Applied in order by cmake/apply-patch-chain.sh, which skips what a re-run finds applied already, so a
# tree that holds only the first few patches gets the rest.
#  1. A planner loop: with only the largest level over its capacity, 10.1.1 merges into a smaller level
#     that fits, leaves the largest level full, and repeats that merge forever on an idle family.
#  2. That level is added by moving the largest level's files down instead of rewriting the whole family
#     into it (6.8 GB of writes for a 60M-key strings family).
#  3. The block cache turns eviction tombstones back into empty buckets, and rebuilds a shard's table once
#     they pass a quarter of it. Without that, after enough churn a miss probes the whole table.
#  4. A compaction reads each input level below L1 as one merge source instead of one per file, and the
#     merge stops scanning every source for read failures after every entry.
#  5. Point reads fetch only what they ask for: every file is opened with readahead off
#     (POSIX_FADV_RANDOM, RocksDB's advise_random_on_open), and the scans that read in order --
#     recovery, compaction, iteration -- prefetch the ranges ahead of them instead of switching the
#     shared descriptor to sequential. A value-log read also asks for the value's stored length rather
#     than its decoded one. A cold 256 KiB GET read ~450 KiB from disk and now reads ~245 KiB; one
#     stored lz4 in ~110 KiB read ~380 KiB and now reads ~107 KiB.
set(TIDESDB_PATCHES
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-deepen-when-largest-level-full.patch"
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-deepen-by-moving-the-largest-level.patch"
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-cache-clear-tombstones.patch"
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-compaction-merge-level-runs.patch"
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-random-read-hint.patch"
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-range-covering-index.patch"
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-compaction-partition-cursor.patch"
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-btree-single-pass-leaves.patch"
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-btree-builder-arena.patch"
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-io-trim-syscalls.patch"
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-compression-start-level.patch"
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-wal-flusher-wake.patch"
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-vlog-full-codec.patch"
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-commit-gate.patch"
  # a synchronous flush and a checkpoint wait for the memtables queued when they were called, not for
  # an empty queue that writers outrunning flush never leave
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-flush-wait-called-generations.patch"
  # the flusher test's signal bound allows for a loaded host: a signal per appender stall plus one per ten appends
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-wal-flusher-test-load.patch"
  # an iterator opens a level run's table cursors only as a seek or step reaches each table, and asks
  # the run's intervals from the tables it pinned rather than through their cursors
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-lazy-level-cursors.patch"
  # memtable_whole_key_bloom: each memtable keeps a whole-key bloom in front of its skip list, so a point
  # read skips the memtables that cannot hold its key instead of descending each one's skip list
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-memtable-bloom.patch"
  # a decoded btree node is built in one arena sized exactly to what it keeps, so the node cache is
  # charged about the node instead of a pooled chunk twice its size, and a compressed node decodes
  # into a thread buffer rather than a fresh allocation per cache miss
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-node-arena-exact.patch"
  # memtable_l0_queue_max_bytes: a byte budget for the queued immutables beside the count limit, so a large
  # write buffer does not turn the count writers need through a merge into more memory than the host has
  "${CMAKE_CURRENT_LIST_DIR}/patches/tidesdb-l0-queue-byte-budget.patch")
# The patch step reruns only when its command changes, so the command carries a hash of the patches'
# contents, which the script ignores. Editing a patch then reconfigures (CMAKE_CONFIGURE_DEPENDS) and
# reruns the step, and on a tree patched with the old version the step fails, asking for a fresh fetch.
set(TIDESDB_PATCH_HASHES "")
foreach(patch IN LISTS TIDESDB_PATCHES)
  file(SHA256 "${patch}" patch_hash)
  string(APPEND TIDESDB_PATCH_HASHES "${patch_hash}")
endforeach()
string(SHA256 TIDESDB_PATCH_CHAIN_SHA256 "${TIDESDB_PATCH_HASHES}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TIDESDB_PATCHES})
FetchContent_Declare(tidesdb
  GIT_REPOSITORY https://github.com/tidesdb/tidesdb.git
  GIT_TAG 6d1fb731e6ea1bff0de1c290041bdeacbc1267e6
  # one argument per patch: each list element stays whole even if its path has spaces
  PATCH_COMMAND sh "${CMAKE_CURRENT_LIST_DIR}/apply-patch-chain.sh"
                "--chain-sha256=${TIDESDB_PATCH_CHAIN_SHA256}" ${TIDESDB_PATCHES})

# --- spdlog (bundled fmt) ---
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.14.1)

# --- standalone Asio (header-only, no CMakeLists in repo) ---
FetchContent_Declare(asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG asio-1-30-2)

# --- mimalloc (pinned: v2.1.7): the process allocator, TidesDB's C code included ---
# The server executable links mimalloc's object file, so its malloc/free/calloc/... definitions come first in
# the global symbol scope and interpose libc's for every shared library the process loads (libtidesdb.so too).
set(TIDESKV_MIMALLOC_VERSION "2.1.7")
# What INFO's mem_allocator reports, and what the integration test expects.
if(TIDESKV_ALLOCATOR STREQUAL "mimalloc")
  set(TIDESKV_ALLOCATOR_NAME "mimalloc-${TIDESKV_MIMALLOC_VERSION}")
else()
  set(TIDESKV_ALLOCATOR_NAME "libc")
endif()
if(TIDESKV_ALLOCATOR STREQUAL "mimalloc")
  set(MI_OVERRIDE ON CACHE BOOL "" FORCE)
  set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(MI_BUILD_STATIC OFF CACHE BOOL "" FORCE)
  set(MI_BUILD_OBJECT ON CACHE BOOL "" FORCE)
  FetchContent_Declare(mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG v${TIDESKV_MIMALLOC_VERSION})
  FetchContent_MakeAvailable(mimalloc)
endif()

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
