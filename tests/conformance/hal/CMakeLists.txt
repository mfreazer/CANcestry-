# CANcestry HAL conformance suite (Phase 6, issue #17).
#
# Each test pins one behavioural contract of the HAL and runs against the
# mock HAL backend, so the contract is verified on the same code path the
# real gateway uses. Requirement ids are cited in each source header.

function(cancestry_add_hal_conformance_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target}
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../unit/support"
                "${CMAKE_SOURCE_DIR}/platform/mock")
    target_link_libraries(${target}
        PRIVATE cancestry_hal
                cancestry_platform_mock
                cancestry_event
                cancestry_codec)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

cancestry_add_hal_conformance_test(cancestry_conformance_hal_ring_bounds
    test_ring_bounds.c)
cancestry_add_hal_conformance_test(cancestry_conformance_hal_timestamp_monotonicity
    test_timestamp_monotonicity.c)
cancestry_add_hal_conformance_test(cancestry_conformance_hal_bus_error_injection
    test_bus_error_injection.c)
cancestry_add_hal_conformance_test(cancestry_conformance_hal_nonblocking
    test_nonblocking.c)
# CAN FD fallback (issue #20, Phase 7): a CAN FD frame on a classic-only
# interface must raise PROTOCOL_UNSUPPORTED and be dropped, never truncated.
cancestry_add_hal_conformance_test(cancestry_conformance_hal_can_fd_fallback
    test_can_fd_fallback.c)
