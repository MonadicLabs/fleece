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
# quickjs-ng unconditionally appends CMAKE_DL_LIBS (-ldl) to its link libraries.
# Fine on a host, but there is no libdl on a bare-metal target and the link fails
# with "cannot find -ldl". Nothing in fleece or quickjs calls dlopen(); this just
# clears the stray requirement, gated to the case that is actually broken.
if(CMAKE_CROSSCOMPILING)
    set(CMAKE_DL_LIBS "" CACHE STRING "" FORCE)
endif()
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

# Include directories, attached to the target rather than the directory.
# include_directories() is directory-scoped: it works for everything built in
# THIS CMakeLists (the library, its tests, its examples) but propagates nothing
# to a parent project that add_subdirectory()s fleece and links the target. That
# went unnoticed while the one embedded consumer hand-rolled its own copy of
# this list; the moment it depended on the target properly, every fleece header
# went missing. PUBLIC so consumers inherit them.
set(FLEECE_PUBLIC_INCLUDES
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
# microReticulum, for src/reticulum. fleece owns this dependency outright --
# declaring it, pinning it, and configuring it -- because fleece is what uses
# it. An integrator consumes fleece and gets a correctly configured mesh stack;
# they do not have to know microReticulum exists.
#
# The knobs an integrator legitimately needs are cache variables below
# (FLEECE_RNS_HEAP_POOL_BUFFER_SIZE, FLEECE_RNS_LOG_LEVEL), settable before
# add_subdirectory(). Anything platform-specific that cannot be expressed that
# way -- linking an RTOS's include interface, say -- is done by the integrator
# against the targets afterwards.
#
# The guard remains for the case where an enclosing build genuinely defines its
# own microReticulum first; that one wins, and nothing here second-guesses it.
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
    # No filesystem-backed persistence: src/reticulum deliberately routes the
    # only thing worth persisting (the identity key) through the host's own
    # storage hooks instead, so Reticulum never needs to find a file. These
    # gate persistence, not whether Reticulum looks for files at all, which is
    # why a NoopFileSystem still has to be registered at runtime.
    set(RNS_USE_FS OFF CACHE BOOL "" FORCE)
    set(RNS_PERSIST_PATHS OFF CACHE BOOL "" FORCE)
    set(RNS_PERSIST_KNOWN_DESTINATIONS OFF CACHE BOOL "" FORCE)
    set(RNS_PERSIST_HASHLIST OFF CACHE BOOL "" FORCE)
    set(RNS_USE_PROVISIONING OFF CACHE BOOL "" FORCE)

    # A fixed-size TLSF pool rather than the system heap: on the targets fleece
    # exists for, an unbounded allocator in the packet path is how you get a
    # fragmentation failure hours into a flight. 80 KiB is a known-good budget
    # from real use; override before add_subdirectory() if your target differs.
    set(FLEECE_RNS_HEAP_POOL_BUFFER_SIZE 81920 CACHE STRING
        "Bytes for microReticulum's TLSF pool allocator")
    set(RNS_DEFAULT_ALLOCATOR RNS_HEAP_POOL_ALLOCATOR CACHE STRING "" FORCE)
    set(RNS_CONTAINER_ALLOCATOR RNS_HEAP_POOL_ALLOCATOR CACHE STRING "" FORCE)
    set(RNS_HEAP_POOL_BUFFER_SIZE ${FLEECE_RNS_HEAP_POOL_BUFFER_SIZE} CACHE STRING "" FORCE)

    FetchContent_MakeAvailable(rns_microreticulum)

    # microStore's headers select an implementation from whichever of these is
    # defined first, and microReticulum unconditionally defines
    # USTORE_USE_UNIVERSALFS. Without this override the NoopFileSystem adapter
    # src/reticulum registers is not even declared -- found by building fleece
    # standalone, where nothing else had already set it (the embedded build
    # does, which is why it only showed up here).
    set(FLEECE_RNS_LOG_LEVEL RNS_LOG_LEVEL_NONE CACHE STRING
        "microReticulum log level; NONE by default since fleece registers no log callback")
    target_compile_definitions(microReticulum PUBLIC
        USTORE_USE_NOOPFS
        RNS_LOG_LEVEL=${FLEECE_RNS_LOG_LEVEL}
    )

    # Position-independent code addresses every global indirectly through a GOT.
    # On a target with no dynamic loader -- a single static ELF, which is every
    # bare-metal and RTOS target fleece cares about -- nothing ever populates
    # that table, so each lookup silently reads address 0 rather than faulting.
    # That was a real, hard-to-find hang before it was understood. Cross-compiling
    # is the available proxy for "no dynamic loader"; host builds keep PIC, where
    # it is both harmless and needed if anything links fleece into a shared object.
    if(CMAKE_CROSSCOMPILING)
        set_target_properties(microReticulum CryptoLib PROPERTIES POSITION_INDEPENDENT_CODE OFF)
    endif()
endif()

add_library(fleece ${FLEECE_SRC})
target_include_directories(fleece PUBLIC ${FLEECE_PUBLIC_INCLUDES})
# -Os -flto on the C sources only. The C++ side links microReticulum, which is
# built with its own flags; forcing LTO across that boundary buys little and
# has a history of interacting badly with exceptions on embedded toolchains.
target_compile_options(fleece PRIVATE $<$<COMPILE_LANGUAGE:C>:-Os> $<$<COMPILE_LANGUAGE:C>:-flto>)
# microReticulum is PRIVATE: fleece's public headers expose none of its types
# (include/reticulum/fleece_reticulum.h is plain C -- callbacks and byte
# buffers), so consumers neither need nor should get its include paths. PUBLIC
# would let a consumer reach through fleece and use Reticulum directly, which
# is exactly the coupling this module exists to prevent -- and one did.
target_link_libraries(fleece PUBLIC qjs)
target_link_libraries(fleece PRIVATE microReticulum)

# Target properties
set_target_properties(fleece PROPERTIES
    VERSION "${FLEECE_VERSION_MAJOR}.${FLEECE_VERSION_MINOR}.${FLEECE_VERSION_PATCH}"
    SOVERSION "${FLEECE_VERSION_MAJOR}"
    OUTPUT_NAME "fleece"
)
# PIC on for host builds (a shared-library consumer needs it), off when
# cross-compiling -- see the microReticulum PIC comment above for why it is
# actively harmful on a static-ELF target.
set_target_properties(fleece PROPERTIES POSITION_INDEPENDENT_CODE $<NOT:$<BOOL:${CMAKE_CROSSCOMPILING}>>)

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
