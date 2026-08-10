add_executable(simple_stack simple_stack.cpp)
target_link_libraries(simple_stack PRIVATE slick::stacker)

if(MSVC)
  target_compile_options(simple_stack PRIVATE /W4)
else()
  target_compile_options(simple_stack PRIVATE -Wall -Wextra -Wpedantic)
endif()
