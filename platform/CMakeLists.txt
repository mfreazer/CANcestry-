# CANcestry platform backends.
#
# The core HAL (core/hal) is platform-independent; this directory contains
# concrete backends (Linux SocketCAN for production, mock for tests/CI).

add_subdirectory(mock)
add_subdirectory(cortex_m)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(linux)
endif()
