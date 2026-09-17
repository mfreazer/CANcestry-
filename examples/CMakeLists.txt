# CANcestry examples: host programs over the portable core.
#
# Examples are first-class CI artifacts: they are registered as CTest tests so
# the PR pipeline exercises the full integration paths they demonstrate.

add_subdirectory(gateway)

# The real SocketCAN gateway is Linux-only (issue #17, Phase 6).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(gateway_real)
endif()
