find_package(GTest QUIET)

if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
  )
  # For Windows: Prevent overriding the parent project's compiler/linker settings
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(slick-stacker-tests
    test_id_index.cpp
    test_slot_pool.cpp
    test_level_ring.cpp
    test_stacker_shape.cpp
    test_stacker_tracking.cpp
    test_stacker_rejects.cpp
    test_stacker_inflight.cpp
    test_stacker_reprice.cpp
    test_stacker_slicing.cpp
    test_stacker_slack.cpp
    test_stacker_pair.cpp
    test_stacker_order_type.cpp
)

target_link_libraries(slick-stacker-tests PRIVATE slick::stacker GTest::gtest_main)
target_include_directories(slick-stacker-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Invariant checks are on for the whole suite.
target_compile_definitions(slick-stacker-tests PRIVATE SLICK_STACKER_VALIDATE=1)

if(MSVC)
  target_compile_options(slick-stacker-tests PRIVATE /W4)
else()
  target_compile_options(slick-stacker-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GoogleTest)
gtest_discover_tests(slick-stacker-tests)
