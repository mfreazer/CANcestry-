# CANcestry mock HAL backend (for tests and CI).
#
# The mock backend is a deterministic, zero-alloc in-process simulation of a
# CAN bus. It is linked into the conformance suite and is never used in
# production.

add_library(cancestry_platform_mock STATIC mock_hal.c)

target_include_directories(cancestry_platform_mock
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(cancestry_platform_mock PUBLIC cancestry_hal)

set_target_properties(cancestry_platform_mock PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME cancestry_platform_mock)
