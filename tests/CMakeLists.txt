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

# The runtime cores must not allocate. This is verified on the built archives,
# which is a stronger and more portable check than intercepting malloc at
# runtime. The codec and recipe loaders are intentionally excluded: loading
# may allocate (SYS-NF-002); decode/encode/namespace and the recipe engine
# must not.
find_program(CANCESTRY_PYTHON NAMES python3 python)
if(CANCESTRY_PYTHON AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME cancestry_event_no_malloc_symbols
        COMMAND ${CANCESTRY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../ci/check_no_alloc.py"
                $<TARGET_FILE:cancestry_event>)
    add_test(NAME cancestry_codec_no_malloc_symbols
        COMMAND ${CANCESTRY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../ci/check_no_alloc.py"
                $<TARGET_FILE:cancestry_codec>)
    add_test(NAME cancestry_recipe_no_malloc_symbols
        COMMAND ${CANCESTRY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../ci/check_no_alloc.py"
                $<TARGET_FILE:cancestry_recipe>)
    # The FSM runtime path must not allocate either (issue #11 constraint:
    # cancestry_fsm_engine_process_event and cancestry_fsm_engine_tick may not).
    add_test(NAME cancestry_fsm_no_malloc_symbols
        COMMAND ${CANCESTRY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../ci/check_no_alloc.py"
                $<TARGET_FILE:cancestry_fsm>)
endif()
