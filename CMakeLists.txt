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

# QuickJS-ng (embedded JS engine). Configure before add_subdirectory so its own
# options don't build the qjs/qjsc CLIs or install targets as part of this project.
set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/quickjs EXCLUDE_FROM_ALL)

# Source files
file(GLOB_RECURSE FLEECE_SRC
    "src/runtime/*.c"
    "src/state/*.c"
    "src/comms/*.c"
    "src/embedded/*.c"
    "src/platform/*.c"
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
target_compile_options(fleece PRIVATE -Os -flto)
target_link_libraries(fleece PUBLIC qjs)

# Target properties
set_target_properties(fleece PROPERTIES
    VERSION "${FLEECE_VERSION_MAJOR}.${FLEECE_VERSION_MINOR}.${FLEECE_VERSION_PATCH}"
    SOVERSION "${FLEECE_VERSION_MAJOR}"
    OUTPUT_NAME "fleece"
    POSITION_INDEPENDENT_CODE ON
)

# Tests: one executable + one ctest entry per tests/*.c file
enable_testing()
file(GLOB_RECURSE TEST_SRC "tests/*.c")
foreach(test_src ${TEST_SRC})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} fleece)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Examples
file(GLOB_RECURSE EXAMPLE_SRC "examples/*.c")
if(EXAMPLE_SRC)
    add_executable(example1 examples/example1.c)
    target_link_libraries(example1 fleece)
endif()
