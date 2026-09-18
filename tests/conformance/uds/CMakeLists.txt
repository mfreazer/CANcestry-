# CANcestry UDS conformance suite (Phase 10, issue #26).
#
# Each suite defines one normative behavioural contract of the UDS server
# (docs/software/SwRS.md section 14.2) and runs against the shipping loader,
# server, codec namespace, shared signal store, governor stub and (for the
# integration suite) the ISO-TP transport engine, so the contract is verified
# on the same code path firmware and simulation use. Requirement ids are
# cited in the file headers and mapped in core/uds/README.md and
# docs/trace/traceability.csv.

function(cancestry_add_uds_conformance_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target}
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../../unit/support")
    target_link_libraries(${target}
        PRIVATE cancestry_uds
                cancestry_uds_loader
                cancestry_transport
                cancestry_codec
                cancestry_codec_loader
                cancestry_recipe
                cancestry_event)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

cancestry_add_uds_conformance_test(cancestry_conformance_uds_services
    test_uds_services.c)
cancestry_add_uds_conformance_test(cancestry_conformance_uds_governor_integration
    test_uds_governor_integration.c)
