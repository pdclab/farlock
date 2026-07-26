cmake_minimum_required(VERSION 3.18)
project(aflock LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

# Limit benchmark compilation concurrency to avoid OOM kills when make runs with -j,
# while still adapting to the host capacity.
if(DEFINED ENV{BENCHMARK_COMPILE_CONCURRENCY} AND NOT "$ENV{BENCHMARK_COMPILE_CONCURRENCY}" STREQUAL "")
    set(BENCHMARK_COMPILE_CONCURRENCY "$ENV{BENCHMARK_COMPILE_CONCURRENCY}")
else()
    include(ProcessorCount)
    ProcessorCount(BENCHMARK_LOGICAL_CORES)
    if(BENCHMARK_LOGICAL_CORES GREATER 0)
        math(EXPR BENCHMARK_COMPILE_CONCURRENCY "${BENCHMARK_LOGICAL_CORES} / 2")
        if(BENCHMARK_COMPILE_CONCURRENCY LESS 1)
            set(BENCHMARK_COMPILE_CONCURRENCY 1)
        endif()
    else()
        set(BENCHMARK_COMPILE_CONCURRENCY 2)
    endif()
endif()
message(STATUS "Benchmark compile concurrency: ${BENCHMARK_COMPILE_CONCURRENCY}")
set_property(GLOBAL PROPERTY JOB_POOLS benchmark_compile_pool=${BENCHMARK_COMPILE_CONCURRENCY})

include(CheckLibraryExists)

check_library_exists(ibverbs ibv_open_device "" HAVE_IBVERBS)
if(NOT HAVE_IBVERBS)
    message(FATAL_ERROR "IBVerbs library not found")
endif()

# Build every .cc file in this folder as its own executable
file(GLOB BENCHMARK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

# Set the default compile mode to Debug if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Set logging level based on build type or user input
if(NOT DEFINED LOG_LEVEL)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(LOG_LEVEL DEBUG)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(LOG_LEVEL INFO)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Perf")
        set(LOG_LEVEL INFO)
    else()
        set(LOG_LEVEL TRACE)
    endif()
endif()

# Map the simplified log levels to SPDLOG levels
if(LOG_LEVEL STREQUAL "TRACE")
    set(SPDLOG_LEVEL SPDLOG_LEVEL_TRACE)
elseif(LOG_LEVEL STREQUAL "DEBUG")
    set(SPDLOG_LEVEL SPDLOG_LEVEL_DEBUG)
elseif(LOG_LEVEL STREQUAL "INFO")
    set(SPDLOG_LEVEL SPDLOG_LEVEL_INFO)
elseif(LOG_LEVEL STREQUAL "WARN")
    set(SPDLOG_LEVEL SPDLOG_LEVEL_WARN)
elseif(LOG_LEVEL STREQUAL "ERROR")
    set(SPDLOG_LEVEL SPDLOG_LEVEL_ERROR)
elseif(LOG_LEVEL STREQUAL "CRITICAL")
    set(SPDLOG_LEVEL SPDLOG_LEVEL_CRITICAL)
elseif(LOG_LEVEL STREQUAL "OFF")
    set(SPDLOG_LEVEL SPDLOG_LEVEL_OFF)
else()
    message(FATAL_ERROR "Invalid LOG_LEVEL specified")
endif()

set(INCLUDE_BASE ${PROJECT_SOURCE_DIR}/../include)
include_directories(${INCLUDE_BASE})
include_directories(${INCLUDE_BASE}/locks)
include_directories(${INCLUDE_BASE}/third_party)

find_library(IBVERBS_LIB ibverbs)

set(BENCHMARK_TARGETS)

foreach(SRC_FILE IN LISTS BENCHMARK_SOURCES)
    get_filename_component(TARGET_NAME "${SRC_FILE}" NAME_WE)
    add_executable("${TARGET_NAME}" "${SRC_FILE}")
    set_property(TARGET "${TARGET_NAME}" PROPERTY JOB_POOL_COMPILE benchmark_compile_pool)

    # Unix Makefiles does not honor JOB_POOL_COMPILE for compile rules, so keep
    # a small dependency window to cap peak compiler memory use.
    list(LENGTH BENCHMARK_TARGETS BENCHMARK_TARGET_COUNT)
    if(CMAKE_GENERATOR MATCHES "Makefiles" AND BENCHMARK_TARGET_COUNT GREATER BENCHMARK_COMPILE_CONCURRENCY)
        math(EXPR PRIOR_TARGET_INDEX "${BENCHMARK_TARGET_COUNT} - ${BENCHMARK_COMPILE_CONCURRENCY}")
        list(GET BENCHMARK_TARGETS ${PRIOR_TARGET_INDEX} PRIOR_BENCHMARK_TARGET)
        add_dependencies("${TARGET_NAME}" "${PRIOR_BENCHMARK_TARGET}")
    endif()
    list(APPEND BENCHMARK_TARGETS "${TARGET_NAME}")

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options("${TARGET_NAME}" PRIVATE -g -O0)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options("${TARGET_NAME}" PRIVATE -O3)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Perf")
        target_compile_options("${TARGET_NAME}" PRIVATE -O2 -g)
    endif()

    target_compile_definitions("${TARGET_NAME}" PUBLIC SPDLOG_ACTIVE_LEVEL=${SPDLOG_LEVEL})
    target_link_libraries("${TARGET_NAME}" PRIVATE ${IBVERBS_LIB})
endforeach()