find_package(benchmark QUIET)

if(NOT benchmark_FOUND)
  include(FetchContent)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.8.3
  )
  FetchContent_MakeAvailable(benchmark)
endif()

set(SLICK_STACKER_BENCHMARKS
    bench_quote
    bench_events
)

foreach(bench ${SLICK_STACKER_BENCHMARKS})
  add_executable(${bench} ${bench}.cpp)
  target_link_libraries(${bench} PRIVATE slick::stacker benchmark::benchmark)
  target_include_directories(${bench} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(MSVC)
    target_compile_options(${bench} PRIVATE /O2 /W4)
  else()
    target_compile_options(${bench} PRIVATE -O3 -march=native -Wall -Wextra)
  endif()
endforeach()

add_custom_target(run_benchmarks
  COMMAND bench_quote --benchmark_out=bench_quote.json --benchmark_out_format=json
  COMMAND bench_events --benchmark_out=bench_events.json --benchmark_out_format=json
  DEPENDS ${SLICK_STACKER_BENCHMARKS}
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  COMMENT "Running slick-stacker benchmarks"
)
