# CANcestry portable event model: types, clock and bounded queue.
#
# The library is warning-clean and allocation-free, so it is equally usable in
# firmware and in host simulation.

add_library(cancestry_event STATIC
    src/clock.c
    src/fault.c
    src/isr_queue.c
    src/queue.c
    src/types.c)

target_include_directories(cancestry_event
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

set_target_properties(cancestry_event PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME cancestry_event)
