#!/usr/bin/env bash
#
# CANcestry T2 virtual-bench firmware ELF build (H-08, issue #55).
#
# Builds the T2 off-tree firmware ELF from the tag v1.0.0 firmware sources
# (core/ + platform/cortex_m/, unmodified) plus the T2 driver in this
# directory. The v1.0.0 sources are expected at $CANCESTRY_T2_FW_TAG_DIR
# (the CI job creates it with `git archive --format=tar v1.0.0`); locally a
# checkout of the tag may be passed instead.
#
# Determinism contract (HwAGENTS.md rule 5; docs/hw/t2-bringup-report.md):
#   - -O0 throughout: no inlining, so the Renode cpu symbol hooks on
#     cancestry_watchdog_write_retention_register /
#     cancestry_hardware_set_safe_state / cancestry_watchdog_escalate observe
#     real function entries (bring-up finding F-9);
#   - the build log records the compiler identity, the exact source list and
#     the ELF sha256; the runner copies the log into the runlog evidence.
#
# Usage:
#   CANCESTRY_T2_FW_TAG_DIR=<tag v1.0.0 sources> \
#   CANCESTRY_T2_ELF_OUT=<output elf path> \
#   CANCESTRY_T2_BUILD_LOG=<build log path> \
#   ./build_firmware.sh
#
# The linker script comes from the tag v1.0.0 platform
# (platform/cortex_m/cancestry_baremetal.ld), so the memory map and the
# zero-allocation link-time assertions are the v1.0.0 ones.

set -euo pipefail

FW_TAG_DIR="${CANCESTRY_T2_FW_TAG_DIR:?CANCESTRY_T2_FW_TAG_DIR (tag v1.0.0 sources) is required}"
ELF_OUT="${CANCESTRY_T2_ELF_OUT:?CANCESTRY_T2_ELF_OUT (output ELF path) is required}"
BUILD_LOG="${CANCESTRY_T2_BUILD_LOG:?CANCESTRY_T2_BUILD_LOG (build log path) is required}"

CC="${CANCESTRY_T2_CROSS_CC:-arm-none-eabi-gcc}"
DRIVER_DIR="$(cd "$(dirname "$0")" && pwd)"

# Cortex-M4F (FPU = fpv4-sp-d16 hard float), matching the platform model
# cpuType "cortex-m4f" and the v1.0.0 platform assumptions.
CPU_FLAGS="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard"
CFLAGS="-O0 -g -Wall -Wextra -std=c99 $CPU_FLAGS"

mkdir -p "$(dirname "$ELF_OUT")"

{
    echo "== CANcestry T2 firmware ELF build log (determinism evidence) =="
    echo "date_note: no wall clock in the log; build order is fixed by this script"
    echo "compiler: $($CC --version | head -1)"
    echo "fw_tag_dir: $FW_TAG_DIR"
    echo "elf_out: $ELF_OUT"
    echo "cpu_flags: $CPU_FLAGS"
    echo "cflags: $CFLAGS"
    echo ""

    # Tag v1.0.0 firmware sources (read-only; unmodified).
    FIRMWARE_SOURCES=(
        "$FW_TAG_DIR/core/event/src/fault.c"
        "$FW_TAG_DIR/core/event/src/queue.c"
        "$FW_TAG_DIR/core/event/src/types.c"
        "$FW_TAG_DIR/core/event/src/clock.c"
        "$FW_TAG_DIR/core/event/src/isr_queue.c"
        "$FW_TAG_DIR/core/hal/src/hal.c"
        "$FW_TAG_DIR/core/hal/src/types.c"
        "$FW_TAG_DIR/platform/cortex_m/watchdog.c"
        "$FW_TAG_DIR/platform/cortex_m/hal_stm32.c"
        "$FW_TAG_DIR/platform/cortex_m/startup.c"
        "$FW_TAG_DIR/platform/cortex_m/alloc_stubs.c"
    )
    DRIVER_SOURCES=(
        "$DRIVER_DIR/startup.s"
        "$DRIVER_DIR/main.c"
    )

    echo "-- source list (tag v1.0.0 + T2 driver) --"
    for src in "${FIRMWARE_SOURCES[@]}" "${DRIVER_SOURCES[@]}"; do
        [ -f "$src" ] || { echo "MISSING SOURCE: $src" >&2; exit 1; }
        echo "$src"
    done
    echo ""

    INCLUDES=(
        -I"$FW_TAG_DIR/core/event/include"
        -I"$FW_TAG_DIR/core/hal/include"
        -I"$FW_TAG_DIR/platform/cortex_m"
    )

    echo "-- compile --"
    OBJECTS=()
    for src in "${FIRMWARE_SOURCES[@]}"; do
        obj="$ELF_OUT.$(echo "${src##*/}" | tr . _).o"
        echo "$CC $CFLAGS ${INCLUDES[*]} -c $src -o $obj"
        $CC $CFLAGS "${INCLUDES[@]}" -c "$src" -o "$obj"
        OBJECTS+=("$obj")
    done
    for src in "${DRIVER_SOURCES[@]}"; do
        obj="$ELF_OUT.$(echo "${src##*/}" | tr . _).o"
        echo "$CC $CFLAGS ${INCLUDES[*]} -c $src -o $obj"
        $CC $CFLAGS "${INCLUDES[@]}" -c "$src" -o "$obj"
        OBJECTS+=("$obj")
    done

    echo "-- link --"
    LDSCRIPT="$FW_TAG_DIR/platform/cortex_m/cancestry_baremetal.ld"
    echo "$CC $CPU_FLAGS -T $LDSCRIPT -nostartfiles --specs=nosys.specs -Wl,-Map=$ELF_OUT.map -o $ELF_OUT ${OBJECTS[*]} -lc -lnosys"
    $CC $CPU_FLAGS -T "$LDSCRIPT" -nostartfiles --specs=nosys.specs \
        -Wl,-Map="$ELF_OUT.map" -o "$ELF_OUT" "${OBJECTS[@]}" -lc -lnosys

    echo ""
    echo "-- result --"
    echo "elf: $ELF_OUT"
    echo "elf_sha256: $(sha256sum "$ELF_OUT" | cut -d' ' -f1)"
    if command -v arm-none-eabi-size >/dev/null 2>&1; then
        echo "sizes:"
        arm-none-eabi-size "$ELF_OUT"
    fi
} > "$BUILD_LOG"

echo "T2 firmware ELF built: $ELF_OUT"
echo "build log: $BUILD_LOG"
sha256sum "$ELF_OUT"
