/*
 * CANcestry Bare-Metal Conformance: Linker-Level Zero-Allocation Enforcement.
 *
 * Contract under test (docs/software/SwRS.md section 15):
 *   - SW-FR-BM-001: linker-level zero-allocation enforcement. The bare-metal
 *     linker script (cancestry_baremetal.ld) and toolchain stubs shall exclude
 *     standard library heap allocation functions (malloc, free, realloc,
 *     calloc, _sbrk, sbrk), causing a linker undefined reference error if any
 *     dynamic allocation is attempted.
 *
 * Test ids: BM-ALLOC-001.
 */

#include "alloc_stubs.h"
#include "cancestry_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void test_zero_alloc_runtime_invariants(void)
{
    static uint8_t s_buffer[64];
    uint8_t stack_buffer[64];

    CANCESSTRY_TEST_CASE("BM-ALLOC-001: runtime zero-alloc check and tripwire");

    /* Core runtime structures must never be flagged as heap allocated */
    CANCESSTRY_TEST_CHECK(!cancestry_is_heap_allocated(s_buffer));
    CANCESSTRY_TEST_CHECK(!cancestry_is_heap_allocated(stack_buffer));
    CANCESSTRY_TEST_CHECK(!cancestry_is_heap_allocated(NULL));
}

static void test_linker_script_zero_alloc_rejection(void)
{
    char command[4096];
    int ret;

    CANCESSTRY_TEST_CASE("BM-ALLOC-001: linker script undefined reference rejection");

    /*
     * Run the automated linker verification:
     * 1. A clean bare-metal translation unit must link cleanly with
     *    cancestry_baremetal.ld.
     * 2. Attempting to link calls to malloc, free, calloc, realloc, or _sbrk
     *    must be strictly rejected by ld with an undefined reference error.
     */
    (void)snprintf(command, sizeof(command),
                   "python3 -c '\n"
                   "import subprocess, tempfile, os, sys\n"
                   "ld_script = os.path.abspath(\"%s\")\n"
                   "if not os.path.exists(ld_script):\n"
                   "    sys.exit(3)\n"
                   "with tempfile.TemporaryDirectory() as tmpdir:\n"
                   "    clean_src = os.path.join(tmpdir, \"clean.c\")\n"
                   "    with open(clean_src, \"w\") as f:\n"
                   "        f.write(\"void Reset_Handler(void) { while(1); }\\n"
                   "__attribute__((section(\\\".isr_vector\\\"))) void (* const g_pfnVectors[])(void) = { (void (*)(void))0x20018000, Reset_Handler };\\n\")\n"
                   "    clean_obj = os.path.join(tmpdir, \"clean.o\")\n"
                   "    clean_elf = os.path.join(tmpdir, \"clean.elf\")\n"
                   "    subprocess.run([\"gcc\", \"-c\", clean_src, \"-o\", clean_obj], check=True)\n"
                   "    r = subprocess.run([\"ld\", \"-T\", ld_script, clean_obj, \"-o\", clean_elf], capture_output=True)\n"
                   "    if r.returncode != 0:\n"
                   "        sys.exit(1)\n"
                   "    for sym in [\"malloc\", \"free\", \"calloc\", \"realloc\", \"_sbrk\"]:\n"
                   "        alloc_src = os.path.join(tmpdir, f\"call_{sym}.c\")\n"
                   "        with open(alloc_src, \"w\") as f:\n"
                   "            f.write(f\"extern void *{sym}(void); void Reset_Handler(void) {{ (void){sym}(); while(1); }}\\n"
                   "__attribute__((section(\\\".isr_vector\\\"))) void (* const g_pfnVectors[])(void) = {{ (void (*)(void))0x20018000, Reset_Handler }};\\n\")\n"
                   "        alloc_obj = os.path.join(tmpdir, f\"call_{sym}.o\")\n"
                   "        alloc_elf = os.path.join(tmpdir, f\"call_{sym}.elf\")\n"
                   "        subprocess.run([\"gcc\", \"-c\", alloc_src, \"-o\", alloc_obj], check=True)\n"
                   "        r = subprocess.run([\"ld\", \"-T\", ld_script, alloc_obj, \"-o\", alloc_elf], capture_output=True)\n"
                   "        if r.returncode == 0:\n"
                   "            sys.exit(2)\n"
                   "'",
                   CANCESTRY_BAREMETAL_LD_PATH);

    ret = system(command);
    CANCESSTRY_TEST_CHECK_U64(ret, 0u);
}

int main(void)
{
    CANCESSTRY_TEST_SUITE_BEGIN("Linker Zero-Allocation Conformance");

    test_zero_alloc_runtime_invariants();
    test_linker_script_zero_alloc_rejection();

    return CANCESSTRY_TEST_SUITE_END();
}
