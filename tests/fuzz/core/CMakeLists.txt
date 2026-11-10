# libFuzzer harnesses for the librecs loaders (fuzz preset only). Added from core/CMakeLists.txt,
# because tests/ is not part of the fuzz build (RECS_BUILD_TESTS=OFF) and the top-level
# CMakeLists.txt is a frozen wiring file. Every *.cpp here becomes fuzz_<name>; drop in a file to
# add a harness. Corpora live in corpus/<name>/ (written by fuzz_core_seedgen, see seedgen/).
file(GLOB RECS_CORE_FUZZERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
foreach(f ${RECS_CORE_FUZZERS})
  get_filename_component(n ${f} NAME_WE)
  add_executable(fuzz_${n} ${f})
  target_compile_options(fuzz_${n} PRIVATE -fsanitize=fuzzer)
  target_link_options(fuzz_${n} PRIVATE -fsanitize=fuzzer)
  target_link_libraries(fuzz_${n} PRIVATE recs_core)
  recs_set_warnings(fuzz_${n})
endforeach()

# Seed generator: an ordinary program (not a fuzzer) that writes corpus/ from real writer output.
add_executable(fuzz_core_seedgen ${CMAKE_CURRENT_SOURCE_DIR}/seedgen/seedgen.cpp)
target_link_libraries(fuzz_core_seedgen PRIVATE recs_core)
recs_set_warnings(fuzz_core_seedgen)
