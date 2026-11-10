# Intermission — top-level build. Owned by the orchestrator (wiring file).
# Modules add sources by dropping files into their directory; they never edit this file.
cmake_minimum_required(VERSION 3.25)
project(intermission LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RECS_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(RECS_FUZZ "Build libFuzzer targets (requires clang)" OFF)
option(RECS_BUILD_TESTS "Build the Catch2 test suite" ON)

if(RECS_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

include(cmake/deps.cmake)
include(cmake/warnings.cmake)
find_package(OpenMP REQUIRED)

add_subdirectory(core)
add_subdirectory(train)
add_subdirectory(model-service)

if(RECS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
