# recs-model. Owned by workstream H. The library target lets tests and fuzzers reuse the handlers.
file(GLOB_RECURSE RECS_MODEL_LIB_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(recs_model_service STATIC ${RECS_MODEL_LIB_SOURCES})
target_include_directories(recs_model_service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
find_package(Threads REQUIRED)
target_link_libraries(recs_model_service PUBLIC recs_core httplib::httplib Threads::Threads)
# httplib's default listen backlog is 5. On a Unix socket a full backlog makes connect() fail at once
# (EAGAIN) instead of retrying, so a burst of new connections from recs-api would see errors.
# PUBLIC so every translation unit that includes httplib.h agrees on the value.
target_compile_definitions(recs_model_service PUBLIC CPPHTTPLIB_LISTEN_BACKLOG=512)
recs_set_warnings(recs_model_service)

add_executable(recs-model ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
target_link_libraries(recs-model PRIVATE recs_model_service CLI11::CLI11)
recs_set_warnings(recs-model)

if(RECS_FUZZ)
  # Coverage-instrument the service library (handlers, search, and the httplib code it
  # instantiates) so libFuzzer is guided through it, not only through the harness files.
  target_compile_options(recs_model_service PRIVATE -fsanitize=fuzzer-no-link)
  file(GLOB RECS_FUZZERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/fuzz/model/*.cpp)
  foreach(f ${RECS_FUZZERS})
    get_filename_component(n ${f} NAME_WE)
    add_executable(fuzz_${n} ${f})
    target_compile_options(fuzz_${n} PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_${n} PRIVATE -fsanitize=fuzzer)
    target_include_directories(fuzz_${n} PRIVATE ${CMAKE_SOURCE_DIR}/tests/fuzz/model)
    target_link_libraries(fuzz_${n} PRIVATE recs_model_service)
    recs_set_warnings(fuzz_${n})
  endforeach()
endif()
