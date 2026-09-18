# CANcestry ISO-TP transport engine (Phase 10, issue #26).
#
# The runtime library (cancestry_transport: types + engine) is allocation-free
# and portable to firmware; CI verifies the archive contains no allocator
# references (SYS-NF-002, SW-FR-TP-001). There is no loader: the engine has no
# file format; timers, block size and STmin are bound through the C config
# struct at init time.

add_library(cancestry_transport STATIC
    src/types.c
    src/engine.c)

target_include_directories(cancestry_transport
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(cancestry_transport PUBLIC cancestry_event)

set_target_properties(cancestry_transport PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME cancestry_transport)
