file(GLOB TIDESKV_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*/*.cc)
list(FILTER TIDESKV_CORE_SOURCES EXCLUDE REGEX "/src/app/")

add_library(tideskv_core STATIC ${TIDESKV_CORE_SOURCES})
target_include_directories(tideskv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tideskv_core
  PUBLIC spdlog::spdlog asio Threads::Threads
  PRIVATE tidesdb)
tideskv_defaults(tideskv_core)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/app/main.cc)
  add_executable(tideskv-server app/main.cc)
  target_link_libraries(tideskv-server PRIVATE tideskv_core)
  tideskv_defaults(tideskv-server)
endif()
