cmake_minimum_required(VERSION 3.10)

# C AND C++: src/reticulum is C++ because microReticulum is. fleece's public
# API stays plain C -- see include/reticulum/fleece_reticulum.h -- so C callers
# are unaffected, but linking fleece now requires a C++ toolchain and runtime.
project(fleece LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    "src/planner/*.c"
    "src/reticulum/*.cpp"
)

# Include directories
include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/runtime"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/state"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/comms"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/embedded"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/planner"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/reticulum"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/state"
)

# Library
# microReticulum, for src/reticulum. If the enclosing build already defines the
# target, use it as-is and do NOT fetch a second copy: an embedded integrator
# typically needs to configure it for their platform first (allocator budget,
# toolchain quirks, linking against an RTOS's own include interface), and that
# configuration must not be silently duplicated or overridden from in here.
# fleece only falls back to fetching when it is the top-level project -- a host
# build, a test run, someone poking at it standalone.
if(NOT TARGET microReticulum)
    include(FetchContent)
    # MonadicLabs' fork: upstream plus portability fixes (a vendor `RNG` macro
    # collision, and a missing <cstdint> in Utilities/Memory.h) without which
    # this does not build on bare-metal/RTOS toolchains. Both are written to be
    # upstream-able, so this can move back to attermann/microReticulum if they
    # land there.
    FetchContent_Declare(
        rns_microreticulum
        GIT_REPOSITORY https://github.com/MonadicLabs/microReticulum.git
        GIT_TAG 3f06409a773a2637c5a696e394a909f4830013be
    )
    set(RNS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(RNS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(RNS_BUILD_INTEROP OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(rns_microreticulum)
endif()

add_library(fleece ${FLEECE_SRC})
# -Os -flto on the C sources only. The C++ side links microReticulum, which is
# built with its own flags; forcing LTO across that boundary buys little and
# has a history of interacting badly with exceptions on embedded toolchains.
target_compile_options(fleece PRIVATE $<$<COMPILE_LANGUAGE:C>:-Os> $<$<COMPILE_LANGUAGE:C>:-flto>)
target_link_libraries(fleece PUBLIC qjs microReticulum)

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

# Benchmarks: headless scale/link-loss simulators. Built like tests but not
# registered with ctest - they report metrics (convergence ticks, throughput,
# alloc churn) rather than pass/fail. Run one directly, e.g. ./bench_swarm.
file(GLOB_RECURSE BENCH_SRC "benchmarks/*.c")
foreach(bench_src ${BENCH_SRC})
    get_filename_component(bench_name ${bench_src} NAME_WE)
    add_executable(${bench_name} ${bench_src})
    target_link_libraries(${bench_name} fleece)
endforeach()

# Examples: one executable per examples/*.c file (http_server.c is a helper
# linked into the examples that need it, not a standalone program).
file(GLOB_RECURSE EXAMPLE_SRC "examples/*.c")
list(FILTER EXAMPLE_SRC EXCLUDE REGEX "examples/http_server\\.c$")
foreach(example_src ${EXAMPLE_SRC})
    get_filename_component(example_name ${example_src} NAME_WE)
    add_executable(${example_name} ${example_src})
    target_link_libraries(${example_name} fleece)
endforeach()

# example5 also needs the local HTTP helper
target_sources(example5_uav_swarm PRIVATE examples/http_server.c)

# Python bindings (nanobind): a standalone fleece_goap_native module wrapping
# only the planner + CBOR codec - not the rest of fleece (QuickJS, comms,
# embedded, runtime) - for offline plan-authoring tools (e.g. the
# swarmpu-groundstation GOAP mission editor). Off by default so the normal
# firmware build never needs a C++ toolchain or a Python dev environment.
option(FLEECE_BUILD_PYTHON_BINDINGS "Build the fleece_goap_native nanobind Python module" OFF)
if(FLEECE_BUILD_PYTHON_BINDINGS)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    find_package(Python 3.9 COMPONENTS Interpreter Development.Module REQUIRED)
    execute_process(
        COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        OUTPUT_VARIABLE NB_DIR
    )
    list(APPEND CMAKE_PREFIX_PATH "${NB_DIR}")
    find_package(nanobind CONFIG REQUIRED)

    nanobind_add_module(fleece_goap_native
        bindings/python/fleece_goap_bindings.cpp
        src/planner/fleece_planner.c
        src/state/fleece_cbor.c
        bindings/python/fleece_alloc_shim.c
    )
    target_include_directories(fleece_goap_native PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/planner"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/state"
    )
endif()
