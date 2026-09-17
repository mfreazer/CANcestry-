# CANcestry host unit tests.
#
# Every test declares the requirements it verifies in a comment header so the
# traceability CSV can be checked against the sources.

set(CANCESTRY_TEST_SUPPORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/unit/support")

function(cancestry_add_unit_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE "${CANCESTRY_TEST_SUPPORT_DIR}")
    target_link_libraries(${target} PRIVATE cancestry_event cancestry_codec
                                              cancestry_codec_loader
                                              cancestry_recipe
                                              cancestry_recipe_loader)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_subdirectory(unit/core/event)
add_subdirectory(unit/core/codec)
add_subdirectory(unit/core/recipe)
add_subdirectory(unit/core/fsm)
add_subdirectory(conformance/fsm)
add_subdirectory(conformance/hal)
# CAN FD conformance (issue #20, Phase 7).
add_subdirectory(conformance/codec)

# The runtime cores must not allocate. This is verified on the built archives,
# which is a stronger and more portable check than intercepting malloc at
# runtime. The codec and recipe loaders are intentionally excluded: loading
# may allocate (SYS-NF-002); decode/encode/namespace and the recipe engine
# must not.
find_program(CANCESTRY_PYTHON NAMES python3 python)
if(CANCESTRY_PYTHON AND NOT CMAKE_CROSSCOMPILING)
    # Every committed JSON Schema must parse and be self-consistent
    # (issue #13: schemas/fsm-0.3.0.schema.json created and valid).
    add_test(NAME cancestry_schemas_valid
        COMMAND ${CANCESTRY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../ci/check_schemas_valid.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/../schemas")
    # Traceability completeness (Test strategy.md section 10): the CSV resolves,
    # no requirement is silently absent, and the deferred ledger is explicit.
    add_test(NAME cancestry_traceability_consistent
        COMMAND ${CANCESTRY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../ci/check_traceability.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/..")
    # Realizes the archive-scan half of EVENT-NO-ALLOC-001 (SYS-NF-002).
    add_test(NAME cancestry_event_no_malloc_symbols
        COMMAND ${CANCESTRY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../ci/check_no_alloc.py"
                $<TARGET_FILE:cancestry_event>)
    # Realizes CODEC-NO-ALLOC-001 (SYS-NF-002).
    add_test(NAME cancestry_codec_no_malloc_symbols
        COMMAND ${CANCESTRY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../ci/check_no_alloc.py"
                $<TARGET_FILE:cancestry_codec>)
    # Realizes RECIPE-NO-ALLOC-001 (SYS-NF-002).
    add_test(NAME cancestry_recipe_no_malloc_symbols
        COMMAND ${CANCESTRY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../ci/check_no_alloc.py"
                $<TARGET_FILE:cancestry_recipe>)
    # The FSM runtime path must not allocate either (issue #11 constraint:
    # cancestry_fsm_engine_process_event and cancestry_fsm_engine_tick may not).
    # Realizes the archive-scan half of FSM-NO-ALLOC-001 (SYS-NF-002).
    add_test(NAME cancestry_fsm_no_malloc_symbols
        COMMAND ${CANCESTRY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../ci/check_no_alloc.py"
                $<TARGET_FILE:cancestry_fsm>)
    # The HAL core must be zero-alloc (issue #17 constraint 1, SYS-NF-002).
    add_test(NAME cancestry_hal_no_malloc_symbols
        COMMAND ${CANCESTRY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../ci/check_no_alloc.py"
                $<TARGET_FILE:cancestry_hal>)
    # The mock platform used in CI must be zero-alloc.
    add_test(NAME cancestry_platform_mock_no_malloc_symbols
        COMMAND ${CANCESTRY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../ci/check_no_alloc.py"
                $<TARGET_FILE:cancestry_platform_mock>)
endif()

# The Linux SocketCAN backend is only built on Linux; check it there.
if(CANCESTRY_PYTHON AND NOT CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME cancestry_platform_linux_no_malloc_symbols
        COMMAND ${CANCESTRY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../ci/check_no_alloc.py"
                $<TARGET_FILE:cancestry_platform_linux>)
endif()
