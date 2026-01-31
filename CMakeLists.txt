cmake_minimum_required(VERSION 3.20)
project(ReactionExample CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
find_package(TBB CONFIG REQUIRED)
# libcds is header-only, no find_package needed
find_package(Threads REQUIRED)

# Build main demo executable
add_executable(demo main.cpp)
target_link_libraries(demo PRIVATE reaction::reaction TBB::tbb)

# Build lock-free test executable
add_executable(test_lock_free test_lock_free.cpp)
target_link_libraries(test_lock_free PRIVATE reaction::reaction TBB::tbb Threads::Threads)

# ---- Tests via vcpkg-provided Catch2 ----
if (ENABLE_TESTS)
  find_package(Catch2 CONFIG REQUIRED)   # provided by vcpkg

  # ensure Threads target exists
  find_package(Threads REQUIRED)

  # find the reaction package (vcpkg should provide a reactionConfig.cmake)
  find_package(reaction CONFIG REQUIRED)

  add_executable(tests_reactive
    tests/test_reactive_collection.cpp
    tests/test_reactive_collection_extra.cpp
    tests/test_reactive_compare.cpp
  )
  target_compile_features(tests_reactive PUBLIC cxx_std_20)
  target_include_directories(tests_reactive PRIVATE ${PROJECT_SOURCE_DIR})

  target_link_libraries(tests_reactive
    PRIVATE
      reaction::reaction
      Threads::Threads
      Catch2::Catch2WithMain
  )

  add_test(NAME reactive_unit_tests COMMAND $<TARGET_FILE:tests_reactive>)
endif()