/*
 * CANcestry - Bare-metal ARM Cortex-M Linker Script.
 *
 * Implements: SW-FR-BM-001 (linker-level zero-alloc enforcement),
 *             SW-FR-BM-008 (bare-metal runtime and memory placement).
 *
 * Enforces zero-allocation at link time:
 *   1. Memory regions map FLASH, RAM, and optional CCMRAM.
 *   2. Core data structures are placed in dedicated RAM sections:
 *      - .cancestry_core  : core FSM instances, event queues, codec tables
 *      - .cancestry_rings : lock-free HAL RX/TX ring buffers
 *      - .cancestry_ram   : general static platform state
 *   3. All heap sections and standard C library allocation entry points
 *      (malloc, free, realloc, calloc, sbrk, _sbrk) are stripped in /DISCARD/.
 *   4. Linker-level assertions immediately halt linking with an error if
 *      any allocator symbol is defined or pulled in by standard libraries.
 */

ENTRY(Reset_Handler)

/* Highest address of the user mode stack */
_estack = ORIGIN(RAM) + LENGTH(RAM); /* end of "RAM" Ram type memory */

_Min_Leftover_RAM = 0x400; /* Minimum 1KB stack space */

MEMORY
{
    FLASH (rx)      : ORIGIN = 0x08000000, LENGTH = 512K
    RAM (xrw)       : ORIGIN = 0x20000000, LENGTH = 96K
    CCMRAM (rw)     : ORIGIN = 0x10000000, LENGTH = 32K
}

SECTIONS
{
    /* The vector table goes to the start of Flash */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    /* Program code and read-only data */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* Dedicated section for core data structures (SW-FR-BM-008) */
    .cancestry_core :
    {
        . = ALIGN(4);
        _scancestry_core = .;
        *(.cancestry_core)
        *(.cancestry_core*)
        . = ALIGN(4);
        _ecancestry_core = .;
    } > RAM

    /* Dedicated section for HAL lock-free rings (SW-FR-BM-008) */
    .cancestry_rings :
    {
        . = ALIGN(4);
        _scancestry_rings = .;
        *(.cancestry_rings)
        *(.cancestry_rings*)
        . = ALIGN(4);
        _ecancestry_rings = .;
    } > RAM

    /* Initialized data placed in RAM, loaded from Flash */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        *(.cancestry_ram)
        *(.cancestry_ram*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT> FLASH

    /* Uninitialized data in RAM */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > RAM

    /* Stack check: ensure enough RAM is left for stack */
    ._check_stack :
    {
        . = ALIGN(4);
        . = . + _Min_Leftover_RAM;
        . = ALIGN(4);
    } > RAM

    /*
     * Absolute Zero Heap: strip all allocator sections and libc allocation
     * members (SW-FR-BM-001). Any attempt to reference these functions causes
     * an undefined reference error at link time.
     */
    /DISCARD/ :
    {
        *(.heap*)
        *(.text.malloc)
        *(.text.free)
        *(.text.realloc)
        *(.text.calloc)
        *(.text._sbrk)
        *(.text.sbrk)
        *(.text._sbrk_r)
        *libc.a:*malloc*
        *libc.a:*free*
        *libc.a:*realloc*
        *libc.a:*calloc*
        *libc.a:*sbrk*
        *libc_nano.a:*malloc*
        *libc_nano.a:*free*
        *libc_nano.a:*realloc*
        *libc_nano.a:*calloc*
        *libc_nano.a:*sbrk*
        *libg.a:*malloc*
        *libg.a:*free*
        *libg.a:*realloc*
        *libg.a:*calloc*
        *libg.a:*sbrk*
    }
}

/*
 * Linker-Level Assertions: fail the build immediately if any allocator entry
 * point is defined or imported (SW-FR-BM-001).
 */
ASSERT(!DEFINED(malloc), "Linker Error: malloc is forbidden in CANcestry bare-metal build (SW-FR-BM-001)");
ASSERT(!DEFINED(free), "Linker Error: free is forbidden in CANcestry bare-metal build (SW-FR-BM-001)");
ASSERT(!DEFINED(calloc), "Linker Error: calloc is forbidden in CANcestry bare-metal build (SW-FR-BM-001)");
ASSERT(!DEFINED(realloc), "Linker Error: realloc is forbidden in CANcestry bare-metal build (SW-FR-BM-001)");
ASSERT(!DEFINED(_sbrk), "Linker Error: _sbrk is forbidden in CANcestry bare-metal build (SW-FR-BM-001)");
ASSERT(!DEFINED(sbrk), "Linker Error: sbrk is forbidden in CANcestry bare-metal build (SW-FR-BM-001)");
