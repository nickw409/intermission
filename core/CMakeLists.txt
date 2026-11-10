# librecs. Sources are globbed per module directory, so adding a .cpp under core/src/<module>/ needs no CMake edit.
file(GLOB_RECURSE RECS_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(recs_core STATIC ${RECS_CORE_SOURCES})
target_include_directories(recs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(recs_core PUBLIC nlohmann_json::nlohmann_json Eigen3::Eigen OpenMP::OpenMP_CXX)
recs_set_warnings(recs_core)
