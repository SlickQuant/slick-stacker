add_executable(simple_stack simple_stack.cpp)
target_link_libraries(simple_stack PRIVATE slick::stacker)

if(MSVC)
  target_compile_options(simple_stack PRIVATE /W4)
else()
  target_compile_options(simple_stack PRIVATE -Wall -Wextra -Wpedantic)
endif()

# With the test suite on, the example also runs under CTest as a smoke test: it
# exits non-zero if the stack it drives fails its own invariant check.
if(SLICK_STACKER_BUILD_TESTS)
  add_test(NAME simple_stack COMMAND simple_stack)
endif()
