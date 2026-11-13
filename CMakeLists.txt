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

# Never fuse a*b + c into one FMA instruction. Fused and unfused results differ in the last bit,
# and determinism (spec "Numerics and determinism"; the golden and bit-identity tests) needs the
# same arithmetic on every architecture. GCC and Clang contract by default where the target has
# FMA: always on arm64 (production, a Raspberry Pi 4), never on baseline x86-64. Applies to all
# targets, third-party code included, so linked code agrees too.
add_compile_options(-ffp-contract=off)

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
