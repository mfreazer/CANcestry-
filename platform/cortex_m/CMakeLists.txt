# CANcestry ARM Cortex-M bare-metal platform backend (Phase 11, issue #28).
#
# Provides register-level STM32 HAL, IWDG watchdog, and zero-allocation
# bare-metal stubs. Compiles cleanly on both host (for HIL / QEMU tests)
# and cross-compilation targets (-mcpu=cortex-m4 -mthumb).

add_library(cancestry_platform_cortex_m STATIC
    hal_stm32.c
    watchdog.c
    alloc_stubs.c)

target_include_directories(cancestry_platform_cortex_m
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(cancestry_platform_cortex_m
    PUBLIC
        cancestry_hal
        cancestry_event)

set_target_properties(cancestry_platform_cortex_m PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME cancestry_platform_cortex_m)
