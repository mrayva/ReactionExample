cmake_minimum_required(VERSION 3.20)
project(ReactionExample CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(ENABLE_TESTS "Build and register tests" ON)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()

# Find dependencies
find_package(reaction CONFIG REQUIRED)
find_path(PARALLEL_HASHMAP_INCLUDE_DIRS "parallel_hashmap/phmap.h" REQUIRED)
find_package(Threads REQUIRED)

# Build main demo executable
add_executable(demo main.cpp)
target_link_libraries(demo PRIVATE reaction::reaction)
target_include_directories(demo PRIVATE ${PARALLEL_HASHMAP_INCLUDE_DIRS})

# Build lock-free test executable
add_executable(test_lock_free test_lock_free.cpp)
target_link_libraries(test_lock_free PRIVATE reaction::reaction Threads::Threads)
target_include_directories(test_lock_free PRIVATE ${PARALLEL_HASHMAP_INCLUDE_DIRS})

# Build simple test executable
add_executable(simple_test simple_test.cpp)
target_link_libraries(simple_test PRIVATE reaction::reaction)
target_include_directories(simple_test PRIVATE ${PARALLEL_HASHMAP_INCLUDE_DIRS})

# Build regression tests executable
add_executable(regression_tests regression_tests.cpp)
target_link_libraries(regression_tests PRIVATE reaction::reaction Threads::Threads)
target_include_directories(regression_tests PRIVATE ${PARALLEL_HASHMAP_INCLUDE_DIRS})

if(ENABLE_TESTS)
  include(CTest)
  enable_testing()

  add_test(NAME simple_test_smoke COMMAND $<TARGET_FILE:simple_test>)
  add_test(NAME demo_smoke COMMAND $<TARGET_FILE:demo>)
  add_test(NAME lock_free_regression COMMAND $<TARGET_FILE:test_lock_free>)
  add_test(NAME api_regression COMMAND $<TARGET_FILE:regression_tests>)
endif()
