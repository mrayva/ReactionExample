cmake_minimum_required(VERSION 3.27.5)
project(ReactionExample VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 1. Find the package
find_package(reaction CONFIG REQUIRED)

# 2. CREATE the target first
add_executable(ReactionExample main.cpp)

# 3. LINK the library to the existing target
target_link_libraries(ReactionExample PRIVATE reaction::reaction)
