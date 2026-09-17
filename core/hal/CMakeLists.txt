# CANcestry Hardware Abstraction Layer: hardware-agnostic core.
#
# The library is warning-clean, allocation-free, and independent of any
# platform-specific headers; platform backends (Linux SocketCAN, mock)
# implement the cancestry_hal_backend vtable defined in hal.h.

add_library(cancestry_hal STATIC
    src/types.c
    src/hal.c)

target_include_directories(cancestry_hal
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(cancestry_hal PUBLIC cancestry_event)

set_target_properties(cancestry_hal PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME cancestry_hal)
