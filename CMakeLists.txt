cmake_minimum_required(VERSION 3.27)
project(ReactionExample VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_WARNINGS "Enable extra warnings" ON)
option(ENABLE_SANITIZERS "Build with ASan/UBSan in Debug" OFF)
option(ENABLE_LTO "Enable LTO in Release" ON)
option(ENABLE_TESTS "Build and run unit tests" ON)

# --- Sanitizers (ASan/UBSan) ----------------------------------------------
# Enable with -DENABLE_SANITIZERS=ON at configure time.
if (ENABLE_SANITIZERS)
  if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(STATUS "Enabling ASan/UBSan (AddressSanitizer + UndefinedBehaviorSanitizer)")
    # Make sanitizer builds easier to debug
    add_compile_options(-g -O1 -fno-omit-frame-pointer)
    # AddressSanitizer + UndefinedBehaviorSanitizer
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
    # Optionally make UBSan fatal so we stop at the first UB
    add_compile_options(-fno-sanitize-recover=undefined)
  else()
    message(WARNING "ENABLE_SANITIZERS requested but compiler does not look like GCC/Clang. Ignored.")
  endif()
endif()
# -------------------------------------------------------------------------

find_package(reaction CONFIG REQUIRED)
find_package(Threads REQUIRED)

enable_testing()

# Main executable
add_executable(ReactionExample main.cpp)
target_compile_features(ReactionExample PUBLIC cxx_std_20)

if (ENABLE_WARNINGS)
  target_compile_options(ReactionExample PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
  )
endif()

target_link_libraries(ReactionExample PRIVATE reaction::reaction Threads::Threads)
install(TARGETS ReactionExample RUNTIME DESTINATION bin)

# ---- Tests via vcpkg-provided Catch2 ----
if (ENABLE_TESTS)
  find_package(Catch2 CONFIG REQUIRED)   # provided by vcpkg

  # Test executable: include both test sources here
  add_executable(tests_reactive
    tests/test_reactive_collection.cpp
    tests/test_reactive_collection_extra.cpp
  )
  target_compile_features(tests_reactive PUBLIC cxx_std_20)

  # Ensure tests can include project headers
  target_include_directories(tests_reactive PRIVATE ${PROJECT_SOURCE_DIR})

  if (ENABLE_WARNINGS)
    target_compile_options(tests_reactive PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
      $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )
  endif()

  target_link_libraries(tests_reactive PRIVATE reaction::reaction Threads::Threads Catch2::Catch2WithMain)

  # Register test using full path to the test binary
  add_test(NAME reactive_unit_tests COMMAND $<TARGET_FILE:tests_reactive>)
endif()