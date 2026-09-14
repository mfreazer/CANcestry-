# CANcestry portable codec engine: loader, decoder, encoder, namespace.
#
# The runtime library (cancestry_codec: decode, encode, namespace) is
# allocation-free and portable to firmware; CI verifies the archive contains
# no allocator references. The loader is kept in a separate static library
# (cancestry_codec_loader) because loading a codec map allocates one block
# (SYS-NF-002: loading may allocate, the runtime path must not).

add_library(cancestry_codec STATIC
    src/types.c
    src/decoder.c
    src/encoder.c
    src/namespace.c)

target_include_directories(cancestry_codec
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

# The codec uses the event model's tagged value type (cancestry_value_t) in
# its decoded output; this is a header-only dependency on the event core.
target_link_libraries(cancestry_codec PUBLIC cancestry_event)

set_target_properties(cancestry_codec PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME cancestry_codec)

add_library(cancestry_codec_loader STATIC
    src/loader.c)

target_include_directories(cancestry_codec_loader
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(cancestry_codec_loader PUBLIC cancestry_codec)

set_target_properties(cancestry_codec_loader PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME cancestry_codec_loader)
