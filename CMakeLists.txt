cmake_minimum_required(VERSION 3.10)

project(fleece LANGUAGES C)

# Project version
set(FLEECE_VERSION_MAJOR 0)
set(FLEECE_VERSION_MINOR 1)
set(FLEECE_VERSION_PATCH 0)

# Compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Memory optimization flags for microcontrollers (commented out for non-ARM targets)
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Os -flto -march=armv7-m -mthumb")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Os -flto")

# Source files
file(GLOB_RECURSE FLEECE_SRC
    "src/runtime/*.c"
    "src/state/*.c"
    "src/comms/*.c"
    "src/embedded/*.c"
    "src/core/*.c"
)

# Include directories
include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/state"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/comms"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/embedded"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
)

# Library
add_library(fleece ${FLEECE_SRC})

# Target properties
set_target_properties(fleece PROPERTIES
    VERSION "${FLEECE_VERSION_MAJOR}.${FLEECE_VERSION_MINOR}.${FLEECE_VERSION_PATCH}"
    SOVERSION "${FLEECE_VERSION_MAJOR}"
    OUTPUT_NAME "fleece"
    POSITION_INDEPENDENT_CODE ON
)

# Tests
enable_testing()
file(GLOB_RECURSE TEST_SRC "tests/*.c")
if(TEST_SRC)
    add_executable(fleece_tests ${TEST_SRC})
    target_link_libraries(fleece_tests fleece)
    add_test(NAME fleece_tests COMMAND fleece_tests)
endif()

# Examples
file(GLOB_RECURSE EXAMPLE_SRC "examples/*.c")
if(EXAMPLE_SRC)
    add_executable(example1 examples/example1.c)
    target_link_libraries(example1 fleece)
endif()
