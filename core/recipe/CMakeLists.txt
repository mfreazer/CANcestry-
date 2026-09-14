# CANcestry portable recipe engine: loader and execution engine.
#
# The runtime library (cancestry_recipe: types, engine) is allocation-free
# and portable to firmware; CI verifies the archive contains no allocator
# references (SYS-NF-002). The loader is kept in a separate static library
# (cancestry_recipe_loader) because loading a recipe file allocates one
# block; the runtime path must not.

add_library(cancestry_recipe STATIC
    src/types.c
    src/engine.c)

target_include_directories(cancestry_recipe
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

# The recipe engine resolves signals and messages through the codec signal
# namespace, encodes send_message frames with the codec encoder, and emits
# generated events onto the event queue.
target_link_libraries(cancestry_recipe PUBLIC cancestry_event cancestry_codec)

set_target_properties(cancestry_recipe PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME cancestry_recipe)

add_library(cancestry_recipe_loader STATIC
    src/loader.c)

target_include_directories(cancestry_recipe_loader
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(cancestry_recipe_loader PUBLIC cancestry_recipe)

set_target_properties(cancestry_recipe_loader PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME cancestry_recipe_loader)
