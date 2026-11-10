# Every third-party dependency is pinned by version AND SHA-256.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)

FetchContent_Declare(Catch2
  URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
  URL_HASH SHA256=c991b247a1a0d7bb9c39aa35faf0fe9e19764213f28ffba3109388e62ee0269c)

FetchContent_Declare(CLI11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.4.2.tar.gz
  URL_HASH SHA256=f2d893a65c3b1324c50d4e682c0cdc021dd0477ae2c048544f39eed6654b699a)

# Header-only libraries: download only (SOURCE_SUBDIR points nowhere so their own CMake isn't run).
FetchContent_Declare(eigen
  URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz
  URL_HASH SHA256=8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72
  SOURCE_SUBDIR do-not-configure)

FetchContent_Declare(httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.63.0.tar.gz
  URL_HASH SHA256=44ab42e16926e95f7641d286bc90d739ab1c16886114d7cc5bc7a9f905c5edf8
  SOURCE_SUBDIR do-not-configure)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(CLI11_BUILD_TESTS OFF CACHE INTERNAL "")
set(CLI11_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(CLI11_BUILD_DOCS OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json CLI11 eigen httplib)
if(RECS_BUILD_TESTS)
  FetchContent_MakeAvailable(Catch2)
endif()

add_library(recs_eigen INTERFACE)
target_include_directories(recs_eigen SYSTEM INTERFACE ${eigen_SOURCE_DIR})
add_library(Eigen3::Eigen ALIAS recs_eigen)

add_library(recs_httplib INTERFACE)
target_include_directories(recs_httplib SYSTEM INTERFACE ${httplib_SOURCE_DIR})
add_library(httplib::httplib ALIAS recs_httplib)
