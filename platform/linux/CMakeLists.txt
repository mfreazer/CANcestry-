# CANcestry Linux SocketCAN backend.
#
# Builds only on Linux hosts (where <linux/can.h> is available). The archive
# must be allocation-free; ci/check_no_alloc.py scans it in CI.

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(cancestry_platform_linux STATIC socketcan.c)

    target_include_directories(cancestry_platform_linux
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<INSTALL_INTERFACE:include>)

    target_link_libraries(cancestry_platform_linux PUBLIC cancestry_hal)

    set_target_properties(cancestry_platform_linux PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        OUTPUT_NAME cancestry_platform_linux)
endif()
