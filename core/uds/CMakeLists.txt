# CANcestry UDS server (Phase 10, issue #26).
#
# The runtime library (cancestry_uds: types + services) is allocation-free and
# portable to firmware; CI verifies the archive contains no allocator
# references (SYS-NF-002, SW-FR-UDS-001). The configuration loader may
# allocate its arena, so it is a separate static library
# (cancestry_uds_loader) exactly like the codec, recipe and FSM loaders.

add_library(cancestry_uds STATIC
    src/types.c
    src/services.c)

target_include_directories(cancestry_uds
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

# The server resolves DID signal mappings through the codec namespace and
# mirrors approved writes into the shared signal value store owned by the
# recipe engine (SW-FR-UDS-008).
target_link_libraries(cancestry_uds PUBLIC cancestry_event cancestry_codec cancestry_recipe)

set_target_properties(cancestry_uds PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME cancestry_uds)

add_library(cancestry_uds_loader STATIC
    src/loader.c)

target_include_directories(cancestry_uds_loader
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(cancestry_uds_loader PUBLIC cancestry_uds)

set_target_properties(cancestry_uds_loader PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME cancestry_uds_loader)
