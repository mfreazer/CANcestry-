# CANcestry FSM loader and runtime unit tests (Phase 4, issue #11).
#
# The loader tests pin schemas/fsm-0.2.0.schema.json field by field; the runtime
# test proves the no-heap rule on the execution path with an allocator tripwire,
# complementing the archive scan CI runs on libcancestry_fsm.

cancestry_add_unit_test(cancestry_test_fsm_loader test_loader.c)
target_link_libraries(cancestry_test_fsm_loader PRIVATE cancestry_fsm
                                                          cancestry_fsm_loader)

cancestry_add_unit_test(cancestry_test_fsm_no_alloc test_no_alloc.c)
target_link_libraries(cancestry_test_fsm_no_alloc PRIVATE cancestry_fsm
                                                             cancestry_fsm_loader)
