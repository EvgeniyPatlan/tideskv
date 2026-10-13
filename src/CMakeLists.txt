file(GLOB TIDESKV_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*/*.cc)
list(FILTER TIDESKV_CORE_SOURCES EXCLUDE REGEX "/src/app/")

# Redis 7.2.5 deps/fpconv (Boost Software License 1.0): the exact double formatting of Redis replies.
add_library(fpconv STATIC ${PROJECT_SOURCE_DIR}/third_party/fpconv/fpconv_dtoa.c)
target_include_directories(fpconv PUBLIC ${PROJECT_SOURCE_DIR}/third_party/fpconv)
set_target_properties(fpconv PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Redis 7.2.16 deps/lua: PUC-Rio Lua 5.1.5 + Redis patches + cjson/cmsgpack/struct/bit (see NOTICE, M5b H1).
set(TIDESKV_LUA_DIR ${PROJECT_SOURCE_DIR}/third_party/lua)
set(TIDESKV_LUA_SOURCES
  lapi.c lcode.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c lmem.c lobject.c lopcodes.c
  lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c lvm.c lzio.c
  lauxlib.c lbaselib.c ldblib.c lmathlib.c lstrlib.c ltablib.c
  lua_cjson.c strbuf.c fpconv.c lua_cmsgpack.c lua_struct.c lua_bit.c)
list(TRANSFORM TIDESKV_LUA_SOURCES PREPEND ${TIDESKV_LUA_DIR}/)
add_library(tideskv_lua STATIC ${TIDESKV_LUA_SOURCES})
# SYSTEM: our warnings, -Werror and clang-tidy never report on Lua's headers.
target_include_directories(tideskv_lua SYSTEM PUBLIC ${TIDESKV_LUA_DIR})
target_compile_definitions(tideskv_lua PUBLIC LUA_ANSI PRIVATE ENABLE_CJSON_GLOBAL)
set_target_properties(tideskv_lua PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
target_link_libraries(tideskv_lua PUBLIC m)
if(TIDESKV_SANITIZE STREQUAL "address")
  # Lua 5.1 casts out-of-range doubles to int (ltable.c, lstrlib.c, lua_cmsgpack.c), as Redis does.
  target_compile_options(tideskv_lua PRIVATE -fno-sanitize=float-cast-overflow)
endif()

# Redis 7.2.16 src/sha1.c (public domain, Steve Reid).
add_library(tideskv_sha1 STATIC ${PROJECT_SOURCE_DIR}/third_party/sha1/sha1.c)
target_include_directories(tideskv_sha1 SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/sha1)
set_target_properties(tideskv_sha1 PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)

add_library(tideskv_core STATIC ${TIDESKV_CORE_SOURCES})
target_include_directories(tideskv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tideskv_core
  PUBLIC spdlog::spdlog asio Threads::Threads
  PRIVATE tidesdb fpconv tideskv_lua tideskv_sha1)
tideskv_defaults(tideskv_core)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/app/main.cc)
  add_executable(tideskv-server app/main.cc)
  target_link_libraries(tideskv-server PRIVATE tideskv_core)
  tideskv_defaults(tideskv-server)
endif()
