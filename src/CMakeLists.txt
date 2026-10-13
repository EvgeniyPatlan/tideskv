file(GLOB TIDESKV_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*/*.cc)
list(FILTER TIDESKV_CORE_SOURCES EXCLUDE REGEX "/src/app/")

# Redis 7.2.5 deps/fpconv (Boost Software License 1.0): the exact double formatting of Redis replies.
add_library(fpconv STATIC ${PROJECT_SOURCE_DIR}/third_party/fpconv/fpconv_dtoa.c)
target_include_directories(fpconv PUBLIC ${PROJECT_SOURCE_DIR}/third_party/fpconv)
set_target_properties(fpconv PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(tideskv_core STATIC ${TIDESKV_CORE_SOURCES})
target_include_directories(tideskv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tideskv_core
  PUBLIC spdlog::spdlog asio Threads::Threads
  PRIVATE tidesdb fpconv)
tideskv_defaults(tideskv_core)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/app/main.cc)
  add_executable(tideskv-server app/main.cc)
  target_link_libraries(tideskv-server PRIVATE tideskv_core)
  tideskv_defaults(tideskv-server)
endif()
