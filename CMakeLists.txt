cmake_minimum_required(VERSION 3.14)

project(slick-stacker
        VERSION 0.1.0
        DESCRIPTION "A standalone, header-only, low-latency order stacker"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options:
option(SLICK_STACKER_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(SLICK_STACKER_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(SLICK_STACKER_BUILD_EXAMPLES "Build examples" OFF)

add_library(slick-stacker INTERFACE)
add_library(slick::stacker ALIAS slick-stacker)

target_include_directories(slick-stacker INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(slick-stacker INTERFACE cxx_std_20)
set_target_properties(slick-stacker PROPERTIES EXPORT_NAME stacker)

if(SLICK_STACKER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SLICK_STACKER_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(SLICK_STACKER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Installation rules
install(DIRECTORY include/ DESTINATION include)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/slick-stackerConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/slick-stackerConfig.cmake"
    INSTALL_DESTINATION lib/cmake/slick-stacker
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/slick-stackerConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/slick-stackerConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/slick-stackerConfigVersion.cmake"
    DESTINATION lib/cmake/slick-stacker
)

install(TARGETS slick-stacker
    EXPORT slick-stackerTargets
)

install(EXPORT slick-stackerTargets
    FILE slick-stackerTargets.cmake
    NAMESPACE slick::
    DESTINATION lib/cmake/slick-stacker
)

message(STATUS "${PROJECT_NAME}: ${PROJECT_VERSION}")
