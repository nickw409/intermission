# One test binary. Every *.cpp under tests/core/ and tests/model-service/ is compiled in, so
# workstreams add test files without editing this file.
file(GLOB_RECURSE RECS_TEST_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/model-service/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/support/*.cpp)
add_executable(recs_tests ${RECS_TEST_SOURCES})
target_include_directories(recs_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(recs_tests PRIVATE recs_core recs_model_service Catch2::Catch2WithMain)
target_compile_definitions(recs_tests PRIVATE
  RECS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  RECS_CONTRACTS_DIR="${CMAKE_SOURCE_DIR}/contracts"
  RECS_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
recs_set_warnings(recs_tests)

include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
# Tests tagged [slow] or [ml32m] are excluded from the default run.
catch_discover_tests(recs_tests TEST_SPEC "~[ml32m]~[slow]" DISCOVERY_MODE PRE_TEST)
