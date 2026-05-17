cmake_minimum_required(VERSION 3.20)
project(url_bloom_filter LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(URL_BLOOM_BUILD_TESTS "Build C++ unit tests" ON)
option(URL_BLOOM_BUILD_BENCHMARKS "Build C++ benchmarks" ON)
option(URL_BLOOM_BUILD_PYTHON "Build Python extension" ON)

add_library(url_bloom STATIC
  src/bloom_filter.cc
  third_party/xxhash/xxhash.c
)
set_target_properties(url_bloom PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(url_bloom PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash
)

if(URL_BLOOM_BUILD_PYTHON)
  find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
  find_package(pybind11 CONFIG REQUIRED)
  pybind11_add_module(url_bloom_native
    src/bindings.cc
  )
  target_link_libraries(url_bloom_native PRIVATE url_bloom)
  install(TARGETS url_bloom_native LIBRARY DESTINATION ".")
endif()

if(URL_BLOOM_BUILD_TESTS)
  enable_testing()
  add_executable(url_bloom_test tests/test_bloom_filter.cc)
  target_link_libraries(url_bloom_test PRIVATE url_bloom)
  add_test(NAME url_bloom_test COMMAND url_bloom_test)
endif()

if(URL_BLOOM_BUILD_BENCHMARKS)
  add_executable(url_bloom_benchmark bench/benchmark.cc)
  target_link_libraries(url_bloom_benchmark PRIVATE url_bloom)
endif()
