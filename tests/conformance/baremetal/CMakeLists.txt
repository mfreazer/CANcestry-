# CANcestry Bare-Metal conformance suite (Phase 11, issue #28).
#
# Each test pins one behavioural contract of the bare-metal Cortex-M port
# (docs/software/SwRS.md section 15):
#   - sub-50µs latency from CAN RX interrupt to FSM event processing (SW-FR-BM-007)
#   - watchdog fail-safe reset recovery to 0-torque state (SW-FR-BM-005, SW-FR-BM-006)
#   - linker-level zero-allocation enforcement (SW-FR-BM-001)

function(cancestry_add_bm_conformance_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target}
        PRIVATE "${CMAKE_SOURCE_DIR}/platform/cortex_m"
                "${CMAKE_SOURCE_DIR}/tests/unit/support"
                "${CMAKE_SOURCE_DIR}/tests/conformance/support")
    target_link_libraries(${target}
        PRIVATE cancestry_platform_cortex_m
                cancestry_fsm
                cancestry_fsm_loader
                cancestry_codec
                cancestry_codec_loader
                cancestry_recipe
                cancestry_recipe_loader
                cancestry_hal
                cancestry_event)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

cancestry_add_bm_conformance_test(cancestry_conformance_bm_latency
    test_baremetal_latency.c)
cancestry_add_bm_conformance_test(cancestry_conformance_bm_watchdog
    test_watchdog_fail_safe.c)
cancestry_add_bm_conformance_test(cancestry_conformance_bm_hard_fault_escalation
    test_hard_fault_escalation.c)
cancestry_add_bm_conformance_test(cancestry_conformance_bm_persist_before_latch
    test_persist_before_latch.c)
cancestry_add_bm_conformance_test(cancestry_conformance_bm_terminal_state_refusal
    test_terminal_state_refusal.c)
cancestry_add_bm_conformance_test(cancestry_conformance_bm_reset_reason_readability
    test_reset_reason_readability.c)
if(UNIX)
    cancestry_add_bm_conformance_test(cancestry_conformance_bm_fallback_spin
        test_fallback_spin.c)
endif()
cancestry_add_bm_conformance_test(cancestry_conformance_bm_zero_alloc
    test_baremetal_zero_alloc.c)
target_compile_definitions(cancestry_conformance_bm_zero_alloc
    PRIVATE CANCESTRY_BAREMETAL_LD_PATH="${CMAKE_SOURCE_DIR}/platform/cortex_m/cancestry_baremetal.ld")
