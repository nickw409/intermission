# librecs. Sources are globbed per module directory, so adding a .cpp under core/src/<module>/ needs no CMake edit.
file(GLOB_RECURSE RECS_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(recs_core STATIC ${RECS_CORE_SOURCES})
target_include_directories(recs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(recs_core PUBLIC nlohmann_json::nlohmann_json Eigen3::Eigen OpenMP::OpenMP_CXX)
recs_set_warnings(recs_core)

if(RECS_FUZZ)
  # Coverage-instrument librecs so libFuzzer is guided through the loaders, not only the harness
  # files. The loader harnesses live in tests/fuzz/core (tests/ is not built in the fuzz preset).
  # The instrumentation's __sanitizer_cov_* hooks are defined by libFuzzer in the fuzz targets and
  # by the ASan/UBSan runtime everywhere else (recs-train, recs-model, fuzz_core_seedgen), so a
  # fuzz build needs RECS_SANITIZE=ON, as the fuzz preset sets.
  if(NOT RECS_SANITIZE)
    message(FATAL_ERROR "RECS_FUZZ=ON requires RECS_SANITIZE=ON (use the fuzz preset)")
  endif()
  target_compile_options(recs_core PRIVATE -fsanitize=fuzzer-no-link)
  add_subdirectory(${CMAKE_SOURCE_DIR}/tests/fuzz/core ${CMAKE_BINARY_DIR}/fuzz-core)
endif()
