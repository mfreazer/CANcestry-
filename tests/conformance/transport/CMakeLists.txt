# CANcestry ISO-TP transport conformance suite (Phase 10, issue #26).
#
# Each suite defines one normative behavioural contract of the ISO-TP engine
# (docs/software/SwRS.md section 14.1) and runs against the shipping engine,
# so the contract is verified on the same code path firmware and simulation
# use. Requirement ids are cited in the file headers and mapped in
# core/transport/README.md and docs/trace/traceability.csv.

function(cancestry_add_tp_conformance_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target}
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../../unit/support")
    target_link_libraries(${target}
        PRIVATE cancestry_transport
                cancestry_event)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

cancestry_add_tp_conformance_test(cancestry_conformance_tp_iso_tp_reassembly
    test_iso_tp_reassembly.c)
cancestry_add_tp_conformance_test(cancestry_conformance_tp_iso_tp_segmentation
    test_iso_tp_segmentation.c)
