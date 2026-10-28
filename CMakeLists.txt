cmake_minimum_required(VERSION 3.20)
project(yumemi)

# ~~ c++ config stuff
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ~~ global cmake vars
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(DEPENDENCY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dependencies)
file(GLOB ALL_DEPENDENCY_INCLUDES "${DEPENDENCY_DIR}/*/include")

# ~~ compiler stuff
if (MSVC)
    set(CUSTOM_COMPILE_OPTIONS
        /W4 /wd4201 /WX /Zc:__cplusplus /MP /D "_MBCS")
else()
    set(CUSTOM_COMPILE_OPTIONS
        -Wall -Wextra -Werror -std=gnu++20)
endif()

# ~~ projects
add_subdirectory(src/core)