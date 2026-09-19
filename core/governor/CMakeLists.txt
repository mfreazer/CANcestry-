# CANcestry BMS safety governor (Phase 12, issue #30).
#
# The governor is a separate, allocation-free static library.  It depends only
# on the C99 integer types so it can be linked into a UDS/CAN integration or
# reviewed/formally verified in isolation.

add_library(cancestry_governor STATIC
    bms_governor.c)

target_include_directories(cancestry_governor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

set_target_properties(cancestry_governor PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME cancestry_governor)
