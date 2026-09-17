# CANcestry real CAN gateway example (Phase 6, issue #17).
#
# Builds only on Linux hosts where the SocketCAN backend is available.
# Links against cancestry_platform_linux and exercises the full RX ->
# Decode -> FSM/Recipe -> Encode -> TX loop over vcan. The binary returns
# SKIP (exit 0) when it cannot open the vcan interfaces so it is safe to
# register as a CTest even in environments where vcan is unavailable.

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    add_executable(cancestry_gateway_real main.c)

    target_include_directories(cancestry_gateway_real PRIVATE
        "${CMAKE_SOURCE_DIR}/platform/linux")

    target_link_libraries(cancestry_gateway_real PRIVATE
        cancestry_event
        cancestry_codec
        cancestry_codec_loader
        cancestry_recipe
        cancestry_recipe_loader
        cancestry_fsm
        cancestry_fsm_loader
        cancestry_hal
        cancestry_platform_linux)

    set_target_properties(cancestry_gateway_real PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)

    if(NOT MSVC)
        target_compile_options(cancestry_gateway_real PRIVATE -Wconversion)
    endif()

    # Zero-allocation check for the example's object file, matching
    # examples/gateway/ (SYS-NF-002).
    find_program(CANCESTRY_PYTHON NAMES python3 python)
    if(CANCESTRY_PYTHON AND NOT CMAKE_CONFIGURATION_TYPES)
        add_test(NAME cancestry_gateway_real_no_alloc_symbols
            COMMAND ${CANCESTRY_PYTHON} "${CMAKE_SOURCE_DIR}/ci/check_no_alloc.py"
                    "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/cancestry_gateway_real.dir/main.c.o")
    endif()

    add_test(NAME cancestry_gateway_real_loop COMMAND cancestry_gateway_real)
    set_tests_properties(cancestry_gateway_real_loop PROPERTIES
        ENVIRONMENT "SKIP_IF_NO_VCAN=1")
endif()
