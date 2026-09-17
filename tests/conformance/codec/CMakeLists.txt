# CANcestry codec conformance suite (Phase 7, issue #20).
#
# These tests pin the CAN FD contract of the codec engine: what a 64-byte
# payload may do, and what a classic 8-byte buffer must never be asked to do.
# Requirement ids are cited in each source header.

function(cancestry_add_codec_conformance_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target}
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../unit/support")
    target_link_libraries(${target}
        PRIVATE cancestry_codec
                cancestry_codec_loader
                cancestry_event)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

cancestry_add_codec_conformance_test(cancestry_conformance_codec_can_fd_payload_bounds
    test_can_fd_payload_bounds.c)
cancestry_add_codec_conformance_test(cancestry_conformance_codec_can_fd_schema
    test_can_fd_schema.c)
