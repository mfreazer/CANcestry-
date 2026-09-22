/*
 * CANcestry T2 virtual-bench firmware - Cortex-M4 startup (off-tree, H-08).
 *
 * This file is NOT part of the v1.0.0 firmware tree. It provides the minimal
 * startup the tag v1.0.0 sources (core/ + platform/cortex_m/) require to run
 * as an executable on the STM32G474 virtual bench: a vector table, stack
 * pointer initialization, .data copy / .bss zero, and the call into
 * main(). The v1.0.0 platform ships a linker script and a recovery-boundary
 * source (startup.c) but no vector table, because production firmware is
 * linked against vendor boot code; the T2 off-tree ELF is standalone.
 *
 * Provenance rule (docs/hw/virtual-bench-plan.md section 2): every other
 * object file in this ELF is compiled from the tag v1.0.0 sources without
 * modification; build_firmware.sh records the exact source list and the
 * compiler version in the build log that travels with the run evidence.
 */

    .syntax unified
    .cpu    cortex-m4
    .thumb

/* ----------------------------------------------------------------------- */
/* Vector table.                                                           */
/* Core exceptions 0-15 plus 24 padding entries (the T2 driver installs    */
/* no device IRQs; the IRQ table region would map to zero anyway).         */
/* ----------------------------------------------------------------------- */

    .section .isr_vector, "a", %progbits
    .word _estack            /*     0: initial stack pointer */
    .word Reset_Handler      /*     1: reset */
    .word g_default_irq      /*     2: NMI */
    .word g_default_irq      /*     3: hard fault */
    .word g_default_irq      /*     4: memory management */
    .word g_default_irq      /*     5: bus fault */
    .word g_default_irq      /*     6: usage fault */
    .word 0                  /*     7: reserved */
    .word 0                  /*     8: reserved */
    .word 0                  /*     9: reserved */
    .word 0                  /*    10: reserved */
    .word g_default_irq      /*    11: SVCall */
    .word g_default_irq      /*    12: debug monitor */
    .word 0                  /*    13: reserved */
    .word g_default_irq      /*    14: PendSV */
    .word g_default_irq      /*    15: SysTick */
    .rept 24
    .word g_default_irq      /*    16..39: device IRQs (unused) */
    .endr

    .size g_default_irq, . - g_default_irq

/* ----------------------------------------------------------------------- */
/* Reset handler.                                                          */
/* ----------------------------------------------------------------------- */

    .section .text
    .type Reset_Handler, %function
    .global Reset_Handler
    .thumb_func
Reset_Handler:
    ldr     sp, =_estack

    /* Copy the initialized .data section from Flash to RAM. */
    ldr     r0, =_sdata
    ldr     r1, =_edata
    ldr     r2, =_sidata
    b       .Lcopy_check
.Lcopy_copy:
    ldr     r3, [r2], #4
    str     r3, [r0], #4
.Lcopy_check:
    cmp     r0, r1
    bcc     .Lcopy_copy

    /* Zero the .bss section. */
    ldr     r0, =_sbss
    ldr     r1, =_ebss
    movs    r2, #0
    b       .Lzero_check
.Lzero_zero:
    str     r2, [r0], #4
.Lzero_check:
    cmp     r0, r1
    bcc     .Lzero_zero

    bl      main
    b       .

    .size Reset_Handler, . - Reset_Handler

    /* Default interrupt handler (park). Lives in .text: it must NOT be
     * emitted inside .isr_vector, or its body would shift the vector
     * table and the initial SP word would no longer sit at the flash
     * origin (the CPU would load SP = 0xBE00 on reset).
     * Bring-up finding F-14 (issue #55). */
    .thumb_func
    .type g_default_irq, %function
    .global g_default_irq

g_default_irq:
    b g_default_irq

    .size g_default_irq, . - g_default_irq

    .end
