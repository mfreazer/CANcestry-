# CANcestry portable FSM runtime: engine, expression evaluator and loader.
#
# The runtime library (cancestry_fsm: engine + expression evaluator) is
# allocation-free and portable to firmware; CI verifies the archive contains no
# allocator references (SYS-NF-002, SW-FR-FSM constraint "no heap allocation in
# the runtime path"). The loader is a separate static library (cancestry_fsm_loader)
# because loading allocates its arena; the split mirrors core/recipe.

add_library(cancestry_fsm STATIC
    src/engine.c
    src/expression.c)

target_include_directories(cancestry_fsm
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

# The FSM engine resolves CAN messages for send_message through the codec
# namespace and encoder (SW-FR-FSM-040 needs the message's CAN id to check the
# package TX allowlist), and emits generated events on the event queue.
target_link_libraries(cancestry_fsm PUBLIC cancestry_event cancestry_codec)

set_target_properties(cancestry_fsm PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME cancestry_fsm)

add_library(cancestry_fsm_loader STATIC
    src/loader.c)

target_include_directories(cancestry_fsm_loader
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(cancestry_fsm_loader PUBLIC cancestry_fsm)

set_target_properties(cancestry_fsm_loader PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME cancestry_fsm_loader)
