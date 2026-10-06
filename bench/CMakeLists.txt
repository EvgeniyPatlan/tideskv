add_executable(tideskv-spike spike.cc)
target_link_libraries(tideskv-spike PRIVATE tideskv_core)
tideskv_defaults(tideskv-spike)
