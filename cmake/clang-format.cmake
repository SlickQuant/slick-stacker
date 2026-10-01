# `format` and `format-check` targets, driving the same clang-format check CI
# runs. Neither is part of the default build: `format-check` fails on the first
# misformatted file, and `format` rewrites the sources in place.
#
#   cmake --build build --target format-check
#   cmake --build build --target format

# Formatting differs between clang-format releases. Keep this in step with
# CLANG_FORMAT_VERSION in .github/workflows/ci.yml.
set(SLICK_STACKER_CLANG_FORMAT_VERSION 20.1.8)

find_program(SLICK_STACKER_CLANG_FORMAT
    NAMES clang-format-20 clang-format
    DOC "clang-format used by the format and format-check targets")

if(NOT SLICK_STACKER_CLANG_FORMAT)
    message(STATUS "${PROJECT_NAME}: clang-format not found, format targets disabled")
    return()
endif()

execute_process(
    COMMAND "${SLICK_STACKER_CLANG_FORMAT}" --version
    OUTPUT_VARIABLE _slick_stacker_cf_version
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCH "version ([0-9]+\\.[0-9]+\\.[0-9]+)" _ "${_slick_stacker_cf_version}")
if(NOT CMAKE_MATCH_1 VERSION_EQUAL SLICK_STACKER_CLANG_FORMAT_VERSION)
    message(WARNING
        "${PROJECT_NAME}: ${SLICK_STACKER_CLANG_FORMAT} is clang-format "
        "'${CMAKE_MATCH_1}', CI uses ${SLICK_STACKER_CLANG_FORMAT_VERSION}; "
        "format-check may disagree with CI. Point SLICK_STACKER_CLANG_FORMAT at "
        "${SLICK_STACKER_CLANG_FORMAT_VERSION} to match.")
endif()

file(GLOB_RECURSE _slick_stacker_format_files CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/include/*.hpp"
    "${PROJECT_SOURCE_DIR}/tests/*.hpp"
    "${PROJECT_SOURCE_DIR}/tests/*.cpp"
    "${PROJECT_SOURCE_DIR}/benchmarks/*.hpp"
    "${PROJECT_SOURCE_DIR}/benchmarks/*.cpp"
    "${PROJECT_SOURCE_DIR}/examples/*.hpp"
    "${PROJECT_SOURCE_DIR}/examples/*.cpp")

add_custom_target(format-check
    COMMAND "${SLICK_STACKER_CLANG_FORMAT}" --dry-run --Werror ${_slick_stacker_format_files}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Checking formatting with clang-format ${CMAKE_MATCH_1}"
    VERBATIM)

add_custom_target(format
    COMMAND "${SLICK_STACKER_CLANG_FORMAT}" -i ${_slick_stacker_format_files}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Formatting sources with clang-format ${CMAKE_MATCH_1}"
    VERBATIM)
