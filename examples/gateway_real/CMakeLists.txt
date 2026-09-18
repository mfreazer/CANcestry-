# CANcestry real CAN gateway example (Phase 6, issue #17).
#
# Builds only on Linux hosts where the SocketCAN backend is available.
# Links against cancestry_platform_linux and exercises the full RX ->
# Decode -> FSM/Recipe -> Encode -> TX loop over vcan. The binary returns
# SKIP (exit 0) when it cannot open the vcan interfaces so it is safe to
# register as a CTest even in environments where vcan is unavailable.
#
# Phase 8 (issue #22) adds a *static* variant: CMake compiles the committed
# gateway_fsm.yaml / gateway_codec.yaml into C headers with tools/yaml2c and
# builds main_static.c against the generated const static definitions. That
# binary needs neither the loaders nor an allocation and is gated with
# ci/check_no_alloc.py (SW-FR-TOOL-003, SW-FR-TOOL-004).

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    find_program(CANCESTRY_PYTHON NAMES python3 python)

    # ------------------------------------------------------------------
    # Static codegen demo (Phase 8, issue #22): yaml2c at build time.
    # ------------------------------------------------------------------
    set(CANCESTRY_YAML2C "${CMAKE_SOURCE_DIR}/tools/yaml2c/yaml2c.py")
    set(CANCESTRY_STATIC_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${CANCESTRY_STATIC_GEN_DIR}")

    if(CANCESTRY_PYTHON)
        add_custom_command(
            OUTPUT "${CANCESTRY_STATIC_GEN_DIR}/gateway_fsm_static.h"
            COMMAND ${CANCESTRY_PYTHON} "${CANCESTRY_YAML2C}"
                    --schema-dir "${CMAKE_SOURCE_DIR}/schemas"
                    --kind fsm
                    --set-symbol gateway_static
                    --guard GATEWAY_FSM_STATIC_H
                    -o "${CANCESTRY_STATIC_GEN_DIR}/gateway_fsm_static.h"
                    "${CMAKE_CURRENT_SOURCE_DIR}/gateway_fsm.yaml"
            DEPENDS "${CANCESTRY_YAML2C}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/gateway_fsm.yaml"
                    "${CMAKE_SOURCE_DIR}/schemas/fsm-0.3.0.schema.json"
            COMMENT "yaml2c: gateway_fsm.yaml -> gateway_fsm_static.h"
            VERBATIM)
        add_custom_command(
            OUTPUT "${CANCESTRY_STATIC_GEN_DIR}/gateway_codec_static.h"
            COMMAND ${CANCESTRY_PYTHON} "${CANCESTRY_YAML2C}"
                    --schema-dir "${CMAKE_SOURCE_DIR}/schemas"
                    --kind codec
                    --set-symbol gateway_static
                    --guard GATEWAY_CODEC_STATIC_H
                    -o "${CANCESTRY_STATIC_GEN_DIR}/gateway_codec_static.h"
                    "${CMAKE_CURRENT_SOURCE_DIR}/gateway_codec.yaml"
            DEPENDS "${CANCESTRY_YAML2C}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/gateway_codec.yaml"
                    "${CMAKE_SOURCE_DIR}/schemas/codec-map-0.3.0.schema.json"
            COMMENT "yaml2c: gateway_codec.yaml -> gateway_codec_static.h"
            VERBATIM)
    endif()

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
    if(CANCESTRY_PYTHON AND NOT CMAKE_CONFIGURATION_TYPES)
        add_test(NAME cancestry_gateway_real_no_alloc_symbols
            COMMAND ${CANCESTRY_PYTHON} "${CMAKE_SOURCE_DIR}/ci/check_no_alloc.py"
                    "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/cancestry_gateway_real.dir/main.c.o")
    endif()

    add_test(NAME cancestry_gateway_real_loop COMMAND cancestry_gateway_real)
    set_tests_properties(cancestry_gateway_real_loop PROPERTIES
        ENVIRONMENT "SKIP_IF_NO_VCAN=1")

    # ------------------------------------------------------------------
    # The static binary: generated definitions only, loaders not linked.
    # ------------------------------------------------------------------
    if(CANCESTRY_PYTHON)
        add_executable(cancestry_gateway_real_static main_static.c
                       "${CANCESTRY_STATIC_GEN_DIR}/gateway_fsm_static.h"
                       "${CANCESTRY_STATIC_GEN_DIR}/gateway_codec_static.h")

        target_include_directories(cancestry_gateway_real_static PRIVATE
            "${CANCESTRY_STATIC_GEN_DIR}")

        target_link_libraries(cancestry_gateway_real_static PRIVATE
            cancestry_event
            cancestry_codec
            cancestry_fsm)

        set_target_properties(cancestry_gateway_real_static PROPERTIES
            C_STANDARD 99
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF)

        if(NOT MSVC)
            target_compile_options(cancestry_gateway_real_static PRIVATE
                -Wconversion)
        endif()

        # The generated header must not drag heap allocation into the
        # example binary (issue #22 acceptance; SW-FR-TOOL-003).
        add_test(NAME cancestry_gateway_real_static_no_alloc_symbols
            COMMAND ${CANCESTRY_PYTHON} "${CMAKE_SOURCE_DIR}/ci/check_no_alloc.py"
                    "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/cancestry_gateway_real_static.dir/main_static.c.o"
                    $<TARGET_FILE:cancestry_event>
                    $<TARGET_FILE:cancestry_fsm>)
        # Codegen demonstration end to end (YAML2C-STATIC-LOOP-001).
        add_test(NAME cancestry_gateway_real_static_loop
            COMMAND cancestry_gateway_real_static)
    endif()
endif()
