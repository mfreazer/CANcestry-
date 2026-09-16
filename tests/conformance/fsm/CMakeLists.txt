# CANcestry FSM conformance suite (Phase 4, issue #11).
#
# Each suite defines one normative behavioural contract of the FSM runtime and
# runs against the shipping loader and engine, so the contract is verified on the
# same code path firmware and simulation use. Requirement ids are cited in the
# file headers and mapped in core/fsm/README.md.

function(cancestry_add_conformance_test target)
    add_executable(${target} ${ARGN})
    target_include_directories(${target}
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../support"
                "${CMAKE_CURRENT_SOURCE_DIR}/../../unit/support")
    target_link_libraries(${target}
        PRIVATE cancestry_fsm
                cancestry_fsm_loader
                cancestry_event
                cancestry_codec
                cancestry_codec_loader)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

cancestry_add_conformance_test(cancestry_conformance_fsm_instance_lifecycle
    test_instance_lifecycle.c)
cancestry_add_conformance_test(cancestry_conformance_fsm_event_order test_event_order.c)
cancestry_add_conformance_test(cancestry_conformance_fsm_timer_semantics
    test_timer_semantics.c)
cancestry_add_conformance_test(cancestry_conformance_fsm_expression_evaluation
    test_expression_evaluation.c)
cancestry_add_conformance_test(cancestry_conformance_fsm_transition_precedence
    test_transition_precedence.c)
cancestry_add_conformance_test(cancestry_conformance_fsm_capability_enforcement
    test_capability_enforcement.c)
cancestry_add_conformance_test(cancestry_conformance_fsm_queue_overflow test_queue_overflow.c)
