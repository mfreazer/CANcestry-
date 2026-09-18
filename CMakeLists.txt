cmake_minimum_required(VERSION 3.16)

project(cancestry
    VERSION 0.3.0
    DESCRIPTION "CAN codec, recipe and emulation runtime"
    LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build configuration" FORCE)
endif()

option(CANCESTRY_BUILD_TESTS "Build the CANcestry host unit tests" ON)
option(CANCESTRY_BUILD_EXAMPLES "Build the CANcestry host examples" ON)
option(CANCESTRY_STRICT_WARNINGS "Treat warnings as errors with a strict warning set" ON)

# Sanitizers are a host-only diagnostic aid: they are never enabled when cross
# compiling for a target.
if(NOT DEFINED CANCESTRY_ENABLE_ASAN)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "")
        set(CANCESTRY_ASAN_DEFAULT ON)
    else()
        set(CANCESTRY_ASAN_DEFAULT OFF)
    endif()
endif()
option(CANCESTRY_ENABLE_ASAN
    "Build with AddressSanitizer and UndefinedBehaviorSanitizer" ${CANCESTRY_ASAN_DEFAULT})

include(CheckCCompilerFlag)

if(CANCESTRY_STRICT_WARNINGS)
    if(MSVC)
        add_compile_options(/W4 /WX)
    else()
        # -Wall -Wextra -Werror -Wpedantic are required by the event core
        # acceptance criteria. The remaining flags enforce the embedded coding
        # rules: no VLAs, no K&R declarations, no qualifier or alignment casts.
        add_compile_options(
            -Wall
            -Wextra
            -Werror
            -Wpedantic
            -Wshadow
            -Wcast-qual
            -Wcast-align
            -Wstrict-prototypes
            -Wmissing-prototypes
            -Wold-style-definition
            -Wvla)
    endif()
endif()

set(CANCESTRY_SANITIZER_FLAGS "")
if(CANCESTRY_ENABLE_ASAN AND NOT CMAKE_CROSSCOMPILING)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    check_c_compiler_flag("-fsanitize=address,undefined" CANCESTRY_HAVE_SANITIZE_ADDRESS_UB)
    unset(CMAKE_REQUIRED_FLAGS)
    if(CANCESTRY_HAVE_SANITIZE_ADDRESS_UB)
        list(APPEND CANCESTRY_SANITIZER_FLAGS -fsanitize=address,undefined)
    else()
        check_c_compiler_flag("-fsanitize=address" CANCESTRY_HAVE_SANITIZE_ADDRESS)
        if(CANCESTRY_HAVE_SANITIZE_ADDRESS)
            list(APPEND CANCESTRY_SANITIZER_FLAGS -fsanitize=address)
        endif()
    endif()
    if(CANCESTRY_SANITIZER_FLAGS STREQUAL "")
        message(STATUS "CANCESTRY_ENABLE_ASAN is ON but no sanitizer is supported; continuing without")
    else()
        list(APPEND CANCESTRY_SANITIZER_FLAGS -fno-omit-frame-pointer -fno-sanitize-recover=all)
        message(STATUS "Building with sanitizers: ${CANCESTRY_SANITIZER_FLAGS}")
        add_compile_options(${CANCESTRY_SANITIZER_FLAGS})
        add_link_options(${CANCESTRY_SANITIZER_FLAGS})
    endif()
endif()

add_subdirectory(core/event)
add_subdirectory(core/codec)
add_subdirectory(core/recipe)
add_subdirectory(core/fsm)
add_subdirectory(core/hal)
add_subdirectory(core/transport)
add_subdirectory(core/uds)
add_subdirectory(platform)

if(CANCESTRY_BUILD_TESTS)
    enable_testing()
endif()

if(CANCESTRY_BUILD_EXAMPLES AND NOT CMAKE_CROSSCOMPILING)
    # The gateway example registers its own CTest tests (issue #13), so it is
    # added after enable_testing().
    add_subdirectory(examples)
endif()

if(CANCESTRY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
